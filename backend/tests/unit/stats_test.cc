#include <filesystem>

#include <gtest/gtest.h>

#include "stats/service.h"
#include "storage/database.h"
#include "storage/test_support.h"

namespace {

auto StatsMigrationsPath() -> std::filesystem::path {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
           "src" / "stats" / "migrations";
}

auto MakeWinCode(std::size_t index) -> qingque::fan_code {
    qingque::fan_code code;
    code.set(index, true);
    return code;
}

auto MakeSeat(std::int64_t player_id, std::string username) -> Json::Value {
    Json::Value seat(Json::objectValue);
    seat["player_id"] = Json::Int64(player_id);
    seat["player_name"] = std::move(username);
    seat["afk_counter"] = 0;
    seat["score"] = 0;
    seat["disconnected"] = false;
    return seat;
}

auto MakeWinTypeBits(int winner_seat, mahjong::win_t extra_bits = 0) -> std::uint64_t {
    return static_cast<std::uint64_t>(extra_bits | static_cast<mahjong::win_t>(winner_seat));
}

auto MakeRecordBase(std::string session_identifier, std::uint64_t round_number) -> Json::Value {
    Json::Value record(Json::objectValue);
    record["version"] = 2;
    record["header"]["session_identifier"] = std::move(session_identifier);
    record["header"]["round_number"] = Json::UInt64(round_number);

    record["initial_seats"].append(MakeSeat(11, "Alice"));
    record["initial_seats"].append(MakeSeat(22, "Bob"));
    record["initial_seats"].append(MakeSeat(33, "Carol"));
    record["initial_seats"].append(MakeSeat(44, "Dave"));
    record["transition_queue"] = Json::Value(Json::arrayValue);
    record["event_queue"] = Json::Value(Json::arrayValue);
    return record;
}

auto MakeRoundResult(bool completed,
                     std::string terminal_kind,
                     bool drawn_game,
                     std::int64_t turn,
                     std::int64_t time_ms,
                     std::array<int, 4> meld_count,
                     std::optional<int> winner_seat = std::nullopt,
                     std::optional<int> from_seat = std::nullopt,
                     std::uint64_t win_type_bits = 0,
                     std::optional<mahjong::tile_t> win_tile = std::nullopt,
                     double fan = 0.0,
                     std::vector<qingque::fan_code> fan_results = {}) -> Json::Value {
    Json::Value result(Json::objectValue);
    result["completed"] = completed;
    result["terminal_kind"] = std::move(terminal_kind);
    result["drawn_game"] = drawn_game;
    result["turn"] = Json::Int64(turn);
    result["total_turn"] = Json::Int64(turn);
    result["time_ms"] = Json::Int64(time_ms);

    Json::Value melds(Json::arrayValue);
    for (const int count : meld_count) {
        melds.append(count);
    }
    result["meld_count"] = std::move(melds);

    if (winner_seat.has_value()) {
        result["winner_seat"] = *winner_seat;
    } else {
        result["winner_seat"] = Json::Value(Json::nullValue);
    }
    if (from_seat.has_value()) {
        result["from_seat"] = *from_seat;
    } else {
        result["from_seat"] = Json::Value(Json::nullValue);
    }
    result["win_type_bits"] = Json::UInt64(win_type_bits);
    if (win_tile.has_value()) {
        result["win_tile"] = Json::UInt(*win_tile);
    } else {
        result["win_tile"] = Json::Value(Json::nullValue);
    }
    result["fan"] = fan;

    Json::Value fan_results_json(Json::arrayValue);
    for (const auto& fan_result : fan_results) {
        fan_results_json.append(fan_result.to_string());
    }
    result["fan_results"] = std::move(fan_results_json);
    return result;
}

auto MakeDiscardWinRecord() -> Json::Value {
    auto record = MakeRecordBase("100_1000", 1);
    record["round_result"] = MakeRoundResult(
        true,
        "discard_win",
        false,
        2,
        80,
        {0, 0, 1, 0},
        1,
        2,
        MakeWinTypeBits(1),
        9,
        5.0,
        {MakeWinCode(qingque::seven_pairs)});
    Json::Value winning_hand(Json::objectValue);
    winning_hand["format"] = "mmcr.hand_wrapper.v1";
    winning_hand["melds"] = Json::Value(Json::arrayValue);
    winning_hand["hand_tiles"] = Json::Value(Json::arrayValue);
    winning_hand["hand_tiles"].append(1);
    winning_hand["hand_tiles"].append(1);
    winning_hand["winning_tile"] = 9;
    winning_hand["winning_type"] = Json::UInt64(MakeWinTypeBits(1));
    record["round_result"]["winning_hand"] = std::move(winning_hand);
    return record;
}

auto MakeSelfDrawRecord() -> Json::Value {
    auto record = MakeRecordBase("100_1000", 2);
    record["round_result"] = MakeRoundResult(
        true,
        "self_drawn_win",
        false,
        3,
        170,
        {0, 0, 0, 0},
        2,
        2,
        MakeWinTypeBits(2, mahjong::win_type::self_drawn),
        24,
        3.0,
        {MakeWinCode(qingque::full_flush)});
    return record;
}

auto MakeDrawnGameRecord() -> Json::Value {
    auto record = MakeRecordBase("1000001_1000", 1);
    record["round_result"] = MakeRoundResult(
        true,
        "drawn_game",
        true,
        4,
        220,
        {0, 0, 0, 0});
    return record;
}

auto MakeIncompleteEndRecord() -> Json::Value {
    auto record = MakeRecordBase("100_1000", 3);
    record["round_result"] = MakeRoundResult(
        false,
        "end",
        false,
        1,
        260,
        {0, 0, 0, 0});
    return record;
}

}  // namespace

TEST(StatsProjectorTest, ProjectsDiscardWinFromCurrentRecordFormat) {
    auto projected = mmcr::stats::ProjectRoundRecord(MakeDiscardWinRecord());
    ASSERT_TRUE(projected.ok()) << projected.status().DebugString();

    EXPECT_FALSE(projected.value().drawn_game);
    EXPECT_EQ(projected.value().round_key.session_identifier, "100_1000");
    EXPECT_EQ(projected.value().round_key.round_number, 1u);
    EXPECT_EQ(projected.value().winner_player_id(), 22);
    EXPECT_EQ(projected.value().from_player_id(), 33);
    EXPECT_EQ(projected.value().timestamp_ms, 80);
    EXPECT_EQ(projected.value().turn, 2);
    EXPECT_EQ(projected.value().meld_count[2], 1);
    EXPECT_FALSE(projected.value().self_drawn());
    EXPECT_TRUE(projected.value().fan_results.front().test(qingque::seven_pairs));
    ASSERT_FALSE(projected.value().fan_ids.empty());
    EXPECT_EQ(projected.value().fan_ids.front(), qingque::seven_pairs);
    ASSERT_TRUE(projected.value().winning_hand.has_value());
    EXPECT_EQ(projected.value().winning_hand->winning_tile, 9);
}

TEST(StatsServiceTest, PersistsLoadsAndQueriesRoundEntries) {
    auto temp = mmcr::storage::MakeTemporaryDatabasePath("mmcr_stats_service");
    ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

    mmcr::storage::Database database;
    ASSERT_TRUE(database.Open({temp.value().path(), true, true}).ok());

    mmcr::stats::StatsService stats(&database);
    ASSERT_TRUE(stats.InitializeSchema(StatsMigrationsPath()).ok());
    ASSERT_TRUE(stats.UpsertRoundRecord(MakeDiscardWinRecord()).ok());
    ASSERT_TRUE(stats.UpsertRoundRecord(MakeSelfDrawRecord()).ok());
    ASSERT_TRUE(stats.UpsertRoundRecord(MakeDrawnGameRecord()).ok());
    ASSERT_TRUE(stats.UpsertRoundRecord(MakeIncompleteEndRecord()).ok());
    ASSERT_EQ(stats.round_count(), 3u);

    auto players = stats.ListPlayers();
    ASSERT_EQ(players.size(), 4u);
    EXPECT_EQ(players.front().username, "Alice");

    mmcr::stats::StatsService reloaded(&database);
    ASSERT_TRUE(reloaded.InitializeSchema(StatsMigrationsPath()).ok());
    ASSERT_TRUE(reloaded.LoadFromDatabase().ok());
    ASSERT_EQ(reloaded.round_count(), 3u);

    mmcr::stats::StatsFilter bob_filter;
    bob_filter.player_id = 22;
    auto bob_stats = reloaded.Query(bob_filter);
    ASSERT_TRUE(bob_stats.ok()) << bob_stats.status().DebugString();
    EXPECT_EQ(bob_stats.value().rounds.size(), 2u);
    EXPECT_EQ(bob_stats.value().tot_wins, 1u);
    EXPECT_DOUBLE_EQ(bob_stats.value().avg_win_pt(), 75.0);
    EXPECT_DOUBLE_EQ(bob_stats.value().drawn_game_rate(), 0.0);

    mmcr::stats::StatsFilter self_draw_filter;
    self_draw_filter.player_id = 33;
    self_draw_filter.self_drawn = true;
    auto self_draw_stats = reloaded.Query(self_draw_filter);
    ASSERT_TRUE(self_draw_stats.ok()) << self_draw_stats.status().DebugString();
    EXPECT_EQ(self_draw_stats.value().rounds.size(), 1u);
    EXPECT_EQ(self_draw_stats.value().tot_hwins, 1u);

    mmcr::stats::StatsFilter standard_only;
    auto standard_page = reloaded.ListRounds(standard_only, "fan", "desc", 0, 10);
    ASSERT_TRUE(standard_page.ok()) << standard_page.status().DebugString();
    ASSERT_EQ(standard_page.value().total_count, 2u);
    ASSERT_EQ(standard_page.value().rounds.size(), 2u);
    EXPECT_GE(standard_page.value().rounds[0]->fan, standard_page.value().rounds[1]->fan);

    ASSERT_TRUE(standard_page.value().rounds.front()->winning_hand.has_value());

    mmcr::stats::StatsFilter nonstandard_only;
    nonstandard_only.nonstandard_only = true;
    auto nonstandard_rounds = reloaded.ListRounds(nonstandard_only, "time", "asc", 0, 10);
    ASSERT_TRUE(nonstandard_rounds.ok()) << nonstandard_rounds.status().DebugString();
    EXPECT_EQ(nonstandard_rounds.value().total_count, 1u);
}