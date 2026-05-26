#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include <jsoncpp/json/json.h>

#include "auth/service.h"
#include "external/qingque/basic/mahjong_utils.h"
#include "external/qingque/rules/qingque.h"
#include "game/hub/hub.h"
#include "random/seed.h"
#include "storage/game_record.h"
#include "storage/test_support.h"

#define private public
#include "game/engine/session.h"
#undef private

namespace mmcr::game {
void GameHub::send_to_player(std::int64_t, const Json::Value&, int) {}
void GameHub::notify_session_lists_changed() {}

namespace {

struct SessionHarness {
	explicit SessionHarness(storage::GameRecordManager* record_manager = nullptr,
						 GameConfig config = GameConfig{})
		: players{std::make_shared<auth::PlayerProfile>(auth::PlayerProfile{101, "Alpha"}),
				  std::make_shared<auth::PlayerProfile>(auth::PlayerProfile{102, "Beta"}),
				  std::make_shared<auth::PlayerProfile>(auth::PlayerProfile{103, "Gamma"}),
				  std::make_shared<auth::PlayerProfile>(auth::PlayerProfile{104, "Delta"})},
		  player_ptrs{auth::PlayerProfilePtr(players[0]),
				  auth::PlayerProfilePtr(players[1]),
				  auth::PlayerProfilePtr(players[2]),
				  auth::PlayerProfilePtr(players[3])},
		  session(&seed, nullptr, 77, player_ptrs, std::move(config), true, record_manager) {
		stop_timers();
	}

	void stop_timers() {
		session.transition_timer_.stop();
		for (auto& timer : session.pending_start_timers_) {
			timer.stop();
		}
	}

	random::SeedContainer seed;
	std::array<std::shared_ptr<auth::PlayerProfile>, 4> players;
	std::array<auth::PlayerProfilePtr, 4> player_ptrs;
	ActiveSession session;
};

void StepOnce(SessionHarness& harness) {
	ASSERT_TRUE(harness.session.state_.next_transition.has_value());
	harness.session.execute_transition();
	harness.stop_timers();
}

void StepToFirstDiscard(SessionHarness& harness) {
	for (int step = 0; step < 18; ++step) {
		StepOnce(harness);
	}
	ASSERT_TRUE(harness.session.state_.next_transition.has_value());
	EXPECT_EQ(harness.session.state_.next_transition->kind, EventKind::kDiscardTile);
	EXPECT_EQ(harness.session.state_.next_transition->actor_seat, 0);
	EXPECT_EQ(harness.session.seats_[0].pending, PendingStatus::kPendingPrimary);
}

auto BuildDiscardInput(mahjong::tile_t tile, std::uint64_t stage_counter) -> Json::Value {
	Json::Value message(Json::objectValue);
	message["type"] = "game.input";
	Json::Value payload(Json::objectValue);
	payload["kind"] = "discard_tile";
	payload["tile"] = Json::UInt(static_cast<unsigned int>(tile));
	payload["use_drawn_tile"] = true;
	payload["stage_counter"] = Json::UInt64(stage_counter);
	message["payload"] = std::move(payload);
	return message;
}

auto FindAvailableAction(const Json::Value& snapshot, const char* kind) -> const Json::Value* {
	const Json::Value& actions = snapshot["viewer"]["available_actions"];
	if (!actions.isArray()) {
		return nullptr;
	}

	for (const auto& action : actions) {
		if (action.isObject() && action["kind"].isString() && action["kind"].asString() == kind) {
			return &action;
		}
	}

	return nullptr;
}

TEST(ActiveSessionTest, RecomputesWaitOptionsDuringInitialDeal) {
	SessionHarness harness;
	StepToFirstDiscard(harness);

	for (int seat = 0; seat < 4; ++seat) {
		std::vector<mahjong::meld> melds;
		for (const auto& wrapper : harness.session.seats_[seat].melds) {
			melds.push_back(wrapper.meld_value);
		}
		mahjong::hand hand(harness.session.seats_[seat].hand_tiles, melds, mahjong::tile::invalid);
		auto waits = mahjong::utils::all_waits(hand, qingque::is_winning_hand);
		EXPECT_EQ(harness.session.seats_[seat].wait_options.size(), waits.size());
		for (mahjong::tile_t wait_tile : waits) {
			EXPECT_TRUE(harness.session.seats_[seat].wait_options.contains(wait_tile));
		}
	}
}

TEST(ActiveSessionTest, ForcedDiscardRecordsDiscardPileAndCountsAfk) {
	SessionHarness harness;
	StepToFirstDiscard(harness);

	const int actor_seat = harness.session.state_.next_transition->actor_seat;
	const mahjong::tile_t drawn_tile = harness.session.seats_[actor_seat].drawn_tile;
	const int afk_before = harness.session.seats_[actor_seat].afk_counter;

	StepOnce(harness);

	ASSERT_FALSE(harness.session.transition_queue_.empty());
	EXPECT_EQ(harness.session.transition_queue_.back().kind, EventKind::kDiscardTile);
	ASSERT_EQ(harness.session.seats_[actor_seat].discard_pile.size(), 1U);
	EXPECT_EQ(harness.session.seats_[actor_seat].discard_pile.back(), drawn_tile);
	EXPECT_EQ(harness.session.seats_[actor_seat].afk_counter, afk_before + 1);
	EXPECT_FALSE(harness.session.seats_[actor_seat].has_drawn_tile());
}

TEST(ActiveSessionTest, RejectsWrongSeatAndStaleDiscardMessages) {
	SessionHarness harness;
	StepToFirstDiscard(harness);

	const int actor_seat = harness.session.state_.next_transition->actor_seat;
	const mahjong::tile_t drawn_tile = harness.session.seats_[actor_seat].drawn_tile;
	const std::uint64_t stage_counter = harness.session.state_.stage_counter;
	const Json::Value message = BuildDiscardInput(drawn_tile, stage_counter);

	auto wrong_seat = harness.session.handle_message(
		harness.players[(actor_seat + 1) % 4]->player_id,
		message);
	EXPECT_FALSE(wrong_seat.ok());
	EXPECT_EQ(wrong_seat.code(), util::StatusCode::kInvalidArgument);

	auto stale_message = BuildDiscardInput(drawn_tile, stage_counter - 1);
	auto stale = harness.session.handle_message(
		harness.players[actor_seat]->player_id,
		stale_message);
	EXPECT_FALSE(stale.ok());
	EXPECT_EQ(stale.code(), util::StatusCode::kInvalidArgument);
	EXPECT_EQ(stale.message(), "event is outdated");
}

TEST(ActiveSessionTest, SnapshotReportsRemainingDecisionTimer) {
	SessionHarness harness;
	StepToFirstDiscard(harness);

	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	harness.session.seats_[0].pending = PendingStatus::kPendingPrimary;
	harness.session.seats_[0].pending_from_ms = now - 1500;
	harness.session.seats_[0].auxiliary_ms = 2500;

	const Json::Value snapshot = harness.session.build_snapshot_for_player(0);
	ASSERT_TRUE(snapshot["viewer"]["decision_timer_ms"].isInt());

	const int remaining_ms = snapshot["viewer"]["decision_timer_ms"].asInt();
	const int expected_ms =
		GameConfig::with_margin(harness.session.config_.primary_timer_ms) + 2500 - 1500 -
		GameConfig::network_delay_ms;
	EXPECT_GE(remaining_ms, expected_ms - 100);
	EXPECT_LE(remaining_ms, expected_ms + 100);
}

TEST(ActiveSessionTest, PrimaryWaitSchedulerIncludesSeatAuxiliaryTime) {
	SessionHarness harness;
	harness.session.seats_[0].auxiliary_ms = 2500;
	harness.session.state_.stage_counter = 41;

	harness.session.process_transition(Event{
		.kind = EventKind::kDrawTile,
		.actor_seat = 0,
		.tile = 0b01000001,
		.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(),
	});

	ASSERT_TRUE(harness.session.transition_timer_.isRunning());
	const auto remaining_ms = harness.session.transition_timer_.remainingMs();
	const auto expected_ms = static_cast<std::uint64_t>(
		GameConfig::with_margin(harness.session.config_.primary_timer_ms) + 2500);
	EXPECT_GE(remaining_ms, expected_ms > 150 ? expected_ms - 150 : 0);
	EXPECT_LE(remaining_ms, expected_ms + 150);
	harness.stop_timers();
}

TEST(ActiveSessionTest, DelayedDiscardExtendsTransitionTimerUntilDelayedBroadcast) {
	SessionHarness harness;
	harness.session.state_.stage_counter = 21;
	harness.session.seats_[2].avail_melds_other = MeldOpFilter::kPung;

	const auto discard_timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();

	const Event discard{
		.kind = EventKind::kDiscardTile,
		.actor_seat = 1,
		.tile = 0b01000001,
		.timestamp_ms = discard_timestamp_ms,
	};
	harness.session.process_transition(discard);

	ASSERT_TRUE(harness.session.transition_timer_.isRunning());
	const auto remaining_ms = harness.session.transition_timer_.remainingMs();
	const auto minimum_expected_ms = static_cast<std::uint64_t>(GameConfig::meld_offset_ms - 150);
	EXPECT_GE(remaining_ms, minimum_expected_ms);
	harness.stop_timers();
}

TEST(ActiveSessionTest, AfkPrimaryWaitFallsBackToMinimalTransitionDelay) {
	SessionHarness harness;
	harness.session.seats_[0].afk_counter = GameConfig::afk_timeout_times + 1;
	harness.session.seats_[0].auxiliary_ms = 5000;
	harness.session.state_.stage_counter = 42;

	harness.session.process_transition(Event{
		.kind = EventKind::kDrawTile,
		.actor_seat = 0,
		.tile = 0b01000001,
		.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(),
	});

	EXPECT_EQ(harness.session.seats_[0].pending, PendingStatus::kPendingNone);
	ASSERT_TRUE(harness.session.transition_timer_.isRunning());
	const auto remaining_ms = harness.session.transition_timer_.remainingMs();
	const auto expected_ms = static_cast<std::uint64_t>(GameConfig::minimal_transition_ms);
	EXPECT_GE(remaining_ms, expected_ms > 150 ? expected_ms - 150 : 0);
	EXPECT_LE(remaining_ms, expected_ms + 150);
	harness.stop_timers();
}

TEST(ActiveSessionTest, SnapshotIncludesTriggerTileForReactionActions) {
	SessionHarness harness;
	constexpr mahjong::tile_t trigger_tile = 0b01000101;

	harness.session.transition_queue_.push_back(Event{
		.kind = EventKind::kDiscardTile,
		.actor_seat = 0,
		.tile = trigger_tile,
	});
	harness.session.seats_[1].pending = PendingStatus::kPendingPrimary;
	harness.session.seats_[1].pending_from_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	harness.session.seats_[1].avail_melds_other =
		MeldOpFilter::kChows[2] | MeldOpFilter::kPung | MeldOpFilter::kDiscardWin;

	const Json::Value snapshot = harness.session.build_snapshot_for_player(1);
	const Json::Value* chow = FindAvailableAction(snapshot, "chow");
	const Json::Value* pung = FindAvailableAction(snapshot, "pung");
	const Json::Value* discard_win = FindAvailableAction(snapshot, "discard_win");
	const Json::Value* pass = FindAvailableAction(snapshot, "pass");

	ASSERT_NE(nullptr, chow);
	ASSERT_NE(nullptr, pung);
	ASSERT_NE(nullptr, discard_win);
	ASSERT_NE(nullptr, pass);
	EXPECT_EQ(static_cast<unsigned int>(trigger_tile), chow->operator[]("tile").asUInt());
	EXPECT_EQ(static_cast<unsigned int>(trigger_tile), pung->operator[]("tile").asUInt());
	EXPECT_EQ(static_cast<unsigned int>(trigger_tile), discard_win->operator[]("tile").asUInt());
	EXPECT_TRUE((*pass)["tile"].isNull());
}

TEST(ActiveSessionTest, IncrementalEventMessageOmitsFullSnapshot) {
	SessionHarness harness;
	StepToFirstDiscard(harness);

	const Json::Value message = harness.session.build_event_message_for_player(
		0,
		Event{
			.kind = EventKind::kDiscardTile,
			.actor_seat = 0,
			.tile = harness.session.seats_[0].drawn_tile,
			.use_drawn_tile = true,
			.forced = false,
		},
		"transition");

	ASSERT_TRUE(message.isObject());
	ASSERT_TRUE(message["payload"].isObject());
	EXPECT_FALSE(message["payload"].isMember("session"));
	EXPECT_TRUE(message["payload"]["state"].isObject());
	EXPECT_TRUE(message["payload"]["viewer"].isObject());
	EXPECT_TRUE(message["payload"]["seat_status"].isArray());
}

TEST(ActiveSessionTest, WinPayloadIncludesExplicitFanNames) {
	SessionHarness harness;
	Event event;
	event.kind = EventKind::kSelfDrawnWin;
	event.actor_seat = 0;
	event.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	event.win_data = WinData{
		.h = mahjong::hand({}, {}, mahjong::tile::invalid),
		.win_fan = 88,
		.win_base_point = 7744,
		.win_fans = {"大三元", "字一色"},
	};
	const Json::Value message = harness.session.build_event_message_for_player(
		0,
		event,
		"transition");

	ASSERT_TRUE(message["payload"]["event"]["win"]["win_fans"].isArray());
	EXPECT_EQ("大三元", message["payload"]["event"]["win"]["win_fans"][0].asString());
	EXPECT_EQ("字一色", message["payload"]["event"]["win"]["win_fans"][1].asString());
}

TEST(ActiveSessionTest, EndSessionWritesAbortRecordWhenRecorded) {
	auto temp = storage::MakeTemporaryDirectoryPath("mmcr_session_record_abort");
	ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

	const auto records_root = temp.value().path();
	std::string session_identifier;
	{
		storage::GameRecordManager manager(records_root);
		GameConfig config;
		config.recorded = true;
		SessionHarness harness(&manager, config);
		StepToFirstDiscard(harness);
		session_identifier = harness.session.session_identifier_;
		harness.session.end_session(12345);
		harness.stop_timers();
	}

	const auto record_path = records_root / session_identifier / "1.json";
	ASSERT_TRUE(std::filesystem::exists(record_path));

	std::ifstream stream(record_path);
	ASSERT_TRUE(stream.is_open());

	Json::CharReaderBuilder builder;
	Json::Value parsed;
	std::string errors;
	ASSERT_TRUE(Json::parseFromStream(builder, stream, &parsed, &errors)) << errors;
	EXPECT_EQ(parsed["version"].asInt(), 2);
	EXPECT_EQ(parsed["header"]["session_identifier"].asString(), session_identifier);
	EXPECT_EQ(parsed["header"]["round_number"].asUInt64(), 1);
	EXPECT_TRUE(parsed["round_start_snapshot"].isObject());
	ASSERT_TRUE(parsed["round_result"].isObject());
	EXPECT_FALSE(parsed["round_result"]["completed"].asBool());
	EXPECT_EQ(parsed["round_result"]["terminal_kind"].asString(), "end");
	EXPECT_EQ(parsed["round_result"]["turn"].asInt64(), 1);
	EXPECT_EQ(parsed["round_result"]["total_turn"].asInt64(), 1);
	ASSERT_TRUE(parsed["round_result"]["meld_count"].isArray());
	EXPECT_EQ(parsed["round_result"]["meld_count"].size(), 4U);
	EXPECT_TRUE(parsed["initial_seats"].isArray());
	EXPECT_TRUE(parsed["transition_queue"].isArray());
	EXPECT_TRUE(parsed["event_queue"].isArray());
	ASSERT_FALSE(parsed["transition_queue"].empty());
	ASSERT_TRUE(parsed["transition_queue"][0]["round_turn"].isInt64());
	EXPECT_EQ(parsed["transition_queue"][0]["round_turn"].asInt64(), 0);
	const Json::Value& last_transition =
		parsed["transition_queue"][parsed["transition_queue"].size() - 1];
	EXPECT_EQ(last_transition["kind"].asString(), "end");
	EXPECT_EQ(last_transition["round_total_turn"].asInt64(), 1);
	ASSERT_TRUE(last_transition["final_scores"].isArray());
	EXPECT_EQ(last_transition["final_scores"].size(), 4U);
	ASSERT_FALSE(parsed["initial_seats"].empty());
	std::unordered_set<std::int64_t> recorded_player_ids;
	for (const auto& seat : parsed["initial_seats"]) {
		recorded_player_ids.insert(seat["player_id"].asInt64());
	}
	EXPECT_EQ(recorded_player_ids.size(), 4U);
	EXPECT_TRUE(recorded_player_ids.contains(101));
	EXPECT_TRUE(recorded_player_ids.contains(102));
	EXPECT_TRUE(recorded_player_ids.contains(103));
	EXPECT_TRUE(recorded_player_ids.contains(104));
}

TEST(ActiveSessionTest, EndSessionSkipsRecordWritesWhenRecordingDisabled) {
	auto temp = storage::MakeTemporaryDirectoryPath("mmcr_session_record_disabled");
	ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

	const auto records_root = temp.value().path();
	{
		storage::GameRecordManager manager(records_root);
		GameConfig config;
		config.recorded = false;
		SessionHarness harness(&manager, config);
		StepToFirstDiscard(harness);
		harness.session.end_session(12345);
		harness.stop_timers();
	}

	EXPECT_TRUE(std::filesystem::is_empty(records_root));
}

TEST(SeatTest, LeaveKeepsExistingAfkCounterWhenDisconnecting) {
	Seat seat;
	seat.afk_counter = GameConfig::afk_timeout_times + 3;

	seat.leave();

	EXPECT_EQ(seat.afk_counter, GameConfig::afk_timeout_times + 3);
	EXPECT_TRUE(seat.disconnected);
}

TEST(ActiveSessionTest, FinalReplayRecordIncludesEndTransitionAndScores) {
	auto temp = storage::MakeTemporaryDirectoryPath("mmcr_session_record_end");
	ASSERT_TRUE(temp.ok()) << temp.status().DebugString();

	const auto records_root = temp.value().path();
	std::string session_identifier;
	{
		storage::GameRecordManager manager(records_root);
		GameConfig config;
		config.recorded = true;
		SessionHarness harness(&manager, config);
		StepToFirstDiscard(harness);
		session_identifier = harness.session.session_identifier_;

		Event end_event;
		end_event.kind = EventKind::kEnd;
		end_event.actor_seat = 0;
		end_event.timestamp_ms = 23456;
		for (int seat = 0; seat < 4; ++seat) {
			end_event.final_scores.push_back(harness.session.seats_[seat].score);
		}
		harness.session.end_session(end_event.timestamp_ms, false);
		harness.session.process_transition(end_event);
		harness.session.enqueue_current_round_record();
		harness.stop_timers();
	}

	const auto record_path = records_root / session_identifier / "1.json";
	ASSERT_TRUE(std::filesystem::exists(record_path));

	std::ifstream stream(record_path);
	ASSERT_TRUE(stream.is_open());

	Json::CharReaderBuilder builder;
	Json::Value parsed;
	std::string errors;
	ASSERT_TRUE(Json::parseFromStream(builder, stream, &parsed, &errors)) << errors;
	EXPECT_EQ(parsed["version"].asInt(), 2);
	ASSERT_TRUE(parsed["round_result"].isObject());
	EXPECT_FALSE(parsed["round_result"]["completed"].asBool());
	EXPECT_EQ(parsed["round_result"]["terminal_kind"].asString(), "end");
	ASSERT_TRUE(parsed["transition_queue"].isArray());
	ASSERT_FALSE(parsed["transition_queue"].empty());

	const Json::Value& last_transition = parsed["transition_queue"][parsed["transition_queue"].size() - 1];
	EXPECT_EQ(last_transition["kind"].asString(), "end");
	EXPECT_EQ(last_transition["round_total_turn"].asInt64(), 1);
	ASSERT_TRUE(last_transition["final_scores"].isArray());
	EXPECT_EQ(last_transition["final_scores"].size(), 4U);
}

TEST(ActiveSessionTest, PredrawEventIncludesVisibleAndHiddenTilesByViewer) {
	SessionHarness harness;
	const Event predraw{
		.kind = EventKind::kPredraw,
		.actor_seat = 1,
		.drawn_tiles = {0b01000001, 0b01100001, 0b11000001, 0b10100001},
	};

	const Json::Value actor_message = harness.session.build_event_message_for_player(1, predraw, "transition");
	const Json::Value other_message = harness.session.build_event_message_for_player(0, predraw, "transition");

	ASSERT_TRUE(actor_message["payload"]["event"]["drawn_tiles"].isArray());
	ASSERT_TRUE(other_message["payload"]["event"]["drawn_tiles"].isArray());
	EXPECT_EQ(4U, actor_message["payload"]["event"]["drawn_tiles"].size());
	EXPECT_EQ(4U, other_message["payload"]["event"]["drawn_tiles"].size());
	EXPECT_EQ(0b01000001U, actor_message["payload"]["event"]["drawn_tiles"][0].asUInt());
	EXPECT_EQ(0U, other_message["payload"]["event"]["drawn_tiles"][0].asUInt());
}

TEST(ActiveSessionTest, WallDrawConsumesTilesFromBothEnds) {
	Wall wall;
	wall.prepare({12345}, {});

	EXPECT_EQ(136U, wall.size());
	ASSERT_TRUE(wall.draw_front().ok());
	EXPECT_EQ(135U, wall.size());
	ASSERT_TRUE(wall.draw_front().ok());
	EXPECT_EQ(134U, wall.size());
	ASSERT_TRUE(wall.draw_back().ok());
	EXPECT_EQ(133U, wall.size());
	ASSERT_TRUE(wall.draw_back().ok());
	EXPECT_EQ(132U, wall.size());
}

}  // namespace
}  // namespace mmcr::game