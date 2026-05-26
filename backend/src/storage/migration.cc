#include "storage/migration.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace mmcr::storage {

namespace {

struct MigrationFile {
    std::string version;
    std::filesystem::path path;
};

auto ReadFile(const std::filesystem::path& path) -> util::StatusOr<std::string> {
    std::ifstream input(path);
    if (!input.is_open()) {
        return util::Status::NotFound("failed to open migration file: " + path.string());
    }

    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

auto ParseVersion(const std::filesystem::path& path) -> util::StatusOr<std::string> {
    const auto stem = path.stem().string();
    const auto separator = stem.find('_');
    if (separator == std::string::npos || separator == 0) {
        return util::Status::InvalidArgument(
            "migration filename must start with an ordered prefix: " + path.filename().string());
    }

    return stem;
}

}  // namespace

MigrationRunner::MigrationRunner(Database* database) : database_(database) {}

util::Status MigrationRunner::ApplyDirectory(const std::filesystem::path& path) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    if (!std::filesystem::exists(path)) {
        return util::Status::NotFound("migration directory does not exist: " + path.string());
    }

    auto status = EnsureMetadataTable();
    if (!status.ok()) {
        return status;
    }

    std::vector<MigrationFile> migrations;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sql") {
            continue;
        }

        auto version = ParseVersion(entry.path());
        if (!version.ok()) {
            return version.status();
        }

        migrations.push_back({version.value(), entry.path()});
    }

    std::sort(migrations.begin(), migrations.end(), [](const auto& left, const auto& right) {
        return left.version < right.version;
    });

    auto applied = database_->Prepare(
        "SELECT 1 FROM schema_migrations WHERE version = ?1 LIMIT 1;");
    if (!applied.ok()) {
        return applied.status();
    }

    auto insert = database_->Prepare(
        "INSERT INTO schema_migrations(version, name) VALUES (?1, ?2);");
    if (!insert.ok()) {
        return insert.status();
    }

    for (const auto& migration : migrations) {
        auto& applied_statement = applied.value();
        status = applied_statement.BindText(1, migration.version);
        if (!status.ok()) {
            return status;
        }

        auto step = applied_statement.Step();
        if (!step.ok()) {
            return step.status();
        }

        const bool already_applied = step.value() == Statement::StepResult::kRow;
        status = applied_statement.Reset();
        if (!status.ok()) {
            return status;
        }

        if (already_applied) {
            continue;
        }

        auto transaction = database_->BeginTransaction();
        if (!transaction.ok()) {
            return transaction.status();
        }

        auto contents = ReadFile(migration.path);
        if (!contents.ok()) {
            return contents.status();
        }

        status = database_->Execute(contents.value());
        if (!status.ok()) {
            return status;
        }

        auto& insert_statement = insert.value();
        status = insert_statement.BindText(1, migration.version);
        if (!status.ok()) {
            return status;
        }
        status = insert_statement.BindText(2, migration.path.filename().string());
        if (!status.ok()) {
            return status;
        }

        step = insert_statement.Step();
        if (!step.ok()) {
            return step.status();
        }
        status = insert_statement.Reset();
        if (!status.ok()) {
            return status;
        }

        status = transaction.value().Commit();
        if (!status.ok()) {
            return status;
        }
    }

    return util::Status::Ok();
}

util::Status MigrationRunner::EnsureMetadataTable() {
    return database_->Execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "applied_at_ms INTEGER NOT NULL DEFAULT (CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER))"
        ");");
}

}  // namespace mmcr::storage