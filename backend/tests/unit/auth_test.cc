#include <filesystem>

#include <gtest/gtest.h>

#include "auth/service.h"
#include "storage/database.h"
#include "storage/test_support.h"

namespace {

auto AuthMigrationsPath() -> std::filesystem::path {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
           "src" / "auth" / "migrations";
}

auto MakeAuthService(mmcr::storage::Database* database) -> mmcr::auth::AuthService {
    return mmcr::auth::AuthService(database);
}

auto OpenTestDatabase(const char* prefix) -> mmcr::util::StatusOr<mmcr::storage::TemporaryPath> {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath(prefix);
    return temp;
}

}  // namespace

TEST(AuthServiceTest, RegisterLoginRefreshAndLogoutFlow) {
    auto temp = OpenTestDatabase("mmcr_auth_flow");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    auto auth = MakeAuthService(&database);
    ASSERT_TRUE(auth.InitializeSchema(AuthMigrationsPath()).ok());

    const auto register_result = auth.Register(
        {.username = "PlayerOne",
         .password = "secret-password",
         .now_ms = 1'000});
    ASSERT_TRUE(register_result.ok()) << register_result.status().DebugString();

    auto login = auth.Login(
        {.identity = "PLAYERONE", .password = "secret-password", .now_ms = 3'000});
    ASSERT_TRUE(login.ok()) << login.status().DebugString();
    EXPECT_EQ(login.value().player.username, "PlayerOne");

    auto authenticated = auth.Authenticate(login.value().session.token, 3'500);
    ASSERT_TRUE(authenticated.ok()) << authenticated.status().DebugString();
    EXPECT_EQ(authenticated.value().player.player_id, login.value().player.player_id);

    auto refreshed = auth.RefreshSession(login.value().session.token, 4'000);
    ASSERT_TRUE(refreshed.ok()) << refreshed.status().DebugString();
    EXPECT_NE(refreshed.value().session.token, login.value().session.token);

    auto old_session = auth.Authenticate(login.value().session.token, 4'100);
    EXPECT_FALSE(old_session.ok());

    auto new_session = auth.Authenticate(refreshed.value().session.token, 4'200);
    ASSERT_TRUE(new_session.ok()) << new_session.status().DebugString();

    ASSERT_TRUE(auth.Logout(refreshed.value().session.token, 4'300).ok());
    EXPECT_FALSE(auth.Authenticate(refreshed.value().session.token, 4'400).ok());
}

TEST(AuthServiceTest, EnforcesCaseInsensitiveUniqueness) {
    auto temp = OpenTestDatabase("mmcr_auth_unique");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    auto auth = MakeAuthService(&database);
    ASSERT_TRUE(auth.InitializeSchema(AuthMigrationsPath()).ok());

    auto first = auth.Register(
        {.username = "CaseUser",
         .password = "secret-password",
         .now_ms = 10'000});
    ASSERT_TRUE(first.ok()) << first.status().DebugString();

    auto duplicate_username = auth.Register(
        {.username = "caseuser",
         .password = "secret-password",
         .now_ms = 11'000});
    EXPECT_FALSE(duplicate_username.ok());
}

TEST(AuthServiceTest, ChangePasswordKeepsCurrentSessionButRevokesOthers) {
    auto temp = OpenTestDatabase("mmcr_auth_change_pw");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    auto auth = MakeAuthService(&database);
    ASSERT_TRUE(auth.InitializeSchema(AuthMigrationsPath()).ok());

    auto register_result = auth.Register(
        {.username = "PasswordUser",
         .password = "old-password",
         .now_ms = 20'000});
    ASSERT_TRUE(register_result.ok()) << register_result.status().DebugString();

    auto first_session = auth.Login(
        {.identity = "PasswordUser", .password = "old-password", .now_ms = 21'000});
    ASSERT_TRUE(first_session.ok()) << first_session.status().DebugString();

    auto second_session = auth.Login(
        {.identity = "passworduser", .password = "old-password", .now_ms = 21'500});
    ASSERT_TRUE(second_session.ok()) << second_session.status().DebugString();

    const auto change_status = auth.ChangePassword(
        {.session_token = first_session.value().session.token,
         .current_password = "old-password",
         .new_password = "new-password",
         .now_ms = 22'000});
    ASSERT_TRUE(change_status.ok()) << change_status.DebugString();

    EXPECT_TRUE(auth.Authenticate(first_session.value().session.token, 22'100).ok());
    EXPECT_FALSE(auth.Authenticate(second_session.value().session.token, 22'100).ok());
    EXPECT_FALSE(auth.Login(
                     {.identity = "PasswordUser", .password = "old-password", .now_ms = 22'200})
                     .ok());

    auto login_with_new_password = auth.Login(
        {.identity = "PasswordUser", .password = "new-password", .now_ms = 22'300});
    ASSERT_TRUE(login_with_new_password.ok()) << login_with_new_password.status().DebugString();
}

TEST(AuthServiceTest, PlayerIdsDoNotReuseDeletedLargestId) {
    auto temp = OpenTestDatabase("mmcr_auth_player_ids");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    auto auth = MakeAuthService(&database);
    ASSERT_TRUE(auth.InitializeSchema(AuthMigrationsPath()).ok());

    auto first = auth.Register(
        {.username = "FirstUser",
         .password = "secret-password",
         .now_ms = 1'000});
    ASSERT_TRUE(first.ok()) << first.status().DebugString();

    auto second = auth.Register(
        {.username = "SecondUser",
         .password = "secret-password",
         .now_ms = 2'000});
    ASSERT_TRUE(second.ok()) << second.status().DebugString();

    ASSERT_EQ(first.value().player.player_id, 1);
    ASSERT_EQ(second.value().player.player_id, 2);

    ASSERT_TRUE(database.Execute(
        "DELETE FROM credentials WHERE player_id = 2;"
        "DELETE FROM sessions WHERE player_id = 2;"
        "DELETE FROM players WHERE id = 2;").ok());

    auto third = auth.Register(
        {.username = "ThirdUser",
         .password = "secret-password",
         .now_ms = 3'000});
    ASSERT_TRUE(third.ok()) << third.status().DebugString();
    EXPECT_EQ(third.value().player.player_id, 3);
}

TEST(AuthServiceTest, MigrationToAutoincrementPreservesCredentialsAndIdSequence) {
    auto temp = OpenTestDatabase("mmcr_auth_migrate_ids");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    ASSERT_TRUE(database.Execute(
        "CREATE TABLE schema_migrations ("
        "version TEXT PRIMARY KEY,"
        "name TEXT NOT NULL,"
        "applied_at_ms INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE players ("
        "id INTEGER PRIMARY KEY,"
        "username TEXT NOT NULL,"
        "username_normalized TEXT NOT NULL UNIQUE,"
        "created_at_ms INTEGER NOT NULL,"
        "updated_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE credentials ("
        "player_id INTEGER PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,"
        "password_hash TEXT NOT NULL,"
        "password_changed_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE sessions ("
        "id TEXT PRIMARY KEY,"
        "player_id INTEGER NOT NULL REFERENCES players(id) ON DELETE CASCADE,"
        "secret_hash TEXT NOT NULL,"
        "created_at_ms INTEGER NOT NULL,"
        "expires_at_ms INTEGER NOT NULL,"
        "last_refreshed_at_ms INTEGER NOT NULL,"
        "revoked_at_ms INTEGER,"
        "revoked_reason TEXT"
        ");"
        "CREATE INDEX idx_sessions_player_id ON sessions(player_id);"
        "INSERT INTO schema_migrations(version, name) VALUES ('0001_auth_core', '0001_auth_core.sql');").ok());

    auto auth = MakeAuthService(&database);

    auto first = auth.Register(
        {.username = "MigratedUserOne",
         .password = "secret-password",
         .now_ms = 1'000});
    ASSERT_TRUE(first.ok()) << first.status().DebugString();

    auto second = auth.Register(
        {.username = "MigratedUserTwo",
         .password = "secret-password",
         .now_ms = 2'000});
    ASSERT_TRUE(second.ok()) << second.status().DebugString();

    ASSERT_TRUE(auth.InitializeSchema(AuthMigrationsPath()).ok());

    auto login_existing = auth.Login(
        {.identity = "MigratedUserOne",
         .password = "secret-password",
         .now_ms = 2'500});
    ASSERT_TRUE(login_existing.ok()) << login_existing.status().DebugString();
    EXPECT_EQ(login_existing.value().player.player_id, 1);

    ASSERT_TRUE(database.Execute(
        "DELETE FROM credentials WHERE player_id = 2;"
        "DELETE FROM sessions WHERE player_id = 2;"
        "DELETE FROM players WHERE id = 2;").ok());

    auto third = auth.Register(
        {.username = "MigratedUserThree",
         .password = "secret-password",
         .now_ms = 3'000});
    ASSERT_TRUE(third.ok()) << third.status().DebugString();
    EXPECT_EQ(third.value().player.player_id, 3);
}