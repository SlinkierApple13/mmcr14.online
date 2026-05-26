#include <barrier>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

#include <gtest/gtest.h>

#include "lobby/service.h"
#include "storage/database.h"
#include "storage/test_support.h"

namespace {

class FakeMatchStarter final : public mmcr::lobby::MatchStarter {
public:
    mmcr::util::StatusOr<mmcr::lobby::StartMatchResult> StartMatch(
        const mmcr::lobby::StartMatchRequest& request) override {
        requests.push_back(request);
        return mmcr::lobby::StartMatchResult{"match-" + std::to_string(requests.size())};
    }

    std::vector<mmcr::lobby::StartMatchRequest> requests;
};

auto LobbyMigrationsPath() -> std::filesystem::path {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
           "src" / "lobby" / "migrations";
}

auto DefaultSettings() -> mmcr::lobby::TableSettings {
    return mmcr::lobby::TableSettings{
        .total_rounds = 8,
        .time_limit_ord_sec = 7,
        .time_limit_sup_sec = 4,
        .rated = false,
        .recorded = false,
        .is_public = true,
        .debug = false,
        .singleplayer = false,
    };
}

}  // namespace

TEST(LobbyServiceTest, CreatesTableAndListsPublicOpenTables) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_lobby_create");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    FakeMatchStarter starter;
    mmcr::lobby::LobbyService lobby(&database, &starter);
    ASSERT_TRUE(lobby.InitializeSchema(LobbyMigrationsPath()).ok());

    auto created = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = DefaultSettings(),
         .now_ms = 1'000});
    ASSERT_TRUE(created.ok()) << created.status().DebugString();
    EXPECT_EQ(created.value().state, mmcr::lobby::TableState::kOpen);
    ASSERT_EQ(created.value().seats.size(), 1U);
    EXPECT_EQ(created.value().seats[0].seat_index, 0);
    EXPECT_EQ(created.value().seats[0].player_id, "player-1");
    EXPECT_FALSE(created.value().seats[0].ready);

    auto open_tables = lobby.ListOpenTables();
    ASSERT_TRUE(open_tables.ok()) << open_tables.status().DebugString();
    ASSERT_EQ(open_tables.value().size(), 1U);
    EXPECT_EQ(open_tables.value()[0].occupied_seat_count, 1);
    EXPECT_EQ(open_tables.value()[0].ready_seat_count, 0);

    auto duplicate = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = DefaultSettings(),
         .now_ms = 2'000});
    EXPECT_FALSE(duplicate.ok());
}

TEST(LobbyServiceTest, JoinLeaveAndSeatReuseFollowLobbyRules) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_lobby_join_leave");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    FakeMatchStarter starter;
    mmcr::lobby::LobbyService lobby(&database, &starter);
    ASSERT_TRUE(lobby.InitializeSchema(LobbyMigrationsPath()).ok());

    auto created = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = DefaultSettings(),
         .now_ms = 1'000});
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    auto joined_two = lobby.JoinTable(
        {.table_id = created.value().table_id,
         .player_id = "player-2",
         .display_name = "Player Two",
         .now_ms = 1'500});
    ASSERT_TRUE(joined_two.ok()) << joined_two.status().DebugString();
    EXPECT_EQ(joined_two.value().seats[1].seat_index, 1);

    auto joined_three = lobby.JoinTable(
        {.table_id = created.value().table_id,
         .player_id = "player-3",
         .display_name = "Player Three",
         .now_ms = 2'000});
    ASSERT_TRUE(joined_three.ok()) << joined_three.status().DebugString();
    EXPECT_EQ(joined_three.value().seats[2].seat_index, 2);

    EXPECT_TRUE(lobby.LeaveTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-2",
                     .now_ms = 2'500})
                    .ok());

    auto joined_four = lobby.JoinTable(
        {.table_id = created.value().table_id,
         .player_id = "player-4",
         .display_name = "Player Four",
         .now_ms = 3'000});
    ASSERT_TRUE(joined_four.ok()) << joined_four.status().DebugString();
    ASSERT_EQ(joined_four.value().seats.size(), 3U);
    EXPECT_EQ(joined_four.value().seats[1].player_id, "player-4");
    EXPECT_EQ(joined_four.value().seats[1].seat_index, 1);

    auto other_table = lobby.CreateTable(
        {.creator_player_id = "player-4",
         .creator_display_name = "Player Four",
         .settings = DefaultSettings(),
         .now_ms = 3'500});
    EXPECT_FALSE(other_table.ok());

    EXPECT_TRUE(lobby.LeaveTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-1",
                     .now_ms = 4'000})
                    .ok());
    auto after_creator_leave = lobby.GetTable(created.value().table_id);
    ASSERT_TRUE(after_creator_leave.ok()) << after_creator_leave.status().DebugString();
    EXPECT_EQ(after_creator_leave.value().seats.size(), 2U);

    EXPECT_TRUE(lobby.LeaveTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-3",
                     .now_ms = 4'500})
                    .ok());
    EXPECT_TRUE(lobby.LeaveTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-4",
                     .now_ms = 5'000})
                    .ok());
    auto deleted = lobby.GetTable(created.value().table_id);
    EXPECT_FALSE(deleted.ok());
}

TEST(LobbyServiceTest, ConcurrentJoinsUseDistinctSeatsAndRejectOverflow) {
    FakeMatchStarter starter;
    mmcr::lobby::LobbyService lobby(nullptr, &starter);
    ASSERT_TRUE(lobby.InitializeSchema(LobbyMigrationsPath()).ok());

    auto created = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = DefaultSettings(),
         .now_ms = 1'000});
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    struct JoinResult {
        bool ok{false};
        int seat_index{-1};
        std::string message;
    };

    std::array<JoinResult, 4> results;
    std::barrier sync_point(5);
    std::array<std::thread, 4> threads;
    const std::array<const char*, 4> player_ids = {"player-2", "player-3", "player-4", "player-5"};
    const std::array<const char*, 4> display_names = {"Player Two", "Player Three", "Player Four", "Player Five"};

    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index]() {
            sync_point.arrive_and_wait();
            auto joined = lobby.JoinTable(
                {.table_id = created.value().table_id,
                 .player_id = player_ids[index],
                 .display_name = display_names[index],
                 .now_ms = 2'000 + static_cast<std::int64_t>(index)});
            if (!joined.ok()) {
                results[index] = JoinResult{false, -1, joined.status().message()};
                return;
            }

            int seat_index = -1;
            for (const auto& seat : joined.value().seats) {
                if (seat.player_id == player_ids[index]) {
                    seat_index = seat.seat_index;
                    break;
                }
            }

            results[index] = JoinResult{true, seat_index, {}};
        });
    }

    sync_point.arrive_and_wait();
    for (auto& thread : threads) {
        thread.join();
    }

    int success_count = 0;
    std::set<int> successful_seats;
    int failure_count = 0;
    for (const auto& result : results) {
        if (result.ok) {
            ++success_count;
            successful_seats.insert(result.seat_index);
        } else {
            ++failure_count;
        }
    }

    EXPECT_EQ(success_count, 3);
    EXPECT_EQ(failure_count, 1);
    EXPECT_EQ(successful_seats, std::set<int>({1, 2, 3}));

    auto snapshot = lobby.GetTable(created.value().table_id);
    ASSERT_TRUE(snapshot.ok()) << snapshot.status().DebugString();
    ASSERT_EQ(snapshot.value().seats.size(), 4U);
    EXPECT_EQ(snapshot.value().seats[0].player_id, "player-1");
}

TEST(LobbyServiceTest, ReadyTableStartsMatchImmediately) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_lobby_start");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    FakeMatchStarter starter;
    mmcr::lobby::LobbyService lobby(&database, &starter);
    ASSERT_TRUE(lobby.InitializeSchema(LobbyMigrationsPath()).ok());

    auto created = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = DefaultSettings(),
         .now_ms = 10'000});
    ASSERT_TRUE(created.ok()) << created.status().DebugString();

    ASSERT_TRUE(lobby.JoinTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-2",
                     .display_name = "Player Two",
                     .now_ms = 10'100})
                    .ok());
    ASSERT_TRUE(lobby.JoinTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-3",
                     .display_name = "Player Three",
                     .now_ms = 10'200})
                    .ok());
    ASSERT_TRUE(lobby.JoinTable(
                    {.table_id = created.value().table_id,
                     .player_id = "player-4",
                     .display_name = "Player Four",
                     .now_ms = 10'300})
                    .ok());

    ASSERT_TRUE(lobby.SetReady(
                    {.table_id = created.value().table_id,
                     .player_id = "player-1",
                     .ready = true,
                     .now_ms = 10'400})
                    .ok());
    ASSERT_TRUE(lobby.SetReady(
                    {.table_id = created.value().table_id,
                     .player_id = "player-2",
                     .ready = true,
                     .now_ms = 10'500})
                    .ok());
    ASSERT_TRUE(lobby.SetReady(
                    {.table_id = created.value().table_id,
                     .player_id = "player-3",
                     .ready = true,
                     .now_ms = 10'600})
                    .ok());

    auto started = lobby.SetReady(
        {.table_id = created.value().table_id,
         .player_id = "player-4",
         .ready = true,
         .now_ms = 10'700});
    ASSERT_TRUE(started.ok()) << started.status().DebugString();
    EXPECT_EQ(started.value().state, mmcr::lobby::TableState::kStarted);
    ASSERT_TRUE(started.value().match_id.has_value());
    EXPECT_EQ(*started.value().match_id, "match-1");

    ASSERT_EQ(starter.requests.size(), 1U);
    EXPECT_EQ(starter.requests[0].participants[0].player_id, "player-1");
    EXPECT_EQ(starter.requests[0].participants[1].player_id, "player-2");
    EXPECT_EQ(starter.requests[0].participants[2].player_id, "player-3");
    EXPECT_EQ(starter.requests[0].participants[3].player_id, "player-4");

    auto open_tables = lobby.ListOpenTables();
    ASSERT_TRUE(open_tables.ok()) << open_tables.status().DebugString();
    EXPECT_TRUE(open_tables.value().empty());
}

TEST(LobbyServiceTest, SingleplayerStartsImmediatelyAndInvalidCombosAreRejected) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_lobby_singleplayer");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    FakeMatchStarter starter;
    mmcr::lobby::LobbyService lobby(&database, &starter);
    ASSERT_TRUE(lobby.InitializeSchema(LobbyMigrationsPath()).ok());

    auto invalid_rated = lobby.CreateTable(
        {.creator_player_id = "player-1",
         .creator_display_name = "Player One",
         .settings = mmcr::lobby::TableSettings{
             .total_rounds = 8,
             .time_limit_ord_sec = 7,
             .time_limit_sup_sec = 4,
             .rated = true,
             .recorded = false,
             .is_public = true,
             .debug = false,
             .singleplayer = false,
         },
         .now_ms = 1'000});
    EXPECT_FALSE(invalid_rated.ok());

    auto invalid_singleplayer = lobby.CreateTable(
        {.creator_player_id = "player-2",
         .creator_display_name = "Player Two",
         .settings = mmcr::lobby::TableSettings{
             .total_rounds = 8,
             .time_limit_ord_sec = 7,
             .time_limit_sup_sec = 4,
             .rated = false,
             .recorded = true,
             .is_public = false,
             .debug = false,
             .singleplayer = true,
         },
         .now_ms = 1'500});
    EXPECT_FALSE(invalid_singleplayer.ok());

    auto singleplayer = lobby.CreateTable(
        {.creator_player_id = "player-3",
         .creator_display_name = "Player Three",
         .settings = mmcr::lobby::TableSettings{
             .total_rounds = 8,
             .time_limit_ord_sec = 7,
             .time_limit_sup_sec = 4,
             .rated = false,
             .recorded = false,
             .is_public = false,
             .debug = false,
             .singleplayer = true,
         },
         .now_ms = 2'000});
    ASSERT_TRUE(singleplayer.ok()) << singleplayer.status().DebugString();
    EXPECT_EQ(singleplayer.value().state, mmcr::lobby::TableState::kStarted);
    ASSERT_EQ(starter.requests.size(), 1U);
    EXPECT_TRUE(starter.requests[0].participants[1].is_virtual);
    EXPECT_TRUE(starter.requests[0].participants[2].is_virtual);
    EXPECT_TRUE(starter.requests[0].participants[3].is_virtual);
}