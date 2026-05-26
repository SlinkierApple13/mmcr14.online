#include "storage/database.h"

#include <sqlite3.h>

#include <utility>

namespace mmcr::storage {

namespace {

auto SqliteStatus(int code, sqlite3* db, std::string_view fallback)
    -> util::Status {
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) {
        return util::Status::Ok();
    }

    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    if (message == nullptr) {
        return util::Status::Internal(std::string(fallback));
    }

    return util::Status::Internal(message);
}

}  // namespace

struct Database::Impl {
    sqlite3* db{nullptr};
    SqliteConfig config;
};

struct Statement::Impl {
    sqlite3* db{nullptr};
    sqlite3_stmt* stmt{nullptr};
};

struct Transaction::Impl {
    sqlite3* db{nullptr};
    bool active{false};
};

Database::Database() : impl_(std::make_unique<Impl>()) {}

Database::~Database() {
    Close();
}

Database::Database(Database&&) noexcept = default;

auto Database::operator=(Database&&) noexcept -> Database& = default;

util::Status Database::Open(const SqliteConfig& config) {
    Close();

    if (config.path.empty()) {
        return util::Status::InvalidArgument("database path must not be empty");
    }

    sqlite3* db = nullptr;
    const int code = sqlite3_open_v2(
        config.path.string().c_str(),
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        nullptr);
    if (code != SQLITE_OK) {
        const auto status = SqliteStatus(code, db, "failed to open database");
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return status;
    }

    impl_->db = db;
    impl_->config = config;

    const int busy_timeout_code = sqlite3_busy_timeout(impl_->db, 5000);
    if (busy_timeout_code != SQLITE_OK) {
        const auto status = SqliteStatus(busy_timeout_code, impl_->db, "failed to set busy timeout");
        Close();
        return status;
    }

    if (config.enable_foreign_keys) {
        const auto status = Execute("PRAGMA foreign_keys = ON;");
        if (!status.ok()) {
            Close();
            return status;
        }
    }

    if (config.enable_wal) {
        const auto status = Execute("PRAGMA journal_mode = WAL;");
        if (!status.ok()) {
            Close();
            return status;
        }
    }

    return util::Status::Ok();
}

void Database::Close() {
    if (impl_ == nullptr || impl_->db == nullptr) {
        return;
    }

    sqlite3_close(impl_->db);
    impl_->db = nullptr;
}

bool Database::is_open() const noexcept {
    return impl_ != nullptr && impl_->db != nullptr;
}

const SqliteConfig& Database::config() const noexcept {
    return impl_->config;
}

util::Status Database::Execute(std::string_view sql) const {
    if (!is_open()) {
        return util::Status::Internal("database is not open");
    }

    char* error_message = nullptr;
    const int code = sqlite3_exec(
        impl_->db,
        std::string(sql).c_str(),
        nullptr,
        nullptr,
        &error_message);
    if (code != SQLITE_OK) {
        std::string message = error_message != nullptr ? error_message : "sqlite exec failed";
        if (error_message != nullptr) {
            sqlite3_free(error_message);
        }
        return util::Status::Internal(message);
    }

    return util::Status::Ok();
}

util::StatusOr<Statement> Database::Prepare(std::string_view sql) const {
    if (!is_open()) {
        return util::Status::Internal("database is not open");
    }

    auto impl = std::make_unique<Statement::Impl>();
    impl->db = impl_->db;

    const int code = sqlite3_prepare_v2(
        impl_->db,
        std::string(sql).c_str(),
        -1,
        &impl->stmt,
        nullptr);
    if (code != SQLITE_OK) {
        return SqliteStatus(code, impl_->db, "failed to prepare statement");
    }

    return Statement(std::move(impl));
}

util::StatusOr<Transaction> Database::BeginTransaction() {
    if (!is_open()) {
        return util::Status::Internal("database is not open");
    }

    const auto status = Execute("BEGIN IMMEDIATE TRANSACTION;");
    if (!status.ok()) {
        return status;
    }

    auto impl = std::make_unique<Transaction::Impl>();
    impl->db = impl_->db;
    impl->active = true;
    return Transaction(std::move(impl));
}

util::StatusOr<std::int64_t> Database::LastInsertRowId() const {
    if (!is_open()) {
        return util::Status::Internal("database is not open");
    }

    return static_cast<std::int64_t>(sqlite3_last_insert_rowid(impl_->db));
}

Statement::Statement() = default;

Statement::Statement(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Statement::~Statement() {
    if (impl_ != nullptr && impl_->stmt != nullptr) {
        sqlite3_finalize(impl_->stmt);
    }
}

Statement::Statement(Statement&&) noexcept = default;

auto Statement::operator=(Statement&&) noexcept -> Statement& = default;

bool Statement::valid() const noexcept {
    return impl_ != nullptr && impl_->stmt != nullptr;
}

util::Status Statement::BindInt64(int index, std::int64_t value) {
    if (!valid()) {
        return util::Status::Internal("statement is not valid");
    }

    return SqliteStatus(
        sqlite3_bind_int64(impl_->stmt, index, value),
        impl_->db,
        "failed to bind int64");
}

util::Status Statement::BindText(int index, std::string_view value) {
    if (!valid()) {
        return util::Status::Internal("statement is not valid");
    }

    return SqliteStatus(
        sqlite3_bind_text(
            impl_->stmt,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT),
        impl_->db,
        "failed to bind text");
}

util::Status Statement::BindNull(int index) {
    if (!valid()) {
        return util::Status::Internal("statement is not valid");
    }

    return SqliteStatus(
        sqlite3_bind_null(impl_->stmt, index),
        impl_->db,
        "failed to bind null");
}

util::StatusOr<Statement::StepResult> Statement::Step() {
    if (!valid()) {
        return util::Status::Internal("statement is not valid");
    }

    const int code = sqlite3_step(impl_->stmt);
    if (code == SQLITE_ROW) {
        return StepResult::kRow;
    }
    if (code == SQLITE_DONE) {
        return StepResult::kDone;
    }

    return SqliteStatus(code, impl_->db, "failed to step statement");
}

std::int64_t Statement::ColumnInt64(int index) const {
    return sqlite3_column_int64(impl_->stmt, index);
}

std::string Statement::ColumnText(int index) const {
    const unsigned char* text = sqlite3_column_text(impl_->stmt, index);
    if (text == nullptr) {
        return {};
    }

    return reinterpret_cast<const char*>(text);
}

util::Status Statement::Reset() {
    if (!valid()) {
        return util::Status::Internal("statement is not valid");
    }

    const int code = sqlite3_reset(impl_->stmt);
    sqlite3_clear_bindings(impl_->stmt);
    return SqliteStatus(code, impl_->db, "failed to reset statement");
}

Transaction::Transaction() = default;

Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Transaction::~Transaction() {
    if (impl_ != nullptr && impl_->active) {
        sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
}

Transaction::Transaction(Transaction&&) noexcept = default;

auto Transaction::operator=(Transaction&&) noexcept -> Transaction& = default;

bool Transaction::active() const noexcept {
    return impl_ != nullptr && impl_->active;
}

util::Status Transaction::Commit() {
    if (!active()) {
        return util::Status::Internal("transaction is not active");
    }

    const int code = sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
    if (code == SQLITE_OK) {
        impl_->active = false;
    }
    return SqliteStatus(code, impl_->db, "failed to commit transaction");
}

util::Status Transaction::Rollback() {
    if (!active()) {
        return util::Status::Internal("transaction is not active");
    }

    const int code = sqlite3_exec(impl_->db, "ROLLBACK;", nullptr, nullptr, nullptr);
    if (code == SQLITE_OK) {
        impl_->active = false;
    }
    return SqliteStatus(code, impl_->db, "failed to rollback transaction");
}

}  // namespace mmcr::storage