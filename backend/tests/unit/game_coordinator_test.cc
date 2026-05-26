#include "game/coordinator/coordinator.h"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "util/status.h"

namespace mmcr::game {
namespace {

auto MakeStartMatchRequest() -> lobby::StartMatchRequest {
    lobby::StartMatchRequest request;
    request.table_id = "table-001";
    request.settings = lobby::TableSettings{
        .total_rounds = 4,
        .time_limit_ord_sec = 7,
        .time_limit_sup_sec = 4,
        .rated = false,
        .recorded = true,
        .is_public = true,
        .debug = false,
        .singleplayer = false,
    };
    request.participants = std::array<lobby::StartMatchParticipant, 4>{
        lobby::StartMatchParticipant{0, "100", "Alpha", false},
        lobby::StartMatchParticipant{1, "101", "Beta", false},
        lobby::StartMatchParticipant{2, "102", "Gamma", false},
        lobby::StartMatchParticipant{3, "103", "Delta", false},
    };
    request.now_ms = 123456789;
    return request;
}

TEST(GameCoordinatorTest, StartMatchCreatesRetrievableSeededHandState) {
    random::SeedContainer seed_container;
    seed_container.RecordTraffic(1001);
    seed_container.RecordTraffic(2002);

    GameCoordinator coordinator(&seed_container);
    auto result = coordinator.StartMatch(MakeStartMatchRequest());

    ASSERT_TRUE(result.ok()) << result.status().DebugString();
    EXPECT_EQ(1U, coordinator.MatchCount());
    EXPECT_FALSE(result.value().match_id.empty());

    auto match = coordinator.GetMatch(result.value().match_id);
    ASSERT_TRUE(match.ok()) << match.status().DebugString();
    EXPECT_EQ("table-001", match.value().table_id);
    EXPECT_EQ(4, match.value().settings.total_rounds);
    EXPECT_EQ(0, match.value().active_session.hand_index());
    EXPECT_EQ(0, match.value().active_session.dealer_seat());
    EXPECT_EQ(123456789, match.value().active_session.started_at_ms());
    EXPECT_EQ(SessionPhase::kAwaitingDiscard, match.value().active_session.phase());
    EXPECT_EQ(0, match.value().active_session.current_turn_seat());
    EXPECT_TRUE(match.value().active_session.seat(0).has_drawn_tile());
    EXPECT_EQ(83U, match.value().active_session.wall().size());
    EXPECT_EQ(3U, match.value().active_session.game_log().moves().size());
    EXPECT_EQ(MoveKind::kMatchStarted, match.value().active_session.game_log().moves()[0].kind);
    EXPECT_EQ(MoveKind::kRoundStarted, match.value().active_session.game_log().moves()[1].kind);
    EXPECT_EQ(MoveKind::kTileDrawn, match.value().active_session.game_log().moves()[2].kind);
    EXPECT_EQ(123456789, match.value().active_session.game_log().moves().front().timestamp_ms);
    ASSERT_NE(nullptr, match.value().active_session.seat(0).player());
    EXPECT_EQ(100, match.value().active_session.seat(0).player()->player_id);

    EXPECT_EQ(13U, match.value().active_session.seat(0).hand_tiles().size());
    EXPECT_EQ(13U, match.value().active_session.seat(1).hand_tiles().size());
    EXPECT_EQ(13U, match.value().active_session.seat(2).hand_tiles().size());
    EXPECT_EQ(13U, match.value().active_session.seat(3).hand_tiles().size());
}

TEST(GameCoordinatorTest, RejectsDuplicateSeatAssignments) {
    random::SeedContainer seed_container;
    seed_container.RecordTraffic(3003);
    seed_container.RecordTraffic(4004);

    auto request = MakeStartMatchRequest();
    request.participants[3].seat_index = 2;

    GameCoordinator coordinator(&seed_container);
    auto result = coordinator.StartMatch(request);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(util::StatusCode::kInvalidArgument, result.status().code());
    EXPECT_EQ(0U, coordinator.MatchCount());
}

}  // namespace
}  // namespace mmcr::game