#include "game/engine/session_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <unordered_set>
#include <utility>

#include "random/seed.h"
#include "storage/game_record.h"
#include "external/qingque/rules/qingque.h"
#include "external/qingque/rules/w_data.h"

namespace mmcr::game {

std::uint64_t now_ns() {
    static uint64_t offset = 0;
    static bool initialized = false;
    
    if (!initialized) {
        offset = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) -
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        initialized = true;
    }

    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() + offset);
}

std::int64_t now_ms() {
    return static_cast<std::int64_t>(now_ns() / 1000000);
}

// Returns a set of initial tiles for debug mode.
std::vector<mahjong::tile_t> DebugInitialTiles(random::SeedContainer* seeder) {
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

Json::Value SerializeRecordGameConfig(const GameConfig& config) {
    Json::Value payload(Json::objectValue);
    payload["primary_timer_ms"] = config.primary_timer_ms;
    payload["secondary_timer_ms"] = config.secondary_timer_ms;
    payload["auxiliary_timer_ms"] = config.auxiliary_timer_ms;
    payload["round_count"] = config.round_count;
    payload["seat_shuffle_period"] = config.seat_shuffle_period;
    payload["recorded"] = config.recorded;
    payload["debug_mode"] = config.debug_mode;
    payload["public_session"] = config.public_session;
    return payload;
}

Json::Value SerializeRecordTiles(const std::vector<mahjong::tile_t>& tiles) {
    Json::Value payload(Json::arrayValue);
    for (const auto tile : tiles) {
        payload.append(Json::UInt(static_cast<unsigned int>(tile)));
    }
    return payload;
}

Json::Value SerializeRecordRoundStartSnapshot(const RoundStartSnapshot& snapshot) {
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

bool StartsStoredRoundTurn(EventKind kind) {
    return kind == EventKind::kDrawTile || kind == EventKind::kChow ||
        kind == EventKind::kPung || kind == EventKind::kMeldedKong;
}

bool CountsAsStoredMeld(EventKind kind) {
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

Json::Value SerializeStringArray(const std::vector<std::string>& values) {
    Json::Value payload(Json::arrayValue);
    for (const auto& value : values) {
        payload.append(value);
    }
    return payload;
}

Json::Value SerializeFanCodeArray(const std::vector<qingque::fan_code>& fan_codes) {
    Json::Value payload(Json::arrayValue);
    for (const auto& fan_code : fan_codes) {
        payload.append(fan_code.to_string());
    }
    return payload;
}

Json::Value SerializeMeldCount(const std::array<int, 4>& meld_count) {
    Json::Value payload(Json::arrayValue);
    for (const int count : meld_count) {
        payload.append(count);
    }
    return payload;
}

bool IsRoundResultTerminal(EventKind kind) {
    return kind == EventKind::kDiscardWin ||
        kind == EventKind::kRobAddedKongWin ||
        kind == EventKind::kSelfDrawnWin ||
        kind == EventKind::kDrawnGame;
}

const Event* FindRecordRoundResultTransition(const std::vector<Event>& transitions,
                             std::size_t start_index) {
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

Json::Value SerializeRecordRoundResult(const Event& terminal,
                        const std::array<int, 4>& meld_count,
                        std::int64_t total_turn) {
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
        if (terminal.winning_hand.has_value()) {
            payload["winning_hand"] = terminal.winning_hand->ToJson();
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
    payload["fan_ids"] = Json::Value(Json::arrayValue);
    return payload;
}

Json::Value SerializeRecordEvent(const Event& event,
                  std::optional<std::int64_t> round_total_turn) {
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
    if (event.winning_hand.has_value()) {
        payload["winning_hand"] = event.winning_hand->ToJson();
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

WinData BuildWinData(const mahjong::hand& h) {
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
        const auto deduped = qingque::dedupe(fan_code_fan_pairs[0].second);
        for (std::size_t index = 0; index < qingque::fans.size(); ++index) {
            if (!deduped.test(index)) {
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

auto ActiveSession::recording_enabled() const -> bool {
    return config_.recorded && record_manager_ != nullptr;
}

void ActiveSession::capture_round_record_state(const Event& transition) {
    if (!recording_enabled() || !transition.round_start_snapshot.has_value()) {
        return;
    }

    recording_.start_snapshot = transition.round_start_snapshot;
    recording_.initial_seats = Json::Value(Json::arrayValue);
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
        recording_.initial_seats.append(std::move(seat_payload));
    }
    recording_.transition_start_index = transition_queue_.size();
    recording_.event_start_index = event_queue_.size();
    recording_.number = state_.round_counter;
    recording_.saved = false;
}

void ActiveSession::enqueue_current_round_record() {
    if (!recording_enabled() || recording_.saved || !recording_.start_snapshot.has_value()) {
        return;
    }

    Json::Value payload(Json::objectValue);
    payload["version"] = 6;

    Json::Value header(Json::objectValue);
    header["session_identifier"] = identity_.identifier;
    header["round_number"] = Json::UInt64(recording_.number);
    header["game_config"] = SerializeRecordGameConfig(config_);
    payload["header"] = std::move(header);
    payload["round_start_snapshot"] =
        SerializeRecordRoundStartSnapshot(*recording_.start_snapshot);
    payload["initial_seats"] = recording_.initial_seats;
    if (const Event* round_result = FindRecordRoundResultTransition(
            transition_queue_, recording_.transition_start_index);
        round_result != nullptr) {
        payload["round_result"] = SerializeRecordRoundResult(
            *round_result,
            recording_.meld_count,
            recording_.turn);
    }

    Json::Value transition_queue(Json::arrayValue);
    for (std::size_t index = recording_.transition_start_index;
         index < transition_queue_.size();
         ++index) {
        transition_queue.append(SerializeRecordEvent(transition_queue_[index], recording_.turn));
    }
    payload["transition_queue"] = std::move(transition_queue);

    Json::Value event_queue(Json::arrayValue);
    for (std::size_t index = recording_.event_start_index; index < event_queue_.size(); ++index) {
        event_queue.append(SerializeRecordEvent(event_queue_[index], recording_.turn));
    }
    payload["event_queue"] = std::move(event_queue);

    Json::Value ratings_array(Json::arrayValue);
    for (const auto& r : recording_.ratings) {
        ratings_array.append(r.ToJson());
    }
    payload["ratings"] = std::move(ratings_array);

    Json::Value final_ratings_array(Json::arrayValue);
    for (const auto& r : recording_.final_ratings) {
        final_ratings_array.append(r.ToJson());
    }
    payload["final_ratings"] = std::move(final_ratings_array);

    auto status = record_manager_->Enqueue(storage::GameRecordTask{
        .session_identifier = identity_.identifier,
        .round_number = recording_.number,
        .payload = std::move(payload),
    });
    if (!status.ok()) {
        std::cerr << status.DebugString() << '\n';
        return;
    }

    recording_.saved = true;
}

}  // namespace mmcr::game
