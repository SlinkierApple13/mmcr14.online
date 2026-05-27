#include "game/engine/session.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string_view>
#include <utility>

#include "game/hub/hub.h"
#include "random/seed.h"
#include "storage/game_record.h"
#include "external/qingque/rules/qingque.h"
#include "external/qingque/rules/w_data.h"

namespace mmcr::game {
namespace {

auto now_ms() -> std::int64_t {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

auto now_ns() -> std::uint64_t {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

// Returns a set of initial tiles for debug mode.
auto DebugInitialTiles(random::SeedContainer* seeder) -> std::vector<mahjong::tile_t> {
	using namespace mahjong::tile_literals;
	using namespace mahjong::honours;
	static std::mt19937_64 rng(seeder->Extract());
	static std::uniform_int_distribution<int> dist(1, 9);
	const int roll = dist(rng);
	switch (roll) {
		case 1: return { 1_m, 2_m, 3_m, 4_m, 5_m, 6_m, 7_m, 8_m, 9_m };
		case 2: return { 1_p, 2_p, 3_p, 4_p, 5_p, 6_p, 7_p, 8_p, 9_p };
		case 3: return { 1_s, 2_s, 3_s, 4_s, 5_s, 6_s, 7_s, 8_s, 9_s };
		case 4: return { 1_m, 2_m, 3_m, 1_p, 2_p, 3_p, 1_s, 2_s, 3_s };
		case 5: return { 4_m, 5_m, 6_m, 4_p, 5_p, 6_p, 4_s, 5_s, 6_s };
		case 6: return { 7_m, 8_m, 9_m, 7_p, 8_p, 9_p, 7_s, 8_s, 9_s };
		default: return { 1_m, 9_m, 1_p, 9_p, 1_s, 9_s, E, S, W, N, C, F, P };
	}
}

auto EventKindName(EventKind kind) -> std::string_view;
auto SerializeWinData(const WinData& data) -> Json::Value;

auto SerializeRecordGameConfig(const GameConfig& config) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["primary_timer_ms"] = config.primary_timer_ms;
	payload["secondary_timer_ms"] = config.secondary_timer_ms;
	payload["auxiliary_timer_ms"] = config.auxiliary_timer_ms;
	payload["round_count"] = config.round_count;
	payload["seat_shuffle_period"] = config.seat_shuffle_period;
	payload["recorded"] = config.recorded;
	payload["debug_mode"] = config.debug_mode;
	return payload;
}

auto SerializeRecordTiles(const std::vector<mahjong::tile_t>& tiles) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto tile : tiles) {
		payload.append(Json::UInt(static_cast<unsigned int>(tile)));
	}
	return payload;
}

auto SerializeRecordRoundStartSnapshot(const RoundStartSnapshot& snapshot) -> Json::Value {
	Json::Value payload(Json::objectValue);
	if (snapshot.seat_shuffle_seed.has_value()) {
		payload["seat_shuffle_seed"] = Json::UInt64(*snapshot.seat_shuffle_seed);
	} else {
		payload["seat_shuffle_seed"] = Json::Value(Json::nullValue);
	}
	Json::Value wall_seeds(Json::arrayValue);
	for (const auto seed : snapshot.wall_seeds) {
		wall_seeds.append(Json::UInt64(seed));
	}
	payload["wall_seeds"] = std::move(wall_seeds);
	Json::Value player_ids(Json::arrayValue);
	for (const auto player_id : snapshot.player_ids) {
		player_ids.append(Json::Int64(player_id));
	}
	payload["player_ids"] = std::move(player_ids);
	return payload;
}

auto StartsStoredRoundTurn(EventKind kind) -> bool {
	return kind == EventKind::kDrawTile || kind == EventKind::kChow ||
		kind == EventKind::kPung || kind == EventKind::kMeldedKong;
}

auto CountsAsStoredMeld(EventKind kind) -> bool {
	return kind == EventKind::kChow || kind == EventKind::kPung ||
		kind == EventKind::kMeldedKong || kind == EventKind::kAddedKong ||
		kind == EventKind::kConcealedKong;
}

void AdvanceStoredRoundTurn(int next_actor, int* current_actor, std::int64_t* turn) {
	if (*current_actor == next_actor) {
		return;
	}
	int idx = *current_actor;
	do {
		idx = (idx + 1) % 4;
		if (idx == 0) {
			++(*turn);
		}
	} while (idx != next_actor);
	*current_actor = next_actor;
}

auto SerializeStringArray(const std::vector<std::string>& values) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto& value : values) {
		payload.append(value);
	}
	return payload;
}

auto SerializeFanCodeArray(const std::vector<qingque::fan_code>& fan_codes) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto& fan_code : fan_codes) {
		payload.append(fan_code.to_string());
	}
	return payload;
}

auto SerializeMeldCount(const std::array<int, 4>& meld_count) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const int count : meld_count) {
		payload.append(count);
	}
	return payload;
}

auto IsRoundResultTerminal(EventKind kind) -> bool {
	return kind == EventKind::kDiscardWin ||
		kind == EventKind::kRobAddedKongWin ||
		kind == EventKind::kSelfDrawnWin ||
		kind == EventKind::kDrawnGame;
}

auto FindRecordRoundResultTransition(const std::vector<Event>& transitions,
							 std::size_t start_index) -> const Event* {
	if (start_index >= transitions.size()) {
		return nullptr;
	}
	for (std::size_t index = transitions.size(); index > start_index; --index) {
		const Event& transition = transitions[index - 1];
		if (IsRoundResultTerminal(transition.kind)) {
			return &transition;
		}
	}
	return &transitions.back();
}

auto SerializeRecordRoundResult(const Event& terminal,
						const std::array<int, 4>& meld_count,
						std::int64_t total_turn) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["completed"] = terminal.kind != EventKind::kEnd;
	payload["terminal_kind"] = std::string(EventKindName(terminal.kind));
	payload["drawn_game"] = terminal.kind == EventKind::kDrawnGame;
	payload["turn"] = Json::Int64(total_turn);
	payload["total_turn"] = Json::Int64(total_turn);
	payload["time_ms"] = Json::Int64(terminal.timestamp_ms);
	payload["meld_count"] = SerializeMeldCount(meld_count);

	auto set_null = [&payload](const char* field_name) {
		payload[field_name] = Json::Value(Json::nullValue);
	};

	if (terminal.kind == EventKind::kDiscardWin ||
		terminal.kind == EventKind::kRobAddedKongWin ||
		terminal.kind == EventKind::kSelfDrawnWin) {
		payload["winner_seat"] = terminal.actor_seat;
		if (terminal.result_source_actor.has_value()) {
			payload["from_seat"] = *terminal.result_source_actor;
		} else {
			set_null("from_seat");
		}
		if (terminal.win_type_bits.has_value()) {
			payload["win_type_bits"] = Json::UInt64(*terminal.win_type_bits);
		} else {
			payload["win_type_bits"] = Json::UInt64(0);
		}
		if (terminal.tile.has_value()) {
			payload["win_tile"] = Json::UInt(static_cast<unsigned int>(*terminal.tile));
		} else {
			set_null("win_tile");
		}
		if (terminal.win_data.has_value()) {
			payload["fan"] = terminal.win_data->win_fan;
			payload["fan_results"] = SerializeFanCodeArray(terminal.win_data->win_fan_codes);
			payload["fan_names"] = SerializeStringArray(terminal.win_data->win_fans);
		} else {
			payload["fan"] = 0.0;
			payload["fan_results"] = Json::Value(Json::arrayValue);
			payload["fan_names"] = Json::Value(Json::arrayValue);
		}
		return payload;
	}

	set_null("winner_seat");
	set_null("from_seat");
	set_null("win_tile");
	payload["win_type_bits"] = Json::UInt64(0);
	payload["fan"] = 0.0;
	payload["fan_results"] = Json::Value(Json::arrayValue);
	payload["fan_names"] = Json::Value(Json::arrayValue);
	return payload;
}

auto SerializeRecordEvent(const Event& event,
				  std::optional<std::int64_t> round_total_turn = std::nullopt) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["kind"] = std::string(EventKindName(event.kind));
	payload["actor_seat"] = event.actor_seat;
	payload["timestamp_ms"] = Json::Int64(event.timestamp_ms);
	payload["stage_counter"] = Json::UInt64(event.stage_counter);
	if (event.round_turn.has_value()) {
		payload["round_turn"] = Json::Int64(*event.round_turn);
	}
	if (round_total_turn.has_value()) {
		payload["round_total_turn"] = Json::Int64(*round_total_turn);
	}
	if (event.tile.has_value()) {
		payload["tile"] = Json::UInt(static_cast<unsigned int>(*event.tile));
	}
	if (event.use_drawn_tile.has_value()) {
		payload["use_drawn_tile"] = *event.use_drawn_tile;
	}
	if (event.draw_from_back.has_value()) {
		payload["draw_from_back"] = *event.draw_from_back;
	}
	if (event.forced.has_value()) {
		payload["forced"] = *event.forced;
	}
	if (!event.drawn_tiles.empty()) {
		payload["drawn_tiles"] = SerializeRecordTiles(event.drawn_tiles);
	}
	if (event.ui64_value.has_value()) {
		payload["ui64_value"] = Json::UInt64(*event.ui64_value);
	}
	if (event.round_start_snapshot.has_value()) {
		payload["round_start_snapshot"] =
			SerializeRecordRoundStartSnapshot(*event.round_start_snapshot);
	}
	if (event.win_data.has_value()) {
		payload["win_data"] = SerializeWinData(*event.win_data);
	}
	if (event.win_type_bits.has_value()) {
		payload["win_type_bits"] = Json::UInt64(*event.win_type_bits);
	}
	if (event.result_source_actor.has_value()) {
		payload["result_source_actor"] = *event.result_source_actor;
	}
	if (!event.revealed_hand_tiles.empty()) {
		payload["revealed_hand_tiles"] = SerializeRecordTiles(event.revealed_hand_tiles);
	}
	if (!event.final_scores.empty()) {
		Json::Value final_scores(Json::arrayValue);
		for (const auto score : event.final_scores) {
			final_scores.append(score);
		}
		payload["final_scores"] = std::move(final_scores);
	}
	return payload;
}

auto BuildWinData(const mahjong::hand& h) -> WinData {
	WinData data{ .h = h };
	auto fan_codes = qingque::evaluate_fans(h);
	const qingque::w_data& wd = qingque_wd::get_wd();
	// to get fan of a fan_code, use qingque::get_fan(wd, fan_code)
	// set win_fan be the maximum fan among the fan_codes, and put the fan_code achieving the maximum fan the first in win_fan_codes
	// also remove duplicate fan_codes
	// do not evaluate get_fan too many times as it can be expensive
	std::vector<std::pair<double, qingque::fan_code>> fan_code_fan_pairs;
	for (const auto& fan_code : fan_codes) {
		double fan = qingque::get_fan(wd, fan_code);
		fan_code_fan_pairs.emplace_back(fan, fan_code);
	}
	std::sort(fan_code_fan_pairs.begin(), fan_code_fan_pairs.end(),
			  [](const auto& a, const auto& b) { return a.first > b.first; });
	if (!fan_code_fan_pairs.empty()) {
		data.win_fan = fan_code_fan_pairs[0].first;
		const auto derepellenised = qingque::derepellenise(fan_code_fan_pairs[0].second);
		for (std::size_t index = 0; index < qingque::fans.size(); ++index) {
			if (!derepellenised.test(index)) {
				continue;
			}
			data.win_fans.push_back(qingque::fans[index].name);
		}
	}
	std::unordered_set<qingque::fan_code> seen;
	std::vector<qingque::fan_code> unique_fan_codes;
	for (const auto& pair : fan_code_fan_pairs) {
		if (seen.insert(pair.second).second) {
			unique_fan_codes.push_back(pair.second);
		}
	}
	data.win_fan_codes = std::move(unique_fan_codes);
	data.win_base_point = static_cast<int>(std::round(data.win_fan * data.win_fan));
	return data;
}

auto SortTilesForDisplay(std::vector<mahjong::tile_t> tiles) -> std::vector<mahjong::tile_t> {
	std::sort(tiles.begin(), tiles.end(), [](mahjong::tile_t left, mahjong::tile_t right) {
		const int left_offset = (left & 0b11100000u) == 0b10100000u ? 1000 : 0;
		const int right_offset = (right & 0b11100000u) == 0b10100000u ? 1000 : 0;
		return static_cast<int>(left) + left_offset > static_cast<int>(right) + right_offset;
	});
	return tiles;
}

auto SerializeScores(const std::array<int, 4>& scores) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const int score : scores) {
		payload.append(score);
	}
	return payload;
}

auto CountRemainingForViewer(const std::array<Seat, 4>& seats,
					 int viewer_seat,
					 mahjong::tile_t tile) -> int {
	if (viewer_seat < 0 || viewer_seat >= static_cast<int>(seats.size())) {
		return 0;
	}

	const Seat& viewer = seats[viewer_seat];
	int count = 4 - static_cast<int>(std::count(viewer.hand_tiles.begin(), viewer.hand_tiles.end(), tile));
	if (viewer.has_drawn_tile() && viewer.drawn_tile == tile) {
		count -= 1;
	}

	for (const Seat& seat : seats) {
		for (const auto& wrapper : seat.melds) {
			if (!wrapper.meld_value.contains({tile})) {
				continue;
			}
			switch (wrapper.meld_value.type()) {
				case mahjong::meld_type::sequence:
					count -= 1;
					break;
				case mahjong::meld_type::triplet:
					count -= 3;
					break;
				case mahjong::meld_type::kong:
					count -= 4;
					break;
			}
		}
		count -= static_cast<int>(std::count(seat.discard_pile.begin(), seat.discard_pile.end(), tile));
	}

	return std::max(0, count);
}

auto SerializeViewerWaitData(const std::array<Seat, 4>& seats, int viewer_seat) -> Json::Value {
	if (viewer_seat < 0 || viewer_seat >= static_cast<int>(seats.size())) {
		return Json::Value(Json::nullValue);
	}

	const Seat& seat = seats[viewer_seat];
	const bool discardable = (seat.hand_tiles.size() % 3 == 2) || seat.has_drawn_tile();
	Json::Value payload(Json::objectValue);
	Json::Value details(Json::arrayValue);

	if (!discardable) {
		payload["type"] = "waits";
		for (const auto& [wait_tile, fan_data] : seat.wait_options) {
			Json::Value entry(Json::objectValue);
			entry["tile"] = Json::UInt(static_cast<unsigned int>(wait_tile));
			entry["base_f"] = fan_data.w_discard.first;
			entry["selfdrawn_f"] = fan_data.w_drawn.first;
			entry["remaining_count"] = CountRemainingForViewer(seats, viewer_seat, wait_tile);
			details.append(std::move(entry));
		}
		payload["details"] = std::move(details);
		return payload;
	}

	std::vector<mahjong::meld> melds;
	melds.reserve(seat.melds.size());
	for (const auto& wrapper : seat.melds) {
		melds.push_back(wrapper.meld_value);
	}

	std::vector<mahjong::tile_t> hand_tiles = seat.hand_tiles;
	if (seat.has_drawn_tile()) {
		hand_tiles.push_back(seat.drawn_tile);
	}
	if (hand_tiles.empty()) {
		return Json::Value(Json::nullValue);
	}

	const mahjong::tile_t seat_wind = mahjong::tile_set::honour_tiles[viewer_seat];
	mahjong::hand hand(
		hand_tiles,
		melds,
		hand_tiles.back(),
		mahjong::win_type(false, false, false, false, seat_wind),
		true,
		false);
	const auto wait_data = qingque::get_all_waits(qingque_wd::get_wd(), hand);

	payload["type"] = "waits_all";
	for (const auto& [discard_tile, added_map] : wait_data) {
		Json::Value entry(Json::objectValue);
		entry["discard_tile"] = Json::UInt(static_cast<unsigned int>(discard_tile));
		Json::Value adds(Json::arrayValue);
		for (const auto& [added_tile, fans] : added_map) {
			Json::Value add_entry(Json::objectValue);
			add_entry["tile"] = Json::UInt(static_cast<unsigned int>(added_tile));
			add_entry["base_f"] = fans.first;
			add_entry["selfdrawn_f"] = fans.second;
			add_entry["remaining_count"] = CountRemainingForViewer(seats, viewer_seat, added_tile);
			adds.append(std::move(add_entry));
		}
		entry["adds"] = std::move(adds);
		details.append(std::move(entry));
	}
	payload["details"] = std::move(details);
	return payload;
}

auto FindPayload(const Json::Value& message) -> const Json::Value* {
	if (!message.isObject()) {
		return nullptr;
	}

	const Json::Value& payload = message["payload"];
	if (!payload.isObject()) {
		return nullptr;
	}
	return &payload;
}

auto ParseEventKind(std::string_view value) -> std::optional<EventKind> {
	// Certain event kinds are not expected to be sent by clients, so they are not parsed here.
	if (value == "discard_tile") {
		return EventKind::kDiscardTile;
	}
	if (value == "chow") {
		return EventKind::kChow;
	}
	if (value == "pung") {
		return EventKind::kPung;
	}
	if (value == "melded_kong") {
		return EventKind::kMeldedKong;
	}
	if (value == "added_kong") {
		return EventKind::kAddedKong;
	}
	if (value == "concealed_kong") {
		return EventKind::kConcealedKong;
	}
	if (value == "discard_win") {
		return EventKind::kDiscardWin;
	}
	if (value == "rob_added_kong_win") {
		return EventKind::kRobAddedKongWin;
	}
	if (value == "self_drawn_win") {
		return EventKind::kSelfDrawnWin;
	}
	if (value == "pass") {
		return EventKind::kPass;
	}
	if (value == "final_pass") {
		return EventKind::kFinalPass;
	}
	return std::nullopt;
}

auto ReadOptionalBool(const Json::Value& object, std::string_view name) -> std::optional<bool> {
	const Json::Value& value = object[std::string(name)];
	if (!value.isBool()) {
		return std::nullopt;
	}
	return value.asBool();
}

auto ReadOptionalUInt64(const Json::Value& object, std::string_view name)
	-> std::optional<std::uint64_t> {
	const Json::Value& value = object[std::string(name)];
	if (value.isUInt64()) {
		return value.asUInt64();
	}
	if (value.isUInt()) {
		return static_cast<std::uint64_t>(value.asUInt());
	}
	if (value.isInt64() && value.asInt64() >= 0) {
		return static_cast<std::uint64_t>(value.asInt64());
	}
	if (value.isInt() && value.asInt() >= 0) {
		return static_cast<std::uint64_t>(value.asInt());
	}
	return std::nullopt;
}

auto ReadOptionalTile(const Json::Value& object, std::string_view name)
	-> std::optional<mahjong::tile_t> {
	auto value = ReadOptionalUInt64(object, name);
	if (!value.has_value() || *value > std::numeric_limits<mahjong::tile_t>::max()) {
		return std::nullopt;
	}
	return static_cast<mahjong::tile_t>(*value);
}

auto IsPassMarginClaim(EventKind kind) -> bool {
	switch (kind) {
		case EventKind::kChow:
		case EventKind::kPung:
		case EventKind::kMeldedKong:
		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
			return true;
		default:
			return false;
	}
}

auto BuildPassAckEnvelope(std::uint64_t stage_counter) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["stage_counter"] = Json::UInt64(stage_counter);

	Json::Value envelope(Json::objectValue);
	envelope["version"] = 1;
	envelope["type"] = "game.pass.ack";
	envelope["payload"] = std::move(payload);
	return envelope;
}

}  // namespace

ActiveSession::ActiveSession(random::SeedContainer* seed_container,
							 GameHub* hub,
							 std::int64_t session_id,
							 std::array<auth::PlayerProfilePtr, 4> players,
							 GameConfig config,
							 bool public_session,
							 storage::GameRecordManager* record_manager)
	: config_(std::move(config)),
	  seed_container_(seed_container),
	  hub_(hub),
	  record_manager_(record_manager),
	  session_id_(session_id),
	  public_session_(public_session) {
	if (recording_enabled()) {
		session_init_timestamp_ns_ = now_ns();
		session_identifier_ =
			std::to_string(session_id_) + "_" + std::to_string(session_init_timestamp_ns_);
	}
	for (std::size_t i = 0; i < 4; ++i) {
		seats_[i].player = players[i];
	}
	init();
}

auto ActiveSession::recording_enabled() const -> bool {
	return config_.recorded && record_manager_ != nullptr;
}

void ActiveSession::capture_round_record_state(const Event& transition) {
	if (!recording_enabled() || !transition.round_start_snapshot.has_value()) {
		return;
	}

	current_round_turn_ = 1;
	current_round_turn_actor_ = 0;
	current_round_meld_count_.fill(0);
	current_round_start_snapshot_ = transition.round_start_snapshot;
	current_round_initial_seats_ = Json::Value(Json::arrayValue);
	for (const auto& seat : seats_) {
		Json::Value seat_payload(Json::objectValue);
		auto player = seat.player.lock();
		if (player != nullptr) {
			seat_payload["player_id"] = Json::Int64(player->player_id);
			seat_payload["player_name"] = player->username;
		} else {
			seat_payload["player_id"] = Json::Value(Json::nullValue);
			seat_payload["player_name"] = Json::Value(Json::nullValue);
		}
		seat_payload["afk_counter"] = seat.afk_counter;
		seat_payload["score"] = seat.score;
		seat_payload["disconnected"] = seat.disconnected;
		current_round_initial_seats_.append(std::move(seat_payload));
	}
	current_round_transition_start_index_ = transition_queue_.size();
	current_round_event_start_index_ = event_queue_.size();
	current_round_number_ = state_.round_counter;
	current_round_saved_ = false;
}

void ActiveSession::enqueue_current_round_record() {
	if (!recording_enabled() || current_round_saved_ || !current_round_start_snapshot_.has_value()) {
		return;
	}

	Json::Value payload(Json::objectValue);
	payload["version"] = 2;

	Json::Value header(Json::objectValue);
	header["session_identifier"] = session_identifier_;
	header["round_number"] = Json::UInt64(current_round_number_);
	header["game_config"] = SerializeRecordGameConfig(config_);
	payload["header"] = std::move(header);
	payload["round_start_snapshot"] =
		SerializeRecordRoundStartSnapshot(*current_round_start_snapshot_);
	payload["initial_seats"] = current_round_initial_seats_;
	if (const Event* round_result = FindRecordRoundResultTransition(
			transition_queue_, current_round_transition_start_index_);
		round_result != nullptr) {
		payload["round_result"] = SerializeRecordRoundResult(
			*round_result,
			current_round_meld_count_,
			current_round_turn_);
	}

	Json::Value transition_queue(Json::arrayValue);
	for (std::size_t index = current_round_transition_start_index_;
		 index < transition_queue_.size();
		 ++index) {
		transition_queue.append(SerializeRecordEvent(transition_queue_[index], current_round_turn_));
	}
	payload["transition_queue"] = std::move(transition_queue);

	Json::Value event_queue(Json::arrayValue);
	for (std::size_t index = current_round_event_start_index_; index < event_queue_.size(); ++index) {
		event_queue.append(SerializeRecordEvent(event_queue_[index], current_round_turn_));
	}
	payload["event_queue"] = std::move(event_queue);

	Json::Value ratings_array(Json::arrayValue);
	for (const auto& r : current_round_ratings_) {
		ratings_array.append(r.ToJson());
	}
	payload["ratings"] = std::move(ratings_array);

	Json::Value final_ratings_array(Json::arrayValue);
	for (const auto& r : final_round_ratings_) {
		final_ratings_array.append(r.ToJson());
	}
	payload["final_ratings"] = std::move(final_ratings_array);

	auto status = record_manager_->Enqueue(storage::GameRecordTask{
		.session_identifier = session_identifier_,
		.round_number = current_round_number_,
		.payload = std::move(payload),
	});
	if (!status.ok()) {
		std::cerr << status.DebugString() << '\n';
		return;
	}

	current_round_saved_ = true;
}

auto ActiveSession::handle_message(std::int64_t player_id, const Json::Value& message)
	-> util::Status {
	const auto seat_index = find_seat_index(player_id);
	if (!seat_index.has_value()) {
		return util::Status::NotFound("player is not in the active session");
	}

	const Json::Value* payload = FindPayload(message);
	if (payload == nullptr) {
		return util::Status::InvalidArgument("payload must be a JSON object");
	}

	const Json::Value& kind_value = (*payload)["kind"];
	if (!kind_value.isString()) {
		return util::Status::InvalidArgument("payload.kind must be a string");
	}

	const auto kind = ParseEventKind(kind_value.asString());
	if (!kind.has_value()) {
		return util::Status::InvalidArgument("payload.kind is not a supported event kind");
	}

	Event event;
	event.kind = *kind;
	event.actor_seat = *seat_index;
	event.tile = ReadOptionalTile(*payload, "tile");
	event.use_drawn_tile = ReadOptionalBool(*payload, "use_drawn_tile");
	event.draw_from_back = ReadOptionalBool(*payload, "draw_from_back");
	event.ui64_value = ReadOptionalUInt64(*payload, "ui64_value");
	event.stage_counter = ReadOptionalUInt64(*payload, "stage_counter").value_or(0);

	if (event.stage_counter <= 0) {
		return util::Status::InvalidArgument("stage_counter is required and must be a positive integer");
	}

	event.timestamp_ms = now_ms();

	if (event.kind == EventKind::kPass) {
		util::Status status;
		{
			std::lock_guard lock(state_.mutex);
			const Seat& seat = seats_[event.actor_seat];
			if (event.stage_counter == state_.stage_counter && seat.avail_melds_other != 0) {
				for (auto it = event_queue_.rbegin(); it != event_queue_.rend(); ++it) {
					if (it->stage_counter < event.stage_counter) {
						break;
					}
					if (it->stage_counter != event.stage_counter) {
						continue;
					}
					if (!IsPassMarginClaim(it->kind) || it->actor_seat == event.actor_seat) {
						continue;
					}
					if (event.timestamp_ms - it->timestamp_ms <= GameConfig::pass_margin_ms) {
						return util::Status::Ok();
					}
					break;
				}
			}
			status = handle_event(event);
		}
		if (!status.ok()) {
			return status;
		}
		(void)send_message(*seat_index, BuildPassAckEnvelope(event.stage_counter), 0);
		return util::Status::Ok();
	}

	return handle_event(event);
}

auto ActiveSession::player_leaves(std::int64_t player_id) -> util::Status {
	const auto seat_index = find_seat_index(player_id);
	if (!seat_index.has_value()) {
		return util::Status::NotFound("player is not in the active session");
	}

	Event event;
	event.kind = EventKind::kPlayerLeft;
	event.actor_seat = *seat_index;
	return handle_event(event);
}

auto ActiveSession::player_resumes(std::int64_t player_id) -> util::Status {
	const auto seat_index = find_seat_index(player_id);
	if (!seat_index.has_value()) {
		return util::Status::NotFound("player is not in the active session");
	}

	Event event;
	event.kind = EventKind::kPlayerResumed;
	event.actor_seat = *seat_index;
	return handle_event(event);
}

auto ActiveSession::build_snapshot_for_player_id(std::int64_t player_id) const
	-> util::StatusOr<Json::Value> {
	std::lock_guard lock(state_.mutex);
	const auto seat_index = find_seat_index(player_id);
	if (!seat_index.has_value()) {
		return util::Status::NotFound("player is not in the active session");
	}

	return build_snapshot_for_player(*seat_index);
}

auto ActiveSession::find_seat_index(std::int64_t player_id) const -> std::optional<int> {
	for (std::size_t index = 0; index < seats_.size(); ++index) {
		if (seats_[index].player.matches(player_id)) {
			return static_cast<int>(index);
		}
	}
	return std::nullopt;
}

auto ActiveSession::send_message(int target_seat, const Json::Value& message, int delay_ms) -> int {
	const int actual_delay_ms = std::max(0, delay_ms);
	if (hub_ == nullptr || target_seat < 0 ||
		target_seat >= static_cast<int>(seats_.size())) {
		return actual_delay_ms;
	}

	const auto player = seats_[target_seat].player.lock();
	if (player == nullptr) {
		return actual_delay_ms;
	}

	hub_->send_to_player(player->player_id, message, actual_delay_ms);
	return actual_delay_ms;
}

void ActiveSession::set_timer(int delay_ms, uint64_t stage_counter) {
	std::lock_guard lock(state_.mutex);
	if (state_.stage_counter != stage_counter && stage_counter != 0) {
		return;
	}
	const int clamped_delay_ms = std::max(
		delay_ms,
		static_cast<int>(std::max<std::int64_t>(0, next_transition_not_before_ms_ - now_ms())));
	transition_timer_.set(clamped_delay_ms, [this]() { this->execute_transition(); });
}

void ActiveSession::set_timer_extend(int delay_ms, uint64_t stage_counter) {
	std::lock_guard lock(state_.mutex);
	if (state_.stage_counter != stage_counter && stage_counter != 0) {
		return;
	}
	const int clamped_delay_ms = std::max(
		delay_ms,
		static_cast<int>(std::max<std::int64_t>(0, next_transition_not_before_ms_ - now_ms())));
	transition_timer_.set_extend(clamped_delay_ms, [this]() { this->execute_transition(); });
}

void ActiveSession::set_timer_shrink(int delay_ms, uint64_t stage_counter) {
	std::lock_guard lock(state_.mutex);
	if (state_.stage_counter != stage_counter && stage_counter != 0) {
		return;
	}
	const int clamped_delay_ms = std::max(
		delay_ms,
		static_cast<int>(std::max<std::int64_t>(0, next_transition_not_before_ms_ - now_ms())));
	transition_timer_.set_shrink(clamped_delay_ms, [this]() { this->execute_transition(); });
}

void ActiveSession::init() {
	{
		std::unique_lock lock(state_.mutex);
		state_.round_counter = 0;
		state_.stage_counter = 0;
		state_.this_priority = -1;
		state_.current_player = -1;
		scheduled_pending_.fill(std::nullopt);
		ended_ = false;
		ended_at_ms_ = 0;
		final_scores_.fill(0);
	}
	// Fetch initial ratings for all players
	if (hub_ != nullptr && hub_->transport() != nullptr) {
		std::array<std::int64_t, 4> player_ids{};
		for (int i = 0; i < 4; ++i) {
			const auto player = seats_[i].player.lock();
			player_ids[i] = (player != nullptr) ? player->player_id : 0;
		}
	// Store ratings as vector; inject into first event below
		current_round_ratings_ = hub_->transport()->get_player_ratings(player_ids);
		// Inject usernames from seats
		for (std::size_t i = 0; i < current_round_ratings_.size() && i < 4; ++i) {
			const auto player = seats_[i].player.lock();
			if (player != nullptr) {
				current_round_ratings_[i].username = player->username;
			}
		}
	}
	random_pause_rng_.seed(seed_container_->Extract());
	(void)handle_event(Event{ .kind = EventKind::kStart, .actor_seat = 0 });
	set_timer(GameConfig::minimal_transition_ms, 0);
}

void ActiveSession::end_session(std::int64_t timestamp_ms, bool enqueue_record) {
	std::lock_guard lock(state_.mutex);
	if (ended_) {
		return;
	}

	ended_ = true;
	ended_at_ms_ = timestamp_ms > 0 ? timestamp_ms : now_ms();
	state_.next_transition.reset();
	state_.current_player = -1;
	transition_timer_.stop();

	for (int seat = 0; seat < 4; ++seat) {
		scheduled_pending_[seat].reset();
		pending_start_timers_[seat].stop();
		end_wait(seat, ended_at_ms_);
		final_scores_[seat] = seats_[seat].score;
	}
	next_transition_not_before_ms_ = 0;

	if (session_end_callback_) {
		std::array<std::int64_t, 4> player_ids{};
		for (int seat = 0; seat < 4; ++seat) {
			const auto player = seats_[seat].player.lock();
			player_ids[seat] = player != nullptr ? player->player_id : 0;
		}
		session_end_callback_(session_id_, player_ids, final_scores_,
			static_cast<int>(state_.round_counter));
	}

	// Re-fetch final ratings for record storage
	if (hub_ != nullptr && hub_->transport() != nullptr && recording_enabled()) {
		std::array<std::int64_t, 4> player_ids{};
		for (int i = 0; i < 4; ++i) {
			const auto player = seats_[i].player.lock();
			player_ids[i] = (player != nullptr) ? player->player_id : 0;
		}
		final_round_ratings_ = hub_->transport()->get_player_ratings(player_ids);
	}

	if (enqueue_record) {
		const bool should_append_end_transition =
			recording_enabled() &&
			current_round_start_snapshot_.has_value() &&
			(transition_queue_.empty() || transition_queue_.back().kind != EventKind::kEnd);
		if (should_append_end_transition) {
			Event transition{
				.kind = EventKind::kEnd,
				.actor_seat = 0,
				.timestamp_ms = ended_at_ms_,
				.stage_counter = state_.stage_counter,
			};
			transition.final_scores.reserve(4);
			for (int seat = 0; seat < 4; ++seat) {
				transition.final_scores.push_back(seats_[seat].score);
			}
			transition_queue_.push_back(std::move(transition));
		}
		enqueue_current_round_record();
	}
}

void ActiveSession::start_primary_wait(int seat, std::int64_t now) {
	if (seat < 0 || seat >= static_cast<int>(seats_.size())) {
		return;
	}
	seats_[seat].pending_from_ms = now;
	seats_[seat].pending = PendingStatus::kPendingPrimary;
}

void ActiveSession::start_secondary_wait(int seat, std::int64_t now) {
	if (seat < 0 || seat >= 4) {
		return;
	}
	seats_[seat].pending_from_ms = now;
	seats_[seat].pending = PendingStatus::kPendingSecondary;
}

void ActiveSession::end_wait(int seat, std::int64_t now) {
	if (seat < 0 || seat >= 4) {
		return;
	}

	// Only deduct auxiliary time for primary waits. Also guard against pending_from_ms == 0
	// to avoid spurious deductions when end_wait is called on an already-cleared seat.
	const bool was_primary =
		seats_[seat].pending == PendingStatus::kPendingPrimary &&
		seats_[seat].pending_from_ms > 0;

	seats_[seat].pending = PendingStatus::kPendingNone;
	const std::int64_t pending_from = seats_[seat].pending_from_ms;
	seats_[seat].pending_from_ms = 0;
	scheduled_pending_[seat].reset();

	if (was_primary) {
		const int used_ms = std::max(
			0,
			static_cast<int>(now - pending_from) - GameConfig::with_margin(config_.primary_timer_ms));

		seats_[seat].auxiliary_ms -= used_ms;
		if (seats_[seat].auxiliary_ms < 0) {
			seats_[seat].auxiliary_ms = 0;
		}
	}
}

PendingStatus ActiveSession::get_pending_status(int seat, std::int64_t now) {
	if (seat < 0 || seat >= 4) {
		return PendingStatus::kPendingNone;
	}
	Seat& s = seats_[seat];
	if (s.pending_from_ms <= 0) {
		s.pending = PendingStatus::kPendingNone;
	}
	if (s.pending == PendingStatus::kPendingPrimary) {
		s.pending = (now - s.pending_from_ms > GameConfig::with_margin(config_.primary_timer_ms) + seats_[seat].auxiliary_ms) ? PendingStatus::kPendingNone : PendingStatus::kPendingPrimary;
	} else if (s.pending == PendingStatus::kPendingSecondary) {
		s.pending = (now - s.pending_from_ms > GameConfig::with_margin(config_.secondary_timer_ms)) ? PendingStatus::kPendingNone : PendingStatus::kPendingSecondary;
	}
	if (s.pending == PendingStatus::kPendingNone) {
		this->end_wait(seat, now);
	}
	return s.pending;
}

auto ActiveSession::handle_event(const Event& event) -> util::Status {
	static constexpr int kPriorityStart = 100;
	static constexpr int kPrioritySelf = 90;
	static constexpr int kPriorityChow = 10;
	static constexpr int kPriorityPung = 20;
	static constexpr int kPriorityWinDiscard = 40;

	std::unique_lock lock(state_.mutex);
	const auto now = now_ms();

	auto claim_finalised = [this, &now]() {
		for (int i = 0; i < 4; ++i) {
			if (get_pending_status(i, now) != PendingStatus::kPendingNone) {
				return false;
			}
		}
		return true;
	};

	auto time_left = [this, &now](int minimum, int minimum_after_last) {
		int time_left_ms = minimum;
		time_left_ms = std::max(time_left_ms, 
			minimum_after_last - static_cast<int>(now - (transition_queue_.empty() ? now : transition_queue_.back().timestamp_ms)));
		for (int i = 0; i < 4; ++i) {
			auto pending_status = get_pending_status(i, now);
			if (pending_status == PendingStatus::kPendingPrimary) {
				const int seat_time_left_ms = GameConfig::with_margin(config_.primary_timer_ms) + seats_[i].auxiliary_ms - static_cast<int>(now - seats_[i].pending_from_ms);
				time_left_ms = std::max(time_left_ms, seat_time_left_ms);
			} else if (pending_status == PendingStatus::kPendingSecondary) {
				const int seat_time_left_ms = GameConfig::with_margin(config_.secondary_timer_ms) - static_cast<int>(now - seats_[i].pending_from_ms);
				time_left_ms = std::max(time_left_ms, seat_time_left_ms);
			}
		}
		return time_left_ms;
	};

	auto get_relative_seat = [](int seat, int against) {
		return (seat - against + 4) % 4;
	};

	auto update_avail_melds_other = [this, get_relative_seat, &now]() {
		if (transition_queue_.empty()) {
			return;
		}
		int target_seat = -1;
		Event last_trans = transition_queue_.back();
		if (last_trans.kind == EventKind::kDiscardTile || last_trans.kind == EventKind::kAddedKong) {
			target_seat = last_trans.actor_seat;
		}
		if (target_seat == -1) {
			return;
		}
		for (int i = 0; i < 4; ++i) {
			if (i == target_seat) {
				continue;
			}
			MeldOptions& avail = seats_[i].avail_melds_other;
			int priority_chow = kPriorityChow;
			int priority_pung = kPriorityPung;
			int priority_win = kPriorityWinDiscard + get_relative_seat(target_seat, i);
			if (state_.this_priority >= priority_chow) {
				avail &= ~MeldOpFilter::kChow;
			}
			if (state_.this_priority >= priority_pung) {
				avail &= ~MeldOpFilter::kPung;
				avail &= ~MeldOpFilter::kMeldedKong;
			}
			if (state_.this_priority >= priority_win) {
				avail &= ~MeldOpFilter::kDiscardWin;
				avail &= ~MeldOpFilter::kRobAddedKongWin;
			}
			if (seats_[i].pending != PendingStatus::kPendingNone) {
				end_wait(i, now);
				if (avail != 0) {
					// reopen secondary wait for the seat
					start_secondary_wait(i, now);
				}
			}
		}
	};

	auto validity = [this, &event, now]() -> util::Status {
		// 1. check stage counter
		if (event.stage_counter != state_.stage_counter && event.stage_counter != 0) {
			if (event.stage_counter < state_.stage_counter) {
				return util::Status::InvalidArgument("event is outdated");
			} else {
				return util::Status::InvalidArgument("event is from the future");
			}
		}
		
		// 2. check actor seat
		if (event.actor_seat < 0 || event.actor_seat >= 4) {
			return util::Status::InvalidArgument("invalid actor seat");
		}

		// 3. reject server-only transition kinds
		if (event.kind == EventKind::kDrawTile || event.kind == EventKind::kPredraw ||
			event.kind == EventKind::kDrawnGame || event.kind == EventKind::kEnd) {
			return util::Status::InvalidArgument("invalid event kind");
		} 

		// 3. pass kStart, kPlayerLeft, and kPlayerResumed
		if (event.kind == EventKind::kStart || event.kind == EventKind::kPlayerLeft || event.kind == EventKind::kPlayerResumed) {
			return util::Status::Ok();
		}

		// Get the last transition event
		Event last_trans = transition_queue_.empty() ? Event{} : transition_queue_.back();

		// 4. if transition queue is empty, reject
		if (transition_queue_.empty()) {
			return util::Status::InvalidArgument("no transition history");
		}

		// 5. check the event type
		bool after_draw = last_trans.kind == EventKind::kDrawTile && last_trans.actor_seat == event.actor_seat;
		bool after_meld = (last_trans.kind == EventKind::kChow || last_trans.kind == EventKind::kPung) && last_trans.actor_seat == event.actor_seat;
		bool after_discard = last_trans.kind == EventKind::kDiscardTile && last_trans.actor_seat != event.actor_seat;
		bool after_added_kong = last_trans.kind == EventKind::kAddedKong && last_trans.actor_seat != event.actor_seat;

		if (!after_discard && !after_added_kong && !after_draw && !after_meld) {
			return util::Status::InvalidArgument("invalid state for event");
		}

		// 6. after draw or meld, only allow kDiscardTile, kAddedKong, kConcealedKong, kSelfDrawnWin
		if (after_draw || after_meld) {
			if (event.kind != EventKind::kDiscardTile && event.kind != EventKind::kAddedKong &&
				event.kind != EventKind::kConcealedKong && event.kind != EventKind::kSelfDrawnWin) {
				return util::Status::InvalidArgument("invalid event kind in current state");
			}
			if (get_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
				get_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
				return util::Status::InvalidArgument("player does not have a pending decision");
			}
			// 6a. discard
			if (event.kind == EventKind::kDiscardTile) {
				if (!event.tile.has_value()) {
					return util::Status::InvalidArgument("tile is required for discard event");
				}
				if (!event.use_drawn_tile.has_value()) {
					return util::Status::InvalidArgument("use_drawn_tile is required for discard event");
				}
				// check if the player has the tile to discard
				const Seat& seat = seats_[event.actor_seat];
				bool has_tile = false;
				if (event.use_drawn_tile.value()) {
					has_tile = seat.has_drawn_tile() && seat.drawn_tile == event.tile.value();
				} else {
					has_tile = std::find(seat.hand_tiles.begin(), seat.hand_tiles.end(), event.tile.value()) != seat.hand_tiles.end();
				}
				if (!has_tile) {
					return util::Status::InvalidArgument("player does not have the tile to discard");
				}
			}
			// 6b. added kong
			if (event.kind == EventKind::kAddedKong) {
				if (!event.tile.has_value()) {
					return util::Status::InvalidArgument("tile is required for added kong event");
				}
				if (!event.use_drawn_tile.has_value()) {
					return util::Status::InvalidArgument("use_drawn_tile is required for added kong event");
				}
				// check against pre-prepared SelfMeldOptions
				const Seat& seat = seats_[event.actor_seat];
				bool can_declare = ( event.use_drawn_tile.value() ?
					std::find(seat.avail_melds_self.akong_from_draw.begin(), 
							  seat.avail_melds_self.akong_from_draw.end(), 
							  event.tile.value()) != seat.avail_melds_self.akong_from_draw.end() :
					std::find(seat.avail_melds_self.akong_from_hand.begin(), 
					          seat.avail_melds_self.akong_from_hand.end(), 
							  event.tile.value()) != seat.avail_melds_self.akong_from_hand.end() );
				if (!can_declare) {
					return util::Status::InvalidArgument("player cannot declare added kong with the specified tile");
				}
			}
			// 6c. concealed kong
			if (event.kind == EventKind::kConcealedKong) {
				if (!event.tile.has_value()) {
					return util::Status::InvalidArgument("tile is required for concealed kong event");
				}
				if (!event.use_drawn_tile.has_value()) {
					return util::Status::InvalidArgument("use_drawn_tile is required for concealed kong event");
				}
				// check against pre-prepared SelfMeldOptions
				const Seat& seat = seats_[event.actor_seat];
				bool can_declare = ( event.use_drawn_tile.value() ?
					std::find(seat.avail_melds_self.ckong_from_draw.begin(), 
							  seat.avail_melds_self.ckong_from_draw.end(), 
							  event.tile.value()) != seat.avail_melds_self.ckong_from_draw.end() :
					std::find(seat.avail_melds_self.ckong_from_hand.begin(), 
					          seat.avail_melds_self.ckong_from_hand.end(), 
							  event.tile.value()) != seat.avail_melds_self.ckong_from_hand.end() );
				if (!can_declare) {
					return util::Status::InvalidArgument("player cannot declare concealed kong with the specified tile");
				}
			}
			// 6d. self-drawn win
			if (event.kind == EventKind::kSelfDrawnWin) {
				if (after_meld) {
					return util::Status::InvalidArgument("invalid event kind in current state");
				}
				// check against pre-prepared SelfMeldOptions
				const Seat& seat = seats_[event.actor_seat];
				if (!seat.avail_melds_self.self_drawn_win) {
					return util::Status::InvalidArgument("player cannot declare self-drawn win in the current state");
				}
			}
		}

		// 7. after discard, only allow kChow, kPung, kMeldedKong, kDiscardWin, kPass, kFinalPass
		if (after_discard) {
			if (event.kind != EventKind::kChow && event.kind != EventKind::kPung && 
				event.kind != EventKind::kMeldedKong && event.kind != EventKind::kDiscardWin &&
				event.kind != EventKind::kPass && event.kind != EventKind::kFinalPass) {
				return util::Status::InvalidArgument("invalid event kind in current state");
			}
			const Seat& seat = seats_[event.actor_seat];
			if (get_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
				get_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
				return util::Status::InvalidArgument("player does not have a pending decision");
			}
			// 7a. chow
			if (event.kind == EventKind::kChow) {
				if (!event.ui64_value.has_value()) {
					return util::Status::InvalidArgument("ui64_value is required for chow event");
				}
				if (event.ui64_value.value() < 1 || event.ui64_value.value() > 3) {
					return util::Status::InvalidArgument("invalid ui64_value for chow event");
				}
				if ((seat.avail_melds_other & MeldOpFilter::kChows[event.ui64_value.value()]) == 0) {
					return util::Status::InvalidArgument("player cannot declare chow with the specified mode");
				}
			}
			// 7b. pung
			if (event.kind == EventKind::kPung) {
				if ((seat.avail_melds_other & MeldOpFilter::kPung) == 0) {
					return util::Status::InvalidArgument("player cannot declare pung in the current state");
				}
			}
			// 7c. melded kong
			if (event.kind == EventKind::kMeldedKong) {
				if ((seat.avail_melds_other & MeldOpFilter::kMeldedKong) == 0) {
					return util::Status::InvalidArgument("player cannot declare melded kong in the current state");
				}
			}
			// 7d. discard win
			if (event.kind == EventKind::kDiscardWin) {
				if ((seat.avail_melds_other & MeldOpFilter::kDiscardWin) == 0) {
					return util::Status::InvalidArgument("player cannot declare discard win in the current state");
				}
			}
		}

		// 8. after added kong, only allow kRobAddedKongWin, kPass, kFinalPass
		if (after_added_kong) {
			if (event.kind != EventKind::kRobAddedKongWin && 
				event.kind != EventKind::kPass && event.kind != EventKind::kFinalPass) {
				return util::Status::InvalidArgument("invalid event kind in current state");
			}
			const Seat& seat = seats_[event.actor_seat];
			if (get_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
				get_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
				return util::Status::InvalidArgument("player does not have a pending decision");
			}
			// 8a. rob added kong win
			if (event.kind == EventKind::kRobAddedKongWin) {
				if ((seat.avail_melds_other & MeldOpFilter::kRobAddedKongWin) == 0) {
					return util::Status::InvalidArgument("player cannot declare rob added kong win in the current state");
				}
			}
		}

		return util::Status::Ok();
	}();

	if (!validity.ok()) {
		return validity;
	}

	Event recorded_event = event;
	recorded_event.round_turn = current_round_turn_;
	event_queue_.push_back(std::move(recorded_event));

	if (state_.stage_counter != event.stage_counter && event.stage_counter != 0) {
		return util::Status::Ok();
	}

	std::optional<EventKind> afk_status_broadcast;
	if (event.kind != EventKind::kPlayerLeft && event.kind != EventKind::kPlayerResumed) {
		const bool was_afk = seats_[event.actor_seat].is_afk();
		seats_[event.actor_seat].afk_counter = 0;
		if (was_afk && !seats_[event.actor_seat].is_afk()) {
			afk_status_broadcast = EventKind::kPlayerResumed;
		}
	}

	switch (event.kind) {
		case EventKind::kStart: {
			state_.this_priority = kPriorityStart;
			state_.next_transition = event;
		} break;

		case EventKind::kDiscardTile: {
			int event_priority = kPrioritySelf;
			if (state_.this_priority < event_priority) {
				state_.this_priority = event_priority;
				state_.next_transition = event;
				end_wait(event.actor_seat, now);
				set_timer(GameConfig::epsilon_ms, event.stage_counter);
			}
		} break;

		case EventKind::kChow: {
			int actor_seat = event.actor_seat;
			int event_priority = kPriorityChow;
			if (state_.this_priority < event_priority) {
				state_.this_priority = event_priority;
				state_.next_transition = event;
				end_wait(actor_seat, now);
				update_avail_melds_other();
				broadcast_claim(event);
				if (claim_finalised()) { // no one can override
					set_timer(GameConfig::meld_pause_ms, event.stage_counter);
				} else { // someone can still override, wait for secondary timer
					set_timer(config_.secondary_timer_ms, event.stage_counter);
				}
			}
		} break;

		case EventKind::kPung:
		case EventKind::kMeldedKong: {
			int actor_seat = event.actor_seat;
			int event_priority = kPriorityPung;
			if (state_.this_priority < event_priority) {
				state_.this_priority = event_priority;
				state_.next_transition = event;
				end_wait(actor_seat, now);
				update_avail_melds_other();
				broadcast_claim(event);
				if (claim_finalised()) {
					set_timer(GameConfig::meld_pause_ms, event.stage_counter);
				} else {
					set_timer(config_.secondary_timer_ms, event.stage_counter);
				}
			}
		} break;

		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin: {
			int actor_seat = event.actor_seat;
			int target_seat = transition_queue_.empty() ? 0 : transition_queue_.back().actor_seat;
			int event_priority = kPriorityWinDiscard + get_relative_seat(target_seat, actor_seat);
			if (state_.this_priority < event_priority) {
				state_.this_priority = event_priority;
				state_.next_transition = event;
				end_wait(actor_seat, now);
				update_avail_melds_other();
				broadcast_claim(event);
				if (claim_finalised()) {
					set_timer(GameConfig::meld_pause_ms, event.stage_counter);
				} else {
					set_timer(config_.secondary_timer_ms, event.stage_counter);
				}
			}
		} break;

		case EventKind::kAddedKong:
		case EventKind::kConcealedKong:
		case EventKind::kSelfDrawnWin: {
			int event_priority = kPrioritySelf;
			if (state_.this_priority < event_priority) {
				state_.this_priority = event_priority;
				state_.next_transition = event;
				end_wait(event.actor_seat, now);
				broadcast_claim(event);
				set_timer(GameConfig::meld_pause_ms, event.stage_counter);
			}
		} break;

		case EventKind::kPass: {
			// if no players can claim except the actor, pass. otherwise, do nothing.
			int actor_seat = event.actor_seat;
			end_wait(actor_seat, now);
			if (!claim_finalised()) {
				// open to further decision by marking as slept
				seats_[actor_seat].pending = PendingStatus::kPendingSlept;
			}
			set_timer(time_left(GameConfig::epsilon_ms, GameConfig::minimal_transition_ms), event.stage_counter);
		} break;

		case EventKind::kFinalPass: {
			// Irrevocably waives all rights to the
			// current discard.  Clear avail_melds_other so that MsgOnClaim will
			// never reopen this player with a secondary wait.
			int actor_seat = event.actor_seat;
			seats_[actor_seat].avail_melds_other = 0;
			end_wait(actor_seat, now);
			set_timer(time_left(GameConfig::epsilon_ms, GameConfig::minimal_transition_ms), event.stage_counter);
		} break;

		case EventKind::kPlayerLeft: {
			seats_[event.actor_seat].leave();
			broadcast_claim(event);
		} break;

		case EventKind::kPlayerResumed: {
			seats_[event.actor_seat].resume();
			broadcast_claim(event);
		} break;

		default: {
			return util::Status::InvalidArgument("unsupported event kind");
		}
	}

	if (afk_status_broadcast.has_value()) {
		broadcast_claim(Event{
			.kind = *afk_status_broadcast,
			.actor_seat = event.actor_seat,
			.timestamp_ms = now,
			.stage_counter = event.stage_counter,
		});
	}

	return util::Status::Ok();
}

void ActiveSession::execute_transition() {
	std::unique_lock lock(state_.mutex);
	if (!state_.next_transition.has_value()) {
		return;
	}
	Event transition = *state_.next_transition;
	state_.next_transition.reset();
	++state_.stage_counter;
	state_.this_priority = -1;
	state_.current_player = transition.actor_seat;

	// clear waiting status of all players
	const auto now = now_ms();
	for (int i = 0; i < 4; ++i) {
		end_wait(i, now);
	}

	transition.timestamp_ms = now;
	if (transition.kind == EventKind::kStart) {
		current_round_turn_ = 0;
		current_round_turn_actor_ = 0;
		current_round_meld_count_.fill(0);
	}
	transition.round_turn =
		(transition.kind == EventKind::kStart || transition.kind == EventKind::kPredraw)
			? 0
			: current_round_turn_;
	if (StartsStoredRoundTurn(transition.kind) &&
		transition.actor_seat >= 0 && transition.actor_seat < 4) {
		AdvanceStoredRoundTurn(
			transition.actor_seat,
			&current_round_turn_actor_,
			&current_round_turn_);
	}
	if (CountsAsStoredMeld(transition.kind) &&
		transition.actor_seat >= 0 && transition.actor_seat < 4) {
		++current_round_meld_count_[static_cast<std::size_t>(transition.actor_seat)];
	}

	auto recompute_wait_options = [this](int seat) {
		Seat& s = seats_[seat];
		const mahjong::tile_t seat_wind = mahjong::tile_set::wind_tiles[seat];
		// 1. extract open melds from the wrapper
		std::vector<mahjong::meld> melds;
		for (const auto& wrapper : s.melds) {
			melds.push_back(wrapper.meld_value);
		}
		// 2. get all waits
		mahjong::hand hand(s.hand_tiles, melds, mahjong::tile::invalid);
		auto all_waits = mahjong::utils::all_waits(hand, qingque::is_winning_hand);
		// 3. evaluate fans
		s.wait_options.clear();
		for (const auto& wait : all_waits) {
			const mahjong::hand discard_hand(
				s.hand_tiles,
				melds,
				wait,
				mahjong::win_type(false, false, false, false, seat_wind));
			const mahjong::hand drawn_hand(
				s.hand_tiles,
				melds,
				wait,
				mahjong::win_type(true, false, false, false, seat_wind));
			auto fan_discard = qingque::get_fan(
				qingque_wd::get_wd(),
				discard_hand
			);
			auto fan_draw = qingque::get_fan(
				qingque_wd::get_wd(),
				drawn_hand
			);
			s.wait_options.emplace(wait, FanData{fan_discard, fan_draw});
		}
	};

	auto update_self = [this, &transition](int seat) {
		Seat& s = seats_[seat];
		s.avail_melds_self = SelfMeldOptions{};
		if (s.is_afk()) {
			return;
		}
		// 1. kong options
		mahjong::tile_counter counter(s.hand_tiles);
		std::unordered_set<mahjong::tile_t> punged_tiles;
		for (const auto& wrapper : s.melds) {
			if (wrapper.meld_value.type() == mahjong::meld_type::triplet) {
				punged_tiles.insert(wrapper.meld_value.tile());
			}
		}
		for (mahjong::tile_t ti : mahjong::tile_set::all_tiles) {
			// 1a. concealed kong
			if (counter.count(ti) >= 4) {
				s.avail_melds_self.ckong_from_hand.push_back(ti);
			}
			if (s.has_drawn_tile() && s.drawn_tile == ti && counter.count(ti) >= 3) {
				s.avail_melds_self.ckong_from_draw.push_back(ti);
			}
			// 1b. added kong
			if (punged_tiles.contains(ti) && counter.count(ti) >= 1) {
				s.avail_melds_self.akong_from_hand.push_back(ti);
			}
			if (s.has_drawn_tile() && s.drawn_tile == ti && punged_tiles.contains(ti)) {
				s.avail_melds_self.akong_from_draw.push_back(ti);
			}
		}
		// 2. check win if has drawn tile
		if (transition.kind == EventKind::kDrawTile) {
			mahjong::tile_t ti = s.drawn_tile;
			// can win anyway
			if (s.wait_options.can_win_drawn(ti)) {
				s.avail_melds_self.self_drawn_win = true;
			} else {		
				// check for special conditions
				// heavenly hand is omitted here because concealed hand must be satisfied
				bool from_replacement = transition.draw_from_back.value_or(false);
				bool last_tile = wall_.empty();
				if (from_replacement || last_tile) {
					s.avail_melds_self.self_drawn_win = s.wait_options.contains(ti);
				}
			}
		}
	};

	auto update_other = [this, &transition](int seat) {
		for (int aseat = 0; aseat < 4; ++aseat) {
			Seat& s = seats_[aseat];
			s.avail_melds_other = 0;
			if (aseat == seat) {
				continue;
			}
			if (s.is_afk()) {
				continue;
			}
			// 1. if the transition is added kong, only check win
			if (transition.kind == EventKind::kAddedKong) {
				mahjong::tile_t konged_tile = transition.tile.value();
				if (s.wait_options.contains(konged_tile)) {
					s.avail_melds_other |= MeldOpFilter::kRobAddedKongWin;
				}
			} else {
				mahjong::tile_counter counter(s.hand_tiles);
				mahjong::tile_t ti = transition.tile.value();
				// 2. check chow[1], chow[2], chow[3]
				if (aseat == (seat + 1) % 4 && mahjong::tile(ti).suit() != mahjong::suit_type::z) {
					if (counter.count(ti + 1) && counter.count(ti + 2)) {
						s.avail_melds_other |= MeldOpFilter::kChows[1];
					}
					if (counter.count(ti - 1) && counter.count(ti + 1)) {
						s.avail_melds_other |= MeldOpFilter::kChows[2];
					}
					if (counter.count(ti - 2) && counter.count(ti - 1)) {
						s.avail_melds_other |= MeldOpFilter::kChows[3];
					}
				}
				// 3. check pung and melded kong
				if (counter.count(ti) >= 2) {
					s.avail_melds_other |= MeldOpFilter::kPung;
				}
				if (counter.count(ti) >= 3) {
					s.avail_melds_other |= MeldOpFilter::kMeldedKong;
				}
				// 4. check discard win
				if (s.wait_options.can_win_discard(ti)) {
					s.avail_melds_other |= MeldOpFilter::kDiscardWin;
				} else {
					// check for special conditions
					// earthly hand is omitted here because winning hand must be satisfied
					bool last_tile = wall_.empty();
					if (last_tile && s.wait_options.contains(ti)) {
						s.avail_melds_other |= MeldOpFilter::kDiscardWin;
					}
				}
			}

		}
	};

	auto get_relative_seat = [](int seat, int against) {
		return (seat - against + 4) % 4;
	};

	auto next_round_transition = [this]() {
		const auto configured_round_count = static_cast<std::uint64_t>(std::max(config_.round_count, 0));
		return Event{
			.kind = (configured_round_count != 0 && state_.round_counter >= configured_round_count)
				? EventKind::kEnd
				: EventKind::kStart,
			.actor_seat = 0,
		};
	};

	// if should draw but wall empty, mark as drawn game instead
	if (transition.kind == EventKind::kDrawTile && wall_.empty()) {
		const int original_actor = transition.actor_seat;
		const auto ts = transition.timestamp_ms;
		const auto sc = transition.stage_counter;
		const auto rt = transition.round_turn;
		transition = Event{ .kind = EventKind::kDrawnGame, .actor_seat = original_actor,
			.round_turn = rt, .timestamp_ms = ts, .stage_counter = sc };
	}

	const bool was_afk = seats_[transition.actor_seat].is_afk();
	if (transition.forced.value_or(false)) {
		++seats_[transition.actor_seat].afk_counter;
		if (!was_afk && seats_[transition.actor_seat].is_afk()) {
			broadcast_claim(Event{
				.kind = EventKind::kPlayerLeft,
				.actor_seat = transition.actor_seat,
				.timestamp_ms = transition.timestamp_ms,
				.stage_counter = state_.stage_counter,
			});
		}
	}

	switch (transition.kind) {
		case EventKind::kStart: {
			RoundStartSnapshot snapshot;
			for (int i = 0; i < 4; ++i) {
				Seat& s = seats_[i];
				s.discard_pile.clear();
				s.melds.clear();
				s.hand_tiles.clear();
				s.drawn_tile = mahjong::tile::invalid;
				s.avail_melds_self = SelfMeldOptions{};
				s.avail_melds_other = 0;
				s.wait_options.clear();
				s.pending = PendingStatus::kPendingNone;
				s.auxiliary_ms = config_.auxiliary_timer_ms;
				s.pending_from_ms = 0;
			}
			// 1. shuffle seat if needed; otherwise rotate
			if (config_.seat_shuffle_period > 0 && state_.round_counter % config_.seat_shuffle_period == 0) {
				// 1a. sort players by player id to ensure deterministic shuffling
				std::sort(seats_.begin(), seats_.end(), [](const Seat& a, const Seat& b) {
					return a.player.lock()->player_id < b.player.lock()->player_id;
				});
				// 1b. shuffle using a seed from the seed container
				auto seed = seed_container_->Extract();
				std::shuffle(seats_.begin(), seats_.end(), std::mt19937_64(seed));
				snapshot.seat_shuffle_seed = seed;
			} else {
				std::rotate(seats_.rbegin(), seats_.rbegin() + 1, seats_.rend());
				snapshot.seat_shuffle_seed = std::nullopt;
			}
			++state_.round_counter;
			// 2. record wall seeds
			snapshot.player_ids.fill(0);
			for (int i = 0; i < 4; ++i) {
				if (seats_[i].player.valid()) {
					snapshot.player_ids[i] = seats_[i].player.player_id();
				} else {
					snapshot.player_ids[i] = -1;
				}
			}
			// 3. prepare the wall using seeds from the seed container
			// shuffle 16 times to cover all possible wall states,
			// making seed cracking mid-game impossible
			for (int i = 0; i < 16; ++i) {
				auto seed = seed_container_->Extract();
				snapshot.wall_seeds.push_back(seed);
			}
			wall_.prepare(snapshot.wall_seeds, config_.debug_mode ? DebugInitialTiles(seed_container_) : std::vector<mahjong::tile_t>{});
			// 4. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kPredraw,
				.actor_seat = 0,
				.ui64_value = 0,
			};
			// 5. fill missing fields
			transition.round_start_snapshot = snapshot;
			capture_round_record_state(transition);
		} break;

		case EventKind::kPredraw: {
			// 1. draw tiles
			auto stage = transition.ui64_value.value_or(0);
			int tile_count = (stage >= 12) ? 1 : 4;
			auto tiles = wall_.draw(tile_count).value();
			int actor_seat = transition.actor_seat;
			seats_[actor_seat].hand_tiles.insert(seats_[actor_seat].hand_tiles.end(), tiles.begin(), tiles.end());
			// 2. set next transition
			if (stage < 15) {
				// continue predraw phase
				state_.next_transition = Event{
					.kind = EventKind::kPredraw,
					.actor_seat = (actor_seat + 1) % 4,
					.ui64_value = stage + 1,
				};
			} else {
				// move to main phase
				for (int s = 0; s < 4; ++s) {
					recompute_wait_options(s);
				}
				state_.next_transition = Event{
					.kind = EventKind::kDrawTile,
					.actor_seat = 0,
					.draw_from_back = false,
				};
			}
			// 3. fill missing fields
			transition.drawn_tiles = tiles;
		} break;

		case EventKind::kDrawTile: {
			// 1. draw tile
			int actor_seat = transition.actor_seat;
			bool from_back = transition.draw_from_back.value_or(false);
			auto ti = (from_back ? wall_.draw_back() : wall_.draw_front()).value();
			seats_[actor_seat].drawn_tile = ti;
			// 2. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kDiscardTile,
				.actor_seat = actor_seat,
				.tile = ti,
				.use_drawn_tile = true,
				.forced = true,
			};
			// 3. compute available melds
			update_self(actor_seat);
			// 4. fill missing fields
			transition.tile = ti;
		} break;
		
		case EventKind::kDiscardTile: {
			// 1. remove the tile
			// if from hand, remove one from hand and move drawn tile (if valid) to hand; otherwise, simply set drawn tile to invalid
			int actor_seat = transition.actor_seat;
			Seat& seat = seats_[actor_seat];
			if (transition.use_drawn_tile.value_or(false)) {
				seat.drawn_tile = mahjong::tile::invalid;
			} else {
				auto it = std::find(seat.hand_tiles.begin(), seat.hand_tiles.end(), transition.tile.value());
				if (it != seat.hand_tiles.end()) {
					seat.hand_tiles.erase(it);
				}
				if (seat.has_drawn_tile()) {
					seat.hand_tiles.push_back(seat.drawn_tile);
					seat.drawn_tile = mahjong::tile::invalid;
				}
			}
			seat.discard_pile.push_back(transition.tile.value());
			recompute_wait_options(actor_seat);
			// 2. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kDrawTile,
				.actor_seat = (actor_seat + 1) % 4,
				.draw_from_back = false,
			};
			// 3. compute available melds for other players
			update_other(actor_seat);
		} break;
		
		case EventKind::kChow: {
			// 1. get the tiles
			int actor_seat = transition.actor_seat;
			int target_seat = transition_queue_.back().actor_seat;
			mahjong::tile_t ti0 = transition_queue_.back().tile.value();
			int chow_mode = transition.ui64_value.value_or(0);
			mahjong::tile_t ti1 = mahjong::tile::invalid;
			mahjong::tile_t ti2 = mahjong::tile::invalid;
			mahjong::tile_t tic = mahjong::tile::invalid; // the central one
			if (chow_mode == 1) {
				ti1 = ti0 + 1;
				ti2 = ti0 + 2;
				tic = ti0 + 1;
			} else if (chow_mode == 2) {
				ti1 = ti0 - 1;
				ti2 = ti0 + 1;
				tic = ti0;
			} else if (chow_mode == 3) {
				ti1 = ti0 - 2;
				ti2 = ti0 - 1;
				tic = ti0 - 1;
			}
			// 2. remove the tiles
			Seat& seat = seats_[actor_seat];
			seat.remove_hand_tile(ti1);
			seat.remove_hand_tile(ti2);
			(void)seats_[target_seat].pop_discard();
			// 3. add the meld
			MeldWrapper wrapper{
				.meld_value = {tic, mahjong::meld_type::sequence, false, true},
				.chow_mode = chow_mode,
				.meld_from_rel = 3,
			};
			seat.melds.push_back(wrapper);
			// 4. set next transition
			// get the last tile in hand in ascending order of tile::value_in_order()
			mahjong::tile_t discard_ti = mahjong::tile::invalid;
			for (const auto& t : seat.hand_tiles) {
				if (discard_ti == mahjong::tile::invalid || mahjong::tile(t).value_in_order() > mahjong::tile(discard_ti).value_in_order()) {
					discard_ti = t;
				}
			}
			state_.next_transition = Event{
				.kind = EventKind::kDiscardTile,
				.actor_seat = actor_seat,
				.tile = discard_ti,
				.use_drawn_tile = false,
				.forced = true,
			};
			// 5. compute available melds for the actor
			update_self(actor_seat);
			// 6. fill missing fields
			transition.tile = tic;
		} break;
		
		case EventKind::kPung: {
			// 1. get the tile
			int actor_seat = transition.actor_seat;
			int target_seat = transition_queue_.back().actor_seat;
			mahjong::tile_t ti = transition_queue_.back().tile.value();
			// 2. remove the tiles
			Seat& seat = seats_[actor_seat];
			seat.remove_hand_tile(ti);
			seat.remove_hand_tile(ti);
			(void)seats_[target_seat].pop_discard();
			// 3. add the meld
			MeldWrapper wrapper{
				.meld_value = {ti, mahjong::meld_type::triplet, false, true},
				.meld_from_rel = get_relative_seat(actor_seat, target_seat),
			};
			seat.melds.push_back(wrapper);
			// 4. set next transition
			// get the last tile in hand in ascending order of tile::value_in_order()
			mahjong::tile_t discard_ti = mahjong::tile::invalid;
			for (const auto& t : seat.hand_tiles) {
				if (discard_ti == mahjong::tile::invalid || mahjong::tile(t).value_in_order() > mahjong::tile(discard_ti).value_in_order()) {
					discard_ti = t;
				}
			}
			state_.next_transition = Event{
				.kind = EventKind::kDiscardTile,
				.actor_seat = actor_seat,
				.tile = discard_ti,
				.use_drawn_tile = false,
				.forced = true,
			};
			// 5. compute available melds for the actor
			update_self(actor_seat);
			// 6. fill missing fields
			transition.tile = ti;
		} break;
		
		case EventKind::kMeldedKong: {
			// 1. get the tile
			int actor_seat = transition.actor_seat;
			int target_seat = transition_queue_.back().actor_seat;
			mahjong::tile_t ti = transition_queue_.back().tile.value();
			// 2. remove the tiles
			Seat& seat = seats_[actor_seat];
			seat.remove_hand_tile(ti);
			seat.remove_hand_tile(ti);
			seat.remove_hand_tile(ti);
			(void)seats_[target_seat].pop_discard();
			// 3. add the meld
			MeldWrapper wrapper{
				.meld_value = {ti, mahjong::meld_type::kong, false, true},
				.meld_from_rel = get_relative_seat(actor_seat, target_seat),
			};
			seat.melds.push_back(wrapper);
			recompute_wait_options(actor_seat);
			// 4. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kDrawTile,
				.actor_seat = actor_seat,
				.draw_from_back = true,
			};
			// 5. fill missing fields
			transition.tile = ti;
		} break;
		
		case EventKind::kAddedKong: {
			// 1. get the tile
			int actor_seat = transition.actor_seat;
			mahjong::tile_t ti = transition.tile.value();
			// 2. remove the tile; if from hand, move drawn tile to hand first
			Seat& seat = seats_[actor_seat];
			if (transition.use_drawn_tile.value_or(false)) {
				seat.drawn_tile = mahjong::tile::invalid;
			} else {
				if (seat.has_drawn_tile()) {
					seat.hand_tiles.push_back(seat.drawn_tile);
					seat.drawn_tile = mahjong::tile::invalid;
				}
				seat.remove_hand_tile(ti);
			}
			// 3. find and modify the meld
			for (auto& wrapper : seat.melds) {
				if (wrapper.meld_value.type() == mahjong::meld_type::triplet && wrapper.meld_value.tile() == ti) {
					if (wrapper.meld_from_rel > 0 && wrapper.meld_from_rel < 4) {
						wrapper.meld_from_rel += 4;
					}
					wrapper.meld_value = {ti, mahjong::meld_type::kong, false, true};
					break;
				}
			}
			recompute_wait_options(actor_seat);
			// 4. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kDrawTile,
				.actor_seat = actor_seat,
				.draw_from_back = true,
			};
			// 5. compute available melds for other players
			update_other(actor_seat);
		} break;
		
		case EventKind::kConcealedKong: {
			// 1. get the tile
			int actor_seat = transition.actor_seat;
			mahjong::tile_t ti = transition.tile.value();
			// 2. remove the tiles; if from hand, move drawn tile to hand first
			Seat& seat = seats_[actor_seat];
			if (transition.use_drawn_tile.value_or(false)) {
				seat.drawn_tile = mahjong::tile::invalid;
				seat.remove_hand_tile(ti);
				seat.remove_hand_tile(ti);
				seat.remove_hand_tile(ti);
			} else {
				if (seat.has_drawn_tile()) {
					seat.hand_tiles.push_back(seat.drawn_tile);
					seat.drawn_tile = mahjong::tile::invalid;
				}
				seat.remove_hand_tile(ti);
				seat.remove_hand_tile(ti);
				seat.remove_hand_tile(ti);
				seat.remove_hand_tile(ti);
			}
			// 3. add the meld
			MeldWrapper wrapper{
				.meld_value = {ti, mahjong::meld_type::kong, true, true},
				.meld_from_rel = 0,
			};
			seat.melds.push_back(wrapper);
			recompute_wait_options(actor_seat);
			// 4. set next transition
			state_.next_transition = Event{
				.kind = EventKind::kDrawTile,
				.actor_seat = actor_seat,
				.draw_from_back = true,
			};
		} break;
		
		case EventKind::kDiscardWin: {
			// get the tile last discarded
			mahjong::tile_t ti = transition_queue_.back().tile.value();
			transition.result_source_actor = transition_queue_.back().actor_seat;
			bool last_tile = wall_.empty();
			bool earthly_hand = [this]() {
				std::size_t tcount = transition_queue_.size();
				return transition_queue_[tcount - 3].kind == EventKind::kPredraw;
			}();
			// construct mahjong::hand object
			std::vector<mahjong::meld> melds;
			for (const auto& wrapper : seats_[transition.actor_seat].melds) {
				melds.push_back(wrapper.meld_value);
			}
			mahjong::win_t wtype = 0;
			if (last_tile) {
				wtype |= mahjong::win_type::final_tile;
			}
			if (earthly_hand) {
				wtype |= mahjong::win_type::heavenly_or_earthly_hand;
			}
			wtype |= static_cast<mahjong::win_t>(transition.actor_seat);
			transition.win_type_bits = static_cast<std::uint64_t>(wtype);
			mahjong::hand hand(seats_[transition.actor_seat].hand_tiles, melds, ti, wtype);
			WinData win_data = BuildWinData(hand);
			// fill missing fields
			transition.tile = ti;
			transition.win_data = win_data;
			transition.revealed_hand_tiles =
				SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles);
			// update scores
			int score_inc = win_data.win_base_point * 3;
			seats_[transition.actor_seat].score += score_inc;
			seats_[transition_queue_.back().actor_seat].score -= score_inc;
			// set next transition
			state_.next_transition = next_round_transition();
		} break;
		
		case EventKind::kRobAddedKongWin: {
			// get the tile last discarded
			mahjong::tile_t ti = transition_queue_.back().tile.value();
			transition.result_source_actor = transition_queue_.back().actor_seat;
			bool last_tile = wall_.empty();
			// construct mahjong::hand object
			std::vector<mahjong::meld> melds;
			for (const auto& wrapper : seats_[transition.actor_seat].melds) {
				melds.push_back(wrapper.meld_value);
			}
			mahjong::win_t wtype = mahjong::win_type::kong_related;
			wtype |= static_cast<mahjong::win_t>(transition.actor_seat);
			if (last_tile) {
				wtype |= mahjong::win_type::final_tile;
			}
			transition.win_type_bits = static_cast<std::uint64_t>(wtype);
			mahjong::hand hand(seats_[transition.actor_seat].hand_tiles, melds, ti, wtype);
			WinData win_data = BuildWinData(hand);
			// fill missing fields
			transition.tile = ti;
			transition.win_data = win_data;
			transition.revealed_hand_tiles =
				SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles);
			// update scores
			int score_inc = win_data.win_base_point * 3;
			seats_[transition.actor_seat].score += score_inc;
			seats_[transition_queue_.back().actor_seat].score -= score_inc;
			// set next transition
			state_.next_transition = next_round_transition();
		} break;
		
		case EventKind::kSelfDrawnWin: {
			// construct mahjong::hand object
			bool last_tile = wall_.empty();
			bool heavenly_hand = [this]() {
				std::size_t tcount = transition_queue_.size();
				return transition_queue_[tcount - 2].kind == EventKind::kPredraw;
			}();
			bool from_back = transition_queue_.back().draw_from_back.value_or(false);
			std::vector<mahjong::meld> melds;
			for (const auto& wrapper : seats_[transition.actor_seat].melds) {
				melds.push_back(wrapper.meld_value);
			}
			mahjong::win_t wtype = mahjong::win_type::self_drawn;
			if (last_tile) {
				wtype |= mahjong::win_type::final_tile;
			}
			if (heavenly_hand) {
				wtype |= mahjong::win_type::heavenly_or_earthly_hand;
			}
			if (from_back) {
				wtype |= mahjong::win_type::kong_related;
			}
			wtype |= static_cast<mahjong::win_t>(transition.actor_seat);
			transition.result_source_actor = transition.actor_seat;
			transition.win_type_bits = static_cast<std::uint64_t>(wtype);
			mahjong::hand hand(seats_[transition.actor_seat].hand_tiles, melds, seats_[transition.actor_seat].drawn_tile, wtype);
			WinData win_data = BuildWinData(hand);
			// fill missing fields
			if (seats_[transition.actor_seat].has_drawn_tile()) {
				transition.tile = seats_[transition.actor_seat].drawn_tile;
			}
			transition.win_data = win_data;
			transition.revealed_hand_tiles =
				SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles);
			// update scores
			int score_inc = win_data.win_base_point;
			for (int i = 0; i < 4; ++i) {
				if (i == transition.actor_seat) {
					seats_[i].score += score_inc * 3;
				} else {
					seats_[i].score -= score_inc;
				}
			}
			// set next transition
			state_.next_transition = next_round_transition();
		} break;
		
		case EventKind::kDrawnGame: {
			// set next transition
			state_.next_transition = next_round_transition();
		} break;

		case EventKind::kEnd: {
			transition.final_scores.clear();
			transition.final_scores.reserve(4);
			for (int seat = 0; seat < 4; ++seat) {
				transition.final_scores.push_back(seats_[seat].score);
			}
			end_session(transition.timestamp_ms, false);
		} break;

		default: {
			return;
		}
	}
	process_transition(transition);
	const bool defer_round_record_until_end =
		IsRoundResultTerminal(transition.kind) &&
		state_.next_transition.has_value() &&
		state_.next_transition->kind == EventKind::kEnd;
	if ((!defer_round_record_until_end && IsRoundResultTerminal(transition.kind)) ||
		transition.kind == EventKind::kEnd) {
		enqueue_current_round_record();
	}
	if (transition.kind == EventKind::kEnd && hub_ != nullptr) {
		lock.unlock();
		hub_->notify_session_lists_changed();
	}
}

namespace {

struct MsgPolicy {
	Json::Value msg{};
	int delay_ms{0};
	PendingStatus set_pending{PendingStatus::kPendingNone};
};

auto BuildEnvelope(std::string_view type, Json::Value payload) -> Json::Value {
	Json::Value envelope(Json::objectValue);
	envelope["version"] = 1;
	envelope["type"] = std::string(type);
	envelope["payload"] = std::move(payload);
	return envelope;
}

auto EventKindName(EventKind kind) -> std::string_view {
	switch (kind) {
		case EventKind::kStart:
			return "start";
		case EventKind::kPredraw:
			return "predraw";
		case EventKind::kDrawTile:
			return "draw_tile";
		case EventKind::kDiscardTile:
			return "discard_tile";
		case EventKind::kChow:
			return "chow";
		case EventKind::kPung:
			return "pung";
		case EventKind::kMeldedKong:
			return "melded_kong";
		case EventKind::kAddedKong:
			return "added_kong";
		case EventKind::kConcealedKong:
			return "concealed_kong";
		case EventKind::kDiscardWin:
			return "discard_win";
		case EventKind::kRobAddedKongWin:
			return "rob_added_kong_win";
		case EventKind::kSelfDrawnWin:
			return "self_drawn_win";
		case EventKind::kPass:
			return "pass";
		case EventKind::kFinalPass:
			return "final_pass";
		case EventKind::kDrawnGame:
			return "drawn_game";
		case EventKind::kEnd:
			return "end";
		case EventKind::kPlayerLeft:
			return "player_left";
		case EventKind::kPlayerResumed:
			return "player_resumed";
		case EventKind::kNone:
			return "none";
	}

	return "unknown";
}

auto PendingStatusName(PendingStatus status) -> std::string_view {
	switch (status) {
		case PendingStatus::kPendingNone:
			return "none";
		case PendingStatus::kPendingPrimary:
			return "primary";
		case PendingStatus::kPendingSecondary:
			return "secondary";
		case PendingStatus::kPendingSlept:
			return "slept";
	}

	return "none";
}

auto MeldTypeName(mahjong::meld_type type) -> std::string_view {
	switch (type) {
		case mahjong::meld_type::sequence:
			return "sequence";
		case mahjong::meld_type::triplet:
			return "triplet";
		case mahjong::meld_type::kong:
			return "kong";
	}

	return "unknown";
}

auto IsSyncCheckpoint(EventKind kind) -> bool {
	return kind == EventKind::kDiscardTile || kind == EventKind::kAddedKong ||
		kind == EventKind::kConcealedKong || kind == EventKind::kSelfDrawnWin;
}

auto IsVisibleTransition(EventKind kind) -> bool {
	switch (kind) {
		case EventKind::kStart:
		case EventKind::kPredraw:
		case EventKind::kDrawTile:
		case EventKind::kDiscardTile:
		case EventKind::kChow:
		case EventKind::kPung:
		case EventKind::kMeldedKong:
		case EventKind::kAddedKong:
		case EventKind::kConcealedKong:
		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
		case EventKind::kSelfDrawnWin:
		case EventKind::kDrawnGame:
		case EventKind::kEnd:
			return true;
		default:
			return false;
	}
}

auto IsPublicClaim(EventKind kind) -> bool {
	switch (kind) {
		case EventKind::kChow:
		case EventKind::kPung:
		case EventKind::kMeldedKong:
		case EventKind::kAddedKong:
		case EventKind::kConcealedKong:
		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
		case EventKind::kSelfDrawnWin:
		case EventKind::kPlayerLeft:
		case EventKind::kPlayerResumed:
			return true;
		default:
			return false;
	}
}

auto WaitDurationMs(const GameConfig& config, PendingStatus pending, int auxiliary_ms = 0) -> int {
	switch (pending) {
		case PendingStatus::kPendingPrimary:
			return GameConfig::with_margin(config.primary_timer_ms) + std::max(0, auxiliary_ms);
		case PendingStatus::kPendingSecondary:
			return GameConfig::with_margin(config.secondary_timer_ms);
		default:
			return 0;
	}
}

auto SerializeTiles(const std::vector<mahjong::tile_t>& tiles) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (mahjong::tile_t tile : tiles) {
		payload.append(Json::UInt(static_cast<unsigned int>(tile)));
	}
	return payload;
}

auto SerializeMeld(const MeldWrapper& wrapper) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["tile"] = Json::UInt(static_cast<unsigned int>(wrapper.meld_value.tile()));
	payload["type"] = std::string(MeldTypeName(wrapper.meld_value.type()));
	payload["concealed"] = wrapper.meld_value.concealed();
	payload["chow_mode"] = wrapper.chow_mode;
	payload["meld_from_rel"] = wrapper.meld_from_rel;
	return payload;
}

auto SerializeWinData(const WinData& data) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["win_fan"] = data.win_fan;
	payload["win_base_point"] = data.win_base_point;
	Json::Value fan_codes(Json::arrayValue);
	for (const auto fan_code : data.win_fan_codes) {
		fan_codes.append(fan_code.to_string());
	}
	payload["win_fan_codes"] = std::move(fan_codes);
	Json::Value fan_names(Json::arrayValue);
	for (const auto& fan_name : data.win_fans) {
		fan_names.append(fan_name);
	}
	payload["win_fans"] = std::move(fan_names);
	return payload;
}

auto SerializeAvailableActions(const Seat& seat,
				 PendingStatus pending,
				 bool include_discard,
				 int relative_to_target,
				 std::optional<mahjong::tile_t> reaction_tile = std::nullopt) -> Json::Value {
	Json::Value actions(Json::arrayValue);
	if (pending == PendingStatus::kPendingNone || pending == PendingStatus::kPendingSlept) {
		return actions;
	}

	auto append_simple = [&actions, reaction_tile](std::string_view kind, bool include_reaction_tile = false) {
		Json::Value action(Json::objectValue);
		action["kind"] = std::string(kind);
		if (include_reaction_tile && reaction_tile.has_value()) {
			action["tile"] = Json::UInt(static_cast<unsigned int>(*reaction_tile));
		}
		actions.append(std::move(action));
	};

	auto append_tile_action = [&actions](std::string_view kind,
						   mahjong::tile_t tile,
						   bool use_drawn_tile) {
		Json::Value action(Json::objectValue);
		action["kind"] = std::string(kind);
		action["tile"] = Json::UInt(static_cast<unsigned int>(tile));
		action["use_drawn_tile"] = use_drawn_tile;
		actions.append(std::move(action));
	};

	if (include_discard) {
		append_simple("discard_tile");
		if (seat.avail_melds_self.self_drawn_win) {
			append_simple("self_drawn_win");
		}
		for (mahjong::tile_t tile : seat.avail_melds_self.ckong_from_hand) {
			append_tile_action("concealed_kong", tile, false);
		}
		for (mahjong::tile_t tile : seat.avail_melds_self.ckong_from_draw) {
			append_tile_action("concealed_kong", tile, true);
		}
		for (mahjong::tile_t tile : seat.avail_melds_self.akong_from_hand) {
			append_tile_action("added_kong", tile, false);
		}
		for (mahjong::tile_t tile : seat.avail_melds_self.akong_from_draw) {
			append_tile_action("added_kong", tile, true);
		}
	} else {
		for (int chow_mode = 1; chow_mode <= 3; ++chow_mode) {
			if ((seat.avail_melds_other & MeldOpFilter::kChows[chow_mode]) == 0) {
				continue;
			}
			Json::Value action(Json::objectValue);
			action["kind"] = "chow";
			action["ui64_value"] = Json::UInt64(static_cast<Json::UInt64>(chow_mode));
			if (reaction_tile.has_value()) {
				action["tile"] = Json::UInt(static_cast<unsigned int>(*reaction_tile));
			}
			actions.append(std::move(action));
		}
		if ((seat.avail_melds_other & MeldOpFilter::kPung) != 0) {
			append_simple("pung", true);
		}
		if ((seat.avail_melds_other & MeldOpFilter::kMeldedKong) != 0) {
			append_simple("melded_kong", true);
		}
		if ((seat.avail_melds_other & MeldOpFilter::kDiscardWin) != 0) {
			append_simple("discard_win", true);
		}
		if ((seat.avail_melds_other & MeldOpFilter::kRobAddedKongWin) != 0) {
			append_simple("rob_added_kong_win", true);
		}
		if (seat.avail_melds_other != 0) {
			if (relative_to_target != 1 || 
				(seat.avail_melds_other & MeldOpFilter::kDiscardWin) != 0 ||
				(seat.avail_melds_other & MeldOpFilter::kRobAddedKongWin) != 0) {
				append_simple("pass");
			}
			append_simple("final_pass");
		}
	}
	return actions;
}

auto SerializeVisibleEvent(const Event& event,
					   int viewer_seat,
					   std::uint64_t stage_counter) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["kind"] = std::string(EventKindName(event.kind));
	payload["stage_counter"] = Json::UInt64(stage_counter);
	payload["actor_seat"] = event.actor_seat;
	payload["timestamp_ms"] = Json::Int64(event.timestamp_ms);

	auto append_tile = [&payload](mahjong::tile_t tile) {
		payload["tile"] = Json::UInt(static_cast<unsigned int>(tile));
	};

	switch (event.kind) {
		case EventKind::kPredraw: {
			Json::Value drawn_tiles(Json::arrayValue);
			for (const auto tile : event.drawn_tiles) {
				drawn_tiles.append(
					viewer_seat == event.actor_seat
						? Json::UInt(static_cast<unsigned int>(tile))
						: Json::UInt(0));
			}
			payload["drawn_tiles"] = std::move(drawn_tiles);
			if (event.ui64_value.has_value()) {
				payload["ui64_value"] = Json::UInt64(*event.ui64_value);
			}
		} break;

		case EventKind::kDrawTile:
			if (event.draw_from_back.has_value()) {
				payload["draw_from_back"] = *event.draw_from_back;
			}
			if (viewer_seat == event.actor_seat && event.tile.has_value()) {
				append_tile(*event.tile);
			}
			break;

		case EventKind::kDiscardTile:
			if (event.tile.has_value()) {
				append_tile(*event.tile);
			}
			if (event.use_drawn_tile.has_value()) {
				payload["use_drawn_tile"] = *event.use_drawn_tile;
			}
			if (event.forced.has_value()) {
				payload["forced"] = *event.forced;
			}
			break;

		case EventKind::kChow:
			if (event.tile.has_value()) {
				append_tile(*event.tile);
			}
			if (event.ui64_value.has_value()) {
				payload["ui64_value"] = Json::UInt64(*event.ui64_value);
			}
			break;

		case EventKind::kPung:
		case EventKind::kMeldedKong:
		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
		case EventKind::kSelfDrawnWin:
		case EventKind::kAddedKong:
		case EventKind::kConcealedKong:
			if (event.tile.has_value()) {
				append_tile(*event.tile);
			}
			if ((event.kind == EventKind::kAddedKong ||
				 event.kind == EventKind::kConcealedKong) &&
				event.use_drawn_tile.has_value()) {
				payload["use_drawn_tile"] = *event.use_drawn_tile;
			}
			break;

		case EventKind::kEnd:
			break;

		default:
			break;
	}

	if (event.win_data.has_value()) {
		payload["win"] = SerializeWinData(*event.win_data);
	}
	if (!event.revealed_hand_tiles.empty()) {
		payload["revealed_hand_tiles"] = SerializeTiles(event.revealed_hand_tiles);
	}
	if (!event.final_scores.empty()) {
		Json::Value scores(Json::arrayValue);
		for (const int score : event.final_scores) {
			scores.append(score);
		}
		payload["scores"] = std::move(scores);
	}

	return payload;
}

auto MsgOnClaim(const Event& event,
			 const std::array<Seat, 4>& seats,
			 const std::array<bool, 4>& interval_delayed_seats,
			 std::int64_t dispatch_now_ms) -> std::array<MsgPolicy, 4> {
	std::array<MsgPolicy, 4> policies{};
	const bool apply_interval_delay =
		event.kind != EventKind::kPlayerLeft && event.kind != EventKind::kPlayerResumed;

	for (int seat = 0; seat < 4; ++seat) {
		std::int64_t deliver_at_ms = dispatch_now_ms;
		if (apply_interval_delay && interval_delayed_seats[seat]) {
			deliver_at_ms += GameConfig::meld_offset_ms;
		}
		policies[seat].delay_ms = std::max<int>(
			0,
			static_cast<int>(deliver_at_ms - dispatch_now_ms));
	}

	switch (event.kind) {
		case EventKind::kChow:
		case EventKind::kPung:
		case EventKind::kMeldedKong:
		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
			for (int seat = 0; seat < 4; ++seat) {
				if (seat == event.actor_seat || seats[seat].avail_melds_other == 0) {
					continue;
				}
				policies[seat].set_pending = PendingStatus::kPendingSecondary;
			}
			break;

		default:
			break;
	}

	return policies;
}

auto MsgOnTransition(const Event& transition,
			      const std::array<Seat, 4>& seats,
			      const std::array<bool, 4>& interval_delayed_seats,
			      const std::array<bool, 4>& next_interval_delayed_seats,
			      const std::optional<Event>& previous_transition,
			      std::int64_t dispatch_now_ms,
			      int random_pause_ms) -> std::array<MsgPolicy, 4> {
	std::array<MsgPolicy, 4> policies{};

	for (int seat = 0; seat < 4; ++seat) {
		std::int64_t deliver_at_ms = dispatch_now_ms;
		if (IsSyncCheckpoint(transition.kind)) {
			if (previous_transition.has_value() && seat != transition.actor_seat && interval_delayed_seats[seat]) {
				deliver_at_ms = std::max<std::int64_t>(
					deliver_at_ms,
					previous_transition->timestamp_ms + GameConfig::meld_offset_ms + GameConfig::minimal_transition_ms);
			}
			if (next_interval_delayed_seats[seat] && seat != transition.actor_seat) {
				deliver_at_ms += GameConfig::meld_offset_ms;
			}
			if (seat != transition.actor_seat &&
				(transition.kind == EventKind::kDiscardTile ||
				 transition.kind == EventKind::kAddedKong)) {
				deliver_at_ms += random_pause_ms;
			}
		} else if (transition.kind != EventKind::kEnd &&
			       transition.kind != EventKind::kStart &&
			       interval_delayed_seats[seat]) {
			deliver_at_ms += GameConfig::meld_offset_ms;
		}

		policies[seat].delay_ms = std::max<int>(
			0,
			static_cast<int>(deliver_at_ms - dispatch_now_ms));
	}

	switch (transition.kind) {
		case EventKind::kDrawTile:
		case EventKind::kChow:
		case EventKind::kPung:
			if (!seats[transition.actor_seat].is_afk()) {
				policies[transition.actor_seat].set_pending = PendingStatus::kPendingPrimary;
			}
			break;

		case EventKind::kDiscardTile:
		case EventKind::kAddedKong:
			for (int seat = 0; seat < 4; ++seat) {
				if (seat == transition.actor_seat || seats[seat].avail_melds_other == 0) {
					continue;
				}
				if (seats[seat].is_afk()) {
					continue;
				}
				if (seat == (transition.actor_seat + 1) % 4) {
					policies[seat].set_pending = PendingStatus::kPendingPrimary;
				} else {
					policies[seat].set_pending = PendingStatus::kPendingSecondary;
				}
			}
			break;

		default:
			break;
	}

	return policies;
}

} // namespace

void ActiveSession::schedule_pending_start(int seat,
					   PendingStatus pending,
					   int delay_ms,
					   std::uint64_t stage_counter) {
	if (seat < 0 || seat >= static_cast<int>(seats_.size()) ||
		pending == PendingStatus::kPendingNone ||
		pending == PendingStatus::kPendingSlept) {
		return;
	}

	scheduled_pending_[seat] = pending;

	auto apply_pending = [this, seat, pending, stage_counter]() {
		std::lock_guard lock(state_.mutex);

		// If a newer schedule overwrote this one, do nothing.
		if (scheduled_pending_[seat] != pending) {
			return;
		}
		scheduled_pending_[seat].reset();

		if (state_.stage_counter != stage_counter) {
			return;
		}

		const auto delivery_now = now_ms();
		if (pending == PendingStatus::kPendingPrimary) {
			start_primary_wait(seat, delivery_now);
		} else if (pending == PendingStatus::kPendingSecondary) {
			start_secondary_wait(seat, delivery_now);
		}
	};

	if (delay_ms <= 0) {
		apply_pending();
		return;
	}

	pending_start_timers_[seat].set(delay_ms, [apply_pending]() mutable {
		apply_pending();
	});
}

auto ActiveSession::build_snapshot_for_player(
	int seat,
	const Event* context_event) const -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["phase"] = "active";
	payload["session_id"] = Json::Int64(session_id_);

	Json::Value state_payload(Json::objectValue);
	state_payload["round_counter"] = Json::UInt64(state_.round_counter);
	state_payload["stage_counter"] = Json::UInt64(state_.stage_counter);
	state_payload["remaining_tile_count"] = Json::UInt64(static_cast<Json::UInt64>(wall_.size()));
	state_payload["ended"] = ended_;
	if (ended_) {
		state_payload["final_scores"] = SerializeScores(final_scores_);
	} else {
		state_payload["final_scores"] = Json::Value(Json::nullValue);
	}
	auto last_transition = transition_queue_.empty() ? std::nullopt : std::make_optional(transition_queue_.back());
		if (last_transition.has_value() &&
			(last_transition->kind == EventKind::kDiscardWin ||
			 last_transition->kind == EventKind::kRobAddedKongWin) &&
			transition_queue_.size() >= 2) {
			state_payload["result_source_actor"] = transition_queue_[transition_queue_.size() - 2].actor_seat;
		} else {
			state_payload["result_source_actor"] = Json::Value(Json::nullValue);
		}
	if (last_transition.has_value()) {
		state_payload["last_actor"] = last_transition->actor_seat;
		state_payload["last_event_kind"] = std::string(EventKindName(last_transition->kind));
	} else {
		state_payload["last_actor"] = Json::Value(Json::nullValue);
		state_payload["last_event_kind"] = Json::Value(Json::nullValue);
	}
	state_payload["current_player"] = state_.current_player;
	payload["state"] = std::move(state_payload);

	Json::Value seats_payload(Json::arrayValue);
	for (std::size_t index = 0; index < seats_.size(); ++index) {
		const Seat& current = seats_[index];
		Json::Value seat_payload(Json::objectValue);
		seat_payload["seat_index"] = static_cast<int>(index);
		seat_payload["score"] = current.score;
		seat_payload["afk"] = current.is_afk();
		seat_payload["disconnected"] = current.disconnected;
		seat_payload["hand_tile_count"] = Json::UInt64(current.hand_tiles.size());
		seat_payload["has_drawn_tile"] = current.has_drawn_tile();

		const auto player = current.player.lock();
		if (player != nullptr) {
			seat_payload["player_id"] = Json::Int64(player->player_id);
			seat_payload["username"] = player->username;
		} else {
			seat_payload["player_id"] = Json::Value(Json::nullValue);
			seat_payload["username"] = Json::Value(Json::nullValue);
		}

		seat_payload["discard_pile"] = SerializeTiles(current.discard_pile);
		Json::Value melds(Json::arrayValue);
		for (const auto& wrapper : current.melds) {
			melds.append(SerializeMeld(wrapper));
		}
		seat_payload["melds"] = std::move(melds);

		if (static_cast<int>(index) == seat) {
			seat_payload["hand_tiles"] = SerializeTiles(current.hand_tiles);
			if (current.has_drawn_tile()) {
				seat_payload["drawn_tile"] =
					Json::UInt(static_cast<unsigned int>(current.drawn_tile));
			} else {
				seat_payload["drawn_tile"] = Json::Value(Json::nullValue);
			}
		}

		seats_payload.append(std::move(seat_payload));
	}
	payload["seats"] = std::move(seats_payload);

	const PendingStatus pending = ended_ ? PendingStatus::kPendingNone : scheduled_pending_[seat].value_or(seats_[seat].pending);
	auto decision_timer_ms = [&]() -> std::optional<int> {
		if (ended_) {
			return std::nullopt;
		}
		if (pending != PendingStatus::kPendingPrimary && pending != PendingStatus::kPendingSecondary) {
			return std::nullopt;
		}

		const int total_ms = WaitDurationMs(config_, pending, seats_[seat].auxiliary_ms);

		if (seats_[seat].pending_from_ms <= 0) {
			return std::max(0, total_ms - GameConfig::network_delay_ms);
		}

		const auto elapsed_ms = std::max<std::int64_t>(0, now_ms() - seats_[seat].pending_from_ms);
		return static_cast<int>(std::max<std::int64_t>(0, static_cast<std::int64_t>(total_ms) - elapsed_ms));
	}();
	const bool include_discard =
		!ended_ &&
		state_.next_transition.has_value() &&
		state_.next_transition->kind == EventKind::kDiscardTile &&
		state_.next_transition->actor_seat == seat;

	Json::Value viewer(Json::objectValue);
	viewer["seat_index"] = seat;
	viewer["pending"] = std::string(PendingStatusName(pending));
	if (decision_timer_ms.has_value()) {
		viewer["decision_timer_ms"] = std::max(0, *decision_timer_ms - GameConfig::network_delay_ms);
	} else {
		viewer["decision_timer_ms"] = Json::Value(Json::nullValue);
	}
	std::optional<mahjong::tile_t> reaction_tile;
	int relative_to_target = 0;
	const Event* reaction_source = nullptr;
	if (!ended_ && context_event != nullptr &&
		(context_event->kind == EventKind::kDiscardTile ||
		 context_event->kind == EventKind::kAddedKong) &&
		context_event->actor_seat != seat &&
		context_event->tile.has_value()) {
		reaction_source = context_event;
	} else if (!ended_ && !transition_queue_.empty()) {
		const Event& last_transition = transition_queue_.back();
		if ((last_transition.kind == EventKind::kDiscardTile ||
			 last_transition.kind == EventKind::kAddedKong) &&
			last_transition.actor_seat != seat &&
			last_transition.tile.has_value()) {
			reaction_source = &last_transition;
		}
	}
	if (reaction_source != nullptr) {
		reaction_tile = reaction_source->tile;
		relative_to_target = (seat - reaction_source->actor_seat + 4) % 4;
	}
	if (ended_) {
		viewer["available_actions"] = Json::Value(Json::arrayValue);
		viewer["wait_data"] = Json::Value(Json::nullValue);
	} else {
		viewer["available_actions"] =
			SerializeAvailableActions(seats_[seat], pending, include_discard, relative_to_target, reaction_tile);
		viewer["wait_data"] = SerializeViewerWaitData(seats_, seat);
	}

	// If a pending-start timer is still counting down for this seat,
	// tell the frontend the remaining time so it can delay showing
	// meld options. Only include this in snapshot builds (context_event == nullptr),
	// not in live game event messages — the timer is for resuming players.
	if (!ended_ && context_event == nullptr && pending_start_timers_[seat].isRunning()) {
		viewer["pending_start_timer_remaining_ms"] =
			Json::Int64(static_cast<Json::Int64>(pending_start_timers_[seat].remainingMs()));
	} else {
		viewer["pending_start_timer_remaining_ms"] = Json::Value(Json::nullValue);
	}

	payload["viewer"] = std::move(viewer);
	if (last_transition.has_value() &&
		(last_transition->kind == EventKind::kDiscardWin ||
		 last_transition->kind == EventKind::kRobAddedKongWin ||
		 last_transition->kind == EventKind::kSelfDrawnWin ||
		 last_transition->kind == EventKind::kDrawnGame)) {
		payload["result_event"] = SerializeVisibleEvent(*last_transition, seat, state_.stage_counter);
	} else {
		payload["result_event"] = Json::Value(Json::nullValue);
	}

	Json::Value ratings_array(Json::arrayValue);
	for (const auto& r : current_round_ratings_) {
		ratings_array.append(r.ToJson());
	}
	payload["ratings"] = std::move(ratings_array);

	return payload;
}

auto ActiveSession::build_event_message_for_player(
	int seat,
	const Event& event,
	std::string_view category) const -> Json::Value {
	Json::Value payload(Json::objectValue);
	const Json::Value snapshot = build_snapshot_for_player(seat, &event);
	payload["category"] = std::string(category);
	payload["event"] = SerializeVisibleEvent(event, seat, state_.stage_counter);
	if (event.kind == EventKind::kStart || event.kind == EventKind::kEnd) {
		Json::Value ratings_array(Json::arrayValue);
		for (const auto& r : current_round_ratings_) {
			ratings_array.append(r.ToJson());
		}
		payload["ratings"] = std::move(ratings_array);
	}
	if (event.kind == EventKind::kEnd && !final_round_ratings_.empty()) {
		Json::Value final_arr(Json::arrayValue);
		for (const auto& r : final_round_ratings_) {
			final_arr.append(r.ToJson());
		}
		payload["final_ratings"] = std::move(final_arr);
	}
	payload["state"] = snapshot["state"];
	payload["viewer"] = snapshot["viewer"];

	Json::Value seat_status(Json::arrayValue);
	for (const auto& current : snapshot["seats"]) {
		Json::Value seat_payload(Json::objectValue);
		seat_payload["seat_index"] = current["seat_index"];
		seat_payload["score"] = current["score"];
		seat_payload["afk"] = current["afk"];
		seat_payload["disconnected"] = current["disconnected"];
		seat_payload["username"] = current["username"];
		seat_payload["hand_tile_count"] = current["hand_tile_count"];
		seat_payload["has_drawn_tile"] = current["has_drawn_tile"];
		seat_status.append(std::move(seat_payload));
	}
	payload["seat_status"] = std::move(seat_status);
	return BuildEnvelope("game.event", std::move(payload));
}

int ActiveSession::get_random_pause() {
	// Bernoulli distribution with probability GameConfig::random_pause_prob
	std::bernoulli_distribution apply_random_pause_dist(config_.random_pause_prob);
	if (!apply_random_pause_dist(random_pause_rng_)) {
		return 0;
	} else {
		std::uniform_int_distribution<int> random_pause_dist(0, config_.random_pause_range);
		return random_pause_dist(random_pause_rng_);
	}
}

/* Protect players' hand information by hiding pauses from other players.
 * We define the following transitions as sync checkpoints:
 * 1. Discard tile.
 * 2. Added kong.
 * 3. Concealed kong.
 * 4. Self-drawn win.
 * A basic interval starts from a sync checkpoint and ends at the next sync checkpoint. 
 * After a self-drawn win, the next state transition will be kStart,
 * so no need to do anything special.
 * If an interval starts with concealed kong, also do nothing special since 
 * it cannot trigger meld options from other players.
 * If an interval starts with a discard tile or an added kong, the three players other than
 * the actor might have meld options. We use this algorithm to hide meld pauses:
 * 1. After the start transition, check which players have meld options.
 *    If no players or all players have meld options, do nothing special.
 *    Otherwise, mark those without meld options as "delayed."
 * 2. Broadcast all claims and transitions in the basic interval Config::meld_offset_ms
 *    later for "delayed" players, INCLUDING the starting transition but EXCEPT the ending 
 *    transition, whose rules are listed below.
 * 3. If the ending transition is at least Config::meld_offset_ms after the second to last
 *    transition, broadcast it normally. Otherwise, broadcast it immediately for the actor,
 *    but delay it until Config::meld_offset_ms after the second to last transition for
 *    other players. Note that since the ending transition also marks the start of the 
 *    next basic interval, if that interval triggers the "delayed" players rule, the
 *    broadcast of the ending transition to those players will be further delayed.
 * 4. On top of the above rules, an extra delay of a random duration between 0 and 
 *    Config::random_pause_range is applied with probability Config::random_pause_prob
 *    to all players except the actor on discard tile or added kong transitions.
 * 5. Only start the wait window (start_primary_wait() or start_secondary_wait()) for a 
 *    player when the claim or transition is broadcasted to THAT player. Do not start any 
 *    waits for players without meld options. Set the state transition timer accordingly.
 */

// this function should only broadcast the claim by providing 
// necessary information that should be visible to the players.
// especially, need a full state flush for resumed players.
void ActiveSession::flush_for_player(int seat) {
	if (seat < 0 || seat >= static_cast<int>(seats_.size())) {
		return;
	}

	// When a player reconnects, do NOT cancel pending-start timers.
	// Instead build_snapshot_for_player includes
	// pending_start_timer_remaining_ms so the frontend can delay
	// showing meld choices until the timer fires.
	(void)send_message(seat, BuildEnvelope("session.snapshot", build_snapshot_for_player(seat)), 0);
}

void ActiveSession::broadcast_claim(const Event& event) {
	std::lock_guard lock(state_.mutex);
	if (event.kind == EventKind::kPlayerResumed) {
		flush_for_player(event.actor_seat);
	}

	if (!IsPublicClaim(event.kind)) {
		return;
	}

	const std::int64_t dispatch_now_ms = now_ms();
	auto policies = MsgOnClaim(event, seats_, interval_delayed_seats_, dispatch_now_ms);
	for (int seat = 0; seat < 4; ++seat) {
		if (event.kind == EventKind::kPlayerResumed && seat == event.actor_seat) {
			continue;
		}

		schedule_pending_start(
			seat,
			policies[seat].set_pending,
			policies[seat].delay_ms,
			state_.stage_counter);
		policies[seat].msg = build_event_message_for_player(seat, event, "claim");
		(void)send_message(seat, policies[seat].msg, policies[seat].delay_ms);
	}
}

// this function should not only broadcast the transition,
// but also set up the transition timer and the wait status of the players.
// this function should also add the transition to the queue.
void ActiveSession::process_transition(const Event& transition) {
	std::lock_guard lock(state_.mutex);
	const std::optional<Event> previous_transition =
		transition_queue_.empty() ? std::optional<Event>{}
							 : std::optional<Event>(transition_queue_.back());

	std::array<bool, 4> next_interval_delayed_seats{};
	if (transition.kind == EventKind::kDiscardTile ||
		transition.kind == EventKind::kAddedKong) {
		int reacting_seat_count = 0;
		int non_afk_count = 0;
		for (int seat = 0; seat < 4; ++seat) {
			if (seat == transition.actor_seat) {
				continue;
			} 
			non_afk_count += !seats_[seat].is_afk();
			if (seats_[seat].avail_melds_other == 0) {
				continue;
			}
			++reacting_seat_count;
		}
		if (reacting_seat_count > 0 && reacting_seat_count < non_afk_count) {
			for (int seat = 0; seat < 4; ++seat) {
				if (seat == transition.actor_seat) {
					continue;
				}
				next_interval_delayed_seats[seat] = seats_[seat].avail_melds_other == 0;
			}
		}
	}

	const int random_pause_ms =
		(transition.kind == EventKind::kDiscardTile ||
		 transition.kind == EventKind::kAddedKong)
			? get_random_pause()
			: 0;

	const std::int64_t dispatch_now_ms = now_ms();
	auto policies = MsgOnTransition(
		transition,
		seats_,
		interval_delayed_seats_,
		next_interval_delayed_seats,
		previous_transition,
		dispatch_now_ms,
		random_pause_ms);

	int next_timer_delay_ms = 0;
	int max_delivery_delay_ms = 0;
	if (IsVisibleTransition(transition.kind)) {
		for (int seat = 0; seat < 4; ++seat) {
			schedule_pending_start(
				seat,
				policies[seat].set_pending,
				policies[seat].delay_ms,
				state_.stage_counter);
			policies[seat].msg =
				build_event_message_for_player(seat, transition, "transition");
			const int actual_delay_ms =
				send_message(seat, policies[seat].msg, policies[seat].delay_ms);
			max_delivery_delay_ms = std::max(max_delivery_delay_ms, actual_delay_ms);
			if (policies[seat].set_pending == PendingStatus::kPendingPrimary ||
				policies[seat].set_pending == PendingStatus::kPendingSecondary) {
				next_timer_delay_ms = std::max(
					next_timer_delay_ms,
					actual_delay_ms + WaitDurationMs(config_, policies[seat].set_pending, seats_[seat].auxiliary_ms));
			}
		}
	}

	transition_queue_.push_back(transition);
	if (IsSyncCheckpoint(transition.kind)) {
		interval_delayed_seats_ = next_interval_delayed_seats;
	} else if (transition.kind == EventKind::kStart || transition.kind == EventKind::kEnd) {
		interval_delayed_seats_.fill(false);
	}

	int fallback_delay_ms = 0;
	switch (transition.kind) {
		case EventKind::kStart:
		case EventKind::kDiscardTile:
		case EventKind::kMeldedKong:
		case EventKind::kAddedKong:
		case EventKind::kConcealedKong:
			fallback_delay_ms = GameConfig::minimal_transition_ms;
			break;

		case EventKind::kDrawTile:
		case EventKind::kChow:
		case EventKind::kPung:
			if (seats_[transition.actor_seat].is_afk()) {
				fallback_delay_ms = GameConfig::minimal_transition_ms;
			}
			break;

		case EventKind::kDiscardWin:
		case EventKind::kRobAddedKongWin:
		case EventKind::kSelfDrawnWin:
		case EventKind::kDrawnGame:
			fallback_delay_ms = GameConfig::round_interval_ms;
			break;

		case EventKind::kEnd:
			fallback_delay_ms = 0;
			break;

		case EventKind::kPredraw:
			if (transition.ui64_value.value_or(0) == 15) {
				fallback_delay_ms = GameConfig::minimal_transition_ms * 3;
			} else {
				fallback_delay_ms = GameConfig::minimal_transition_ms / 2;
			}
			break;

		default:
			break;
	}

	if (IsSyncCheckpoint(transition.kind)) {
		next_timer_delay_ms = std::max(next_timer_delay_ms, max_delivery_delay_ms + GameConfig::minimal_transition_ms);
		next_transition_not_before_ms_ = dispatch_now_ms + max_delivery_delay_ms + fallback_delay_ms;
	}

	if (state_.next_transition.has_value()) {
		const int scheduled_delay_ms = std::max(next_timer_delay_ms, fallback_delay_ms);
		next_transition_not_before_ms_ = std::max(next_transition_not_before_ms_, dispatch_now_ms + fallback_delay_ms);
		set_timer(scheduled_delay_ms, state_.stage_counter);
	} else {
		next_transition_not_before_ms_ = 0;
		transition_timer_.stop();
	}
}

auto PlayerRatingSnapshot::ToJson() const -> Json::Value {
	Json::Value json(Json::objectValue);
	json["player_id"] = Json::Int64(player_id);
	json["username"] = username;
	json["mu"] = mu;
	json["tau"] = tau;
	json["sigma"] = sigma;
	json["points"] = points;
	json["level"] = level;
	json["total_games"] = Json::Int64(total_games);
	return json;
}

auto PlayerRatingSnapshot::FromJson(const Json::Value& json) -> PlayerRatingSnapshot {
	PlayerRatingSnapshot r;
	r.player_id = json["player_id"].isInt64() ? json["player_id"].asInt64() : 0;
	r.username = json["username"].isString() ? json["username"].asString() : "";
	r.mu = json["mu"].isDouble() ? json["mu"].asDouble() : 0.0;
	r.tau = json["tau"].isDouble() ? json["tau"].asDouble() : 15.0;
	r.sigma = json["sigma"].isDouble() ? json["sigma"].asDouble() : 300.0;
	r.points = json["points"].isDouble() ? json["points"].asDouble() : 0.0;
	r.level = json["level"].isInt() ? json["level"].asInt() : 0;
	r.total_games = json["total_games"].isInt64() ? json["total_games"].asInt64() : 0;
	return r;
}

}  // namespace mmcr::game
