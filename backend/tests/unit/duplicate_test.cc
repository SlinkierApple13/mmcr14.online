#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "duplicate/manager.h"
#include "storage/database.h"
#include "storage/test_support.h"

namespace {

auto DuplicateMigrationsPath() -> std::filesystem::path {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
           "src" / "duplicate" / "migrations";
}

constexpr std::int64_t kNow = 1'700'000'000'000;

}  // namespace

TEST(DuplicateManagerTest, CreateAndQuerySeedList) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_create");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    auto created = manager.CreateSeedList(4, 72, kNow);
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    EXPECT_EQ(created.value().creation_token.size(), 32u);
    EXPECT_EQ(created.value().master_token.size(), 32u);
    EXPECT_NE(created.value().creation_token, created.value().master_token);
    EXPECT_EQ(created.value().expires_at_ms, kNow + 72 * 3600 * 1000);
    EXPECT_EQ(mmcr::duplicate::TokenDisplayPrefix(created.value().creation_token),
              created.value().creation_token.substr(0, 5));

    auto queried = manager.Query(created.value().creation_token, kNow);
    ASSERT_TRUE(queried.ok()) << queried.status().DebugString();
    EXPECT_EQ(queried.value().round_count, 4u);
    EXPECT_EQ(queried.value().seeds.size(), 64u);
    EXPECT_FALSE(queried.value().expired(kNow));

    auto queried_by_master = manager.Query(created.value().master_token, kNow);
    ASSERT_TRUE(queried_by_master.ok());
    EXPECT_EQ(queried_by_master.value().creation_token, created.value().creation_token);
}

TEST(DuplicateManagerTest, SessionNumbersAllocateSequentially) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_numbers");
    ASSERT_TRUE(temp.ok());

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    auto created = manager.CreateSeedList(2, 3, kNow);
    ASSERT_TRUE(created.ok());

    auto first = manager.StartSession(created.value().creation_token, kNow);
    ASSERT_TRUE(first.ok()) << first.status().DebugString();
    EXPECT_EQ(first.value().next_session_number, 0);
    EXPECT_EQ(first.value().live_session_count, 1);
    EXPECT_EQ(first.value().seeds.size(), 32u);

    auto second = manager.StartSession(created.value().creation_token, kNow);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().next_session_number, 1);

    auto third = manager.StartSession(created.value().creation_token, kNow);
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(third.value().next_session_number, 2);

    auto invalid = manager.StartSession("deadbeef", kNow);
    EXPECT_FALSE(invalid.ok());

    auto expired_check = manager.StartSession(created.value().creation_token, kNow + 4 * 3600 * 1000);
    EXPECT_FALSE(expired_check.ok());
}

TEST(DuplicateManagerTest, StagingReleasesOnForceExpire) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_stage");
    ASSERT_TRUE(temp.ok());

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    std::vector<std::string> released;
    manager.SetReleaseCallback([&released](std::string payload_json) {
        released.push_back(std::move(payload_json));
    });

    auto created = manager.CreateSeedList(1, 72, kNow);
    ASSERT_TRUE(created.ok());
    const auto& token = created.value().creation_token;

    auto start = manager.StartSession(token, kNow);
    ASSERT_TRUE(start.ok());

    auto staged = manager.StageRecord(token, "123_456", 1, "{\"round\":1}", kNow);
    ASSERT_TRUE(staged.ok());
    EXPECT_TRUE(staged.value());
    EXPECT_TRUE(released.empty());

    // Expire while the session is still live: the row survives.
    auto force = manager.ForceExpire(created.value().master_token, kNow + 1);
    ASSERT_TRUE(force.ok());
    EXPECT_TRUE(released.empty());

    auto still_staged = manager.StageRecord(token, "123_456", 2, "{\"round\":2}", kNow + 1);
    ASSERT_TRUE(still_staged.ok());
    EXPECT_TRUE(still_staged.value());

    // Final session completes: row removed and staged records released.
    auto end = manager.EndSession(token, kNow + 2, true);
    ASSERT_TRUE(end.ok());
    ASSERT_EQ(released.size(), 2u);
    EXPECT_EQ(released[0], "{\"round\":1}");
    EXPECT_EQ(released[1], "{\"round\":2}");

    // The seed list is gone — later records release straight through.
    auto late = manager.StageRecord(token, "123_456", 3, "{\"round\":3}", kNow + 3);
    ASSERT_TRUE(late.ok());
    EXPECT_FALSE(late.value());

    auto queried = manager.Query(token, kNow + 3);
    EXPECT_FALSE(queried.ok());
}

TEST(DuplicateManagerTest, StartupRemovesExpiredListsAndReleasesStaging) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_startup");
    ASSERT_TRUE(temp.ok());

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    std::vector<std::string> released;
    manager.SetReleaseCallback([&released](std::string payload_json) {
        released.push_back(std::move(payload_json));
    });

    auto created = manager.CreateSeedList(1, 3, kNow);
    ASSERT_TRUE(created.ok());
    auto staged = manager.StageRecord(
        created.value().creation_token, "123_456", 1, "{\"round\":1}", kNow);
    ASSERT_TRUE(staged.ok());
    EXPECT_TRUE(staged.value());

    auto startup = manager.OnServerStartup(kNow + 4 * 3600 * 1000);
    ASSERT_TRUE(startup.ok());
    ASSERT_EQ(released.size(), 1u);
    EXPECT_EQ(released[0], "{\"round\":1}");

    auto queried = manager.Query(created.value().creation_token, kNow);
    EXPECT_FALSE(queried.ok());
}

TEST(DuplicateManagerTest, ExtendExpiryRules) {    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_extend");
    ASSERT_TRUE(temp.ok());

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    auto created = manager.CreateSeedList(1, 3, kNow);
    ASSERT_TRUE(created.ok());

    auto extend = manager.ExtendExpiry(created.value().master_token, 24, kNow);
    ASSERT_TRUE(extend.ok());
    auto queried = manager.Query(created.value().master_token, kNow);
    ASSERT_TRUE(queried.ok());
    EXPECT_EQ(queried.value().expires_at_ms, kNow + 24 * 3600 * 1000);

    // Cannot extend an expired list.
    auto expired_extend = manager.ExtendExpiry(
        created.value().master_token, 24, kNow + 25 * 3600 * 1000);
    EXPECT_FALSE(expired_extend.ok());

    // Cannot force-expire an already-expired list.
    auto expired_force = manager.ForceExpire(
        created.value().master_token, kNow + 25 * 3600 * 1000);
    EXPECT_FALSE(expired_force.ok());

    // Invalid expiry hours are rejected.
    auto bad_hours = manager.ExtendExpiry(created.value().master_token, 5, kNow);
    EXPECT_FALSE(bad_hours.ok());
}

TEST(DuplicateManagerTest, ForceExpireAfterPendingSessionReleasedDeletesRow) {
    // Mirrors: a table is created, its pending session is garbage-collected
    // (hub releases the live-session count), then the master force-expires
    // the seed list. The row must vanish entirely.
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_duplicate_pending_gc");
    ASSERT_TRUE(temp.ok());

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::duplicate::DuplicateManager manager(&database);
    ASSERT_TRUE(manager.InitializeSchema(DuplicateMigrationsPath()).ok());

    auto created = manager.CreateSeedList(1, 72, kNow);
    ASSERT_TRUE(created.ok());
    const auto& token = created.value().creation_token;

    auto start = manager.StartSession(token, kNow);
    ASSERT_TRUE(start.ok());
    EXPECT_EQ(start.value().live_session_count, 1);

    // Pending session destroyed by hub GC: release the live-session count.
    auto ended = manager.EndSession(token, kNow + 1, false);
    ASSERT_TRUE(ended.ok());

    // The list is not expired yet — the row must still exist.
    auto still_there = manager.Query(token, kNow + 1);
    ASSERT_TRUE(still_there.ok());
    EXPECT_EQ(still_there.value().live_session_count, 0);

    // Force-expire with no live sessions left: the row is removed.
    auto force = manager.ForceExpire(created.value().master_token, kNow + 2);
    ASSERT_TRUE(force.ok());

    auto gone = manager.Query(token, kNow + 2);
    EXPECT_FALSE(gone.ok());
}
