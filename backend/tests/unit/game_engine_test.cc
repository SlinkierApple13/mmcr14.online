#include "game/engine/engine.h"

#include <gtest/gtest.h>

#include "auth/service.h"
#include "external/qingque/basic/mahjong.h"
#include "util/status.h"

namespace mmcr::game {
namespace {

auto MakePlayers() -> std::array<auth::PlayerProfile, 4> {
    return std::array<auth::PlayerProfile, 4>{
        auth::PlayerProfile{100, "Alpha"},
        auth::PlayerProfile{101, "Beta"},
        auth::PlayerProfile{102, "Gamma"},
        auth::PlayerProfile{103, "Delta"},
    };
}

TEST(GameEngineTest, WallPrepareAndBidirectionalDrawUseTileTypeState) {
    Wall wall;
    wall.prepare(0x31415926ULL);

    EXPECT_EQ(136U, wall.size());
    EXPECT_FALSE(wall.empty());

    auto front_tiles = wall.draw(13);
    ASSERT_TRUE(front_tiles.ok()) << front_tiles.status().DebugString();
    EXPECT_EQ(13U, front_tiles.value().size());
    EXPECT_EQ(123U, wall.size());

    auto back_tiles = wall.draw(-2);
    ASSERT_TRUE(back_tiles.ok()) << back_tiles.status().DebugString();
    EXPECT_EQ(2U, back_tiles.value().size());
    EXPECT_EQ(121U, wall.size());

    auto zero_draw = wall.draw(0);
    ASSERT_FALSE(zero_draw.ok());
    EXPECT_EQ(util::StatusCode::kInvalidArgument, zero_draw.status().code());
}

TEST(GameEngineTest, StartHandDealsInitialHandsAndDealerOpeningDraw) {
    ActiveSession session;
    session.SetPlayers(MakePlayers());
    auto started = Engine::StartSession(session, 0, 0, 0x12345678ULL, 1000);

    ASSERT_TRUE(started.ok()) << started.DebugString();
    EXPECT_EQ(SessionPhase::kAwaitingDiscard, session.phase());
    EXPECT_EQ(0, session.current_turn_seat());
    EXPECT_TRUE(session.seat(0).has_drawn_tile());
    EXPECT_FALSE(session.last_discarded_tile().has_value());
    EXPECT_EQ(83U, session.wall().size());
    EXPECT_EQ(2U, session.state().stage_counter);
    ASSERT_TRUE(session.state().next_move.has_value());
    EXPECT_EQ(MoveKind::kTileDiscarded, session.state().next_move->kind);
    EXPECT_EQ(2U, session.game_log().moves().size());
    EXPECT_EQ(MoveKind::kRoundStarted, session.game_log().moves()[0].kind);
    EXPECT_EQ(MoveKind::kTileDrawn, session.game_log().moves()[1].kind);
    ASSERT_NE(nullptr, session.seat(0).player());
    EXPECT_EQ(100, session.seat(0).player()->player_id);

    EXPECT_EQ(13U, session.seat(0).hand_tiles().size());
    EXPECT_EQ(13U, session.seat(1).hand_tiles().size());
    EXPECT_EQ(13U, session.seat(2).hand_tiles().size());
    EXPECT_EQ(13U, session.seat(3).hand_tiles().size());
}

TEST(GameEngineTest, ApplyDiscardConsumesTileAndOpensReactionWindow) {
    ActiveSession session;
    session.SetPlayers(MakePlayers());
    auto started = Engine::StartSession(session, 0, 0, 0x42ULL, 1000);
    ASSERT_TRUE(started.ok()) << started.DebugString();
    ASSERT_TRUE(session.seat(0).has_drawn_tile());

    auto discarded = Engine::ApplyDiscard(
        session,
        DiscardCommand{
            .seat_index = 0,
            .tile = session.seat(0).drawn_tile(),
            .now_ms = 1010,
        });

    ASSERT_TRUE(discarded.ok()) << discarded.DebugString();
    EXPECT_EQ(SessionPhase::kAwaitingReactions, session.phase());
    EXPECT_FALSE(session.seat(0).has_drawn_tile());
    EXPECT_EQ(0, session.current_turn_seat());
    EXPECT_TRUE(session.last_discarded_tile().has_value());
    EXPECT_EQ(0, *session.last_discarded_by_seat());
    EXPECT_EQ(13U, session.seat(0).hand_tiles().size());
    EXPECT_EQ(1U, session.seat(0).discard_pile().size());
    EXPECT_EQ(4U, session.state().stage_counter);
    ASSERT_TRUE(session.state().next_move.has_value());
    EXPECT_EQ(MoveKind::kReactionResolved, session.state().next_move->kind);
    EXPECT_EQ(4U, session.game_log().moves().size());
    EXPECT_EQ(MoveKind::kTileDiscarded, session.game_log().moves()[2].kind);
    EXPECT_EQ(MoveKind::kReactionWindowOpened, session.game_log().moves()[3].kind);
}

TEST(GameEngineTest, RejectsDiscardFromWrongSeatOrMissingTile) {
    ActiveSession session;
    session.SetPlayers(MakePlayers());
    auto started = Engine::StartSession(session, 0, 1, 0x99ULL, 1000);
    ASSERT_TRUE(started.ok()) << started.DebugString();

    auto wrong_seat = Engine::ApplyDiscard(
        session,
        DiscardCommand{
            .seat_index = 0,
            .tile = session.seat(0).hand_tiles().front(),
            .now_ms = 1010,
        });
    ASSERT_FALSE(wrong_seat.ok());
    EXPECT_EQ(util::StatusCode::kInvalidArgument, wrong_seat.status().code());

    auto missing_tile = Engine::ApplyDiscard(
        session,
        DiscardCommand{
            .seat_index = 1,
            .tile = mahjong::tile::invalid,
            .now_ms = 1010,
        });
    ASSERT_FALSE(missing_tile.ok());
    EXPECT_EQ(util::StatusCode::kInvalidArgument, missing_tile.status().code());
}

TEST(GameEngineTest, DiscardingFromHandPromotesDrawnTileIntoHandList) {
    ActiveSession session;
    session.SetPlayers(MakePlayers());
    auto started = Engine::StartSession(session, 0, 0, 0x7788ULL, 1000);
    ASSERT_TRUE(started.ok()) << started.DebugString();
    ASSERT_TRUE(session.seat(0).has_drawn_tile());

    const auto drawn_tile = session.seat(0).drawn_tile();
    const auto hand_tile = session.seat(0).hand_tiles().front();
    ASSERT_NE(hand_tile, drawn_tile);

    auto discarded = Engine::ApplyDiscard(
        session,
        DiscardCommand{
            .seat_index = 0,
            .tile = hand_tile,
            .now_ms = 1010,
        });

    ASSERT_TRUE(discarded.ok()) << discarded.DebugString();
    EXPECT_FALSE(session.seat(0).has_drawn_tile());
    EXPECT_EQ(13U, session.seat(0).hand_tiles().size());
    EXPECT_NE(session.seat(0).hand_tiles().end(),
              std::find(session.seat(0).hand_tiles().begin(),
                        session.seat(0).hand_tiles().end(),
                        drawn_tile));
}

}  // namespace
}  // namespace mmcr::game