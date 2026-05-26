#include <filesystem>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>

#include <jsoncpp/json/json.h>

#include "storage/database.h"
#include "storage/game_record.h"
#include "storage/migration.h"
#include "storage/test_support.h"

namespace {

auto TestMigrationsPath() -> std::filesystem::path {
    return std::filesystem::path(__FILE__).parent_path() / "testdata" / "storage_migrations";
}

}  // namespace

TEST(StorageDatabaseTest, OpensDatabaseWithConfiguredPragmas) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_storage_open");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    const auto status = database.Open({temp.value().path(), true, true});
    ASSERT_TRUE(status.ok()) << status.DebugString();

    auto statement = database.Prepare("PRAGMA foreign_keys;");
    ASSERT_TRUE(statement.ok()) << statement.status().DebugString();
    auto step = statement.value().Step();
    ASSERT_TRUE(step.ok()) << step.status().DebugString();
    ASSERT_EQ(step.value(), mmcr::storage::Statement::StepResult::kRow);
    EXPECT_EQ(statement.value().ColumnInt64(0), 1);
}

TEST(StorageDatabaseTest, RollsBackUncommittedTransactions) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_storage_tx");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());
    ASSERT_TRUE(database.Execute("CREATE TABLE numbers(value INTEGER NOT NULL);").ok());

    {
        auto transaction = database.BeginTransaction();
        ASSERT_TRUE(transaction.ok()) << transaction.status().DebugString();
        ASSERT_TRUE(database.Execute("INSERT INTO numbers(value) VALUES (7);").ok());
    }

    auto statement = database.Prepare("SELECT COUNT(*) FROM numbers;");
    ASSERT_TRUE(statement.ok()) << statement.status().DebugString();
    auto step = statement.value().Step();
    ASSERT_TRUE(step.ok()) << step.status().DebugString();
    EXPECT_EQ(statement.value().ColumnInt64(0), 0);
}

TEST(StorageMigrationTest, AppliesOrderedSqlMigrationsOnce) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_storage_migrate");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::storage::MigrationRunner runner(&database);
    auto status = runner.ApplyDirectory(TestMigrationsPath());
    ASSERT_TRUE(status.ok()) << status.DebugString();

    auto query = database.Prepare(
        "SELECT value FROM test_items ORDER BY id ASC;");
    ASSERT_TRUE(query.ok()) << query.status().DebugString();

    auto first = query.value().Step();
    ASSERT_TRUE(first.ok()) << first.status().DebugString();
    ASSERT_EQ(first.value(), mmcr::storage::Statement::StepResult::kRow);
    EXPECT_EQ(query.value().ColumnText(0), "seeded");

    auto second = query.value().Step();
    ASSERT_TRUE(second.ok()) << second.status().DebugString();
    EXPECT_EQ(second.value(), mmcr::storage::Statement::StepResult::kDone);

    status = runner.ApplyDirectory(TestMigrationsPath());
    ASSERT_TRUE(status.ok()) << status.DebugString();

    auto count = database.Prepare("SELECT COUNT(*) FROM schema_migrations;");
    ASSERT_TRUE(count.ok()) << count.status().DebugString();
    auto step = count.value().Step();
    ASSERT_TRUE(step.ok()) << step.status().DebugString();
    EXPECT_EQ(count.value().ColumnInt64(0), 2);
}

TEST(StorageGameRecordTest, FlushesQueuedRoundRecordToDiskOnShutdown) {
    auto temp = mmcr::storage::MakeTemporaryDirectoryPath("mmcr_game_records");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    const auto root = temp.value().path();
    {
        mmcr::storage::GameRecordManager manager(root);
        Json::Value payload(Json::objectValue);
        payload["version"] = 1;
        payload["header"]["session_identifier"] = "123_456";
        payload["header"]["round_number"] = Json::UInt64(1);

        auto status = manager.Enqueue(mmcr::storage::GameRecordTask{
            .session_identifier = "123_456",
            .round_number = 1,
            .payload = payload,
        });
        ASSERT_TRUE(status.ok()) << status.DebugString();
    }

    const auto record_path = root / "123_456" / "1.json";
    ASSERT_TRUE(std::filesystem::exists(record_path));

    std::ifstream stream(record_path);
    ASSERT_TRUE(stream.is_open());

    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    ASSERT_TRUE(Json::parseFromStream(builder, stream, &parsed, &errors)) << errors;
    EXPECT_EQ(parsed["version"].asInt(), 1);
    EXPECT_EQ(parsed["header"]["session_identifier"].asString(), "123_456");
    EXPECT_EQ(parsed["header"]["round_number"].asUInt64(), 1);
}