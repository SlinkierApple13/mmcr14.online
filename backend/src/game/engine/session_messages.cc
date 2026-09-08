#include "game/engine/session_internal.h"

#include <algorithm>
#include <string>
#include <utility>

#include "external/qingque/rules/w_data.h"

namespace mmcr::game {

std::vector<mahjong::tile_t> SortTilesForDisplay(std::vector<mahjong::tile_t> tiles) {
    std::sort(tiles.begin(), tiles.end(), [](mahjong::tile_t left, mahjong::tile_t right) {
        const int left_offset = (left & 0b11100000u) == 0b10100000u ? 1000 : 0;
        const int right_offset = (right & 0b11100000u) == 0b10100000u ? 1000 : 0;
        return static_cast<int>(left) + left_offset > static_cast<int>(right) + right_offset;
    });
    return tiles;
}

Json::Value SerializeScores(const std::array<int, 4>& scores) {
    Json::Value payload(Json::arrayValue);
    for (const int score : scores) {
        payload.append(score);
    }
    return payload;
}

int CountRemainingForViewer(const std::array<Seat, 4>& seats,
                     int viewer_seat,
                     mahjong::tile_t tile) {
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

Json::Value SerializeViewerWaitData(const std::array<Seat, 4>& seats, int viewer_seat) {
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

Json::Value BuildEnvelope(std::string_view type, Json::Value payload) {
    Json::Value envelope(Json::objectValue);
    envelope["version"] = 1;
    envelope["type"] = std::string(type);
    envelope["payload"] = std::move(payload);
    return envelope;
}

std::string_view EventKindName(EventKind kind) {
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

std::string_view PendingStatusName(PendingStatus status) {
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

std::string_view MeldTypeName(mahjong::meld_type type) {
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

bool IsSyncCheckpoint(EventKind kind) {
    return kind == EventKind::kDiscardTile || kind == EventKind::kAddedKong ||
        kind == EventKind::kConcealedKong || kind == EventKind::kSelfDrawnWin;
}

bool IsVisibleTransition(EventKind kind) {
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

bool IsPublicClaim(EventKind kind) {
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

int WaitDurationMs(const GameConfig& config, PendingStatus pending, int auxiliary_ms) {
    switch (pending) {
        case PendingStatus::kPendingPrimary:
            return GameConfig::with_margin(config.primary_timer_ms) + std::max(0, auxiliary_ms);
        case PendingStatus::kPendingSecondary:
            return GameConfig::with_margin(config.secondary_timer_ms);
        default:
            return 0;
    }
}

Json::Value SerializeTiles(const std::vector<mahjong::tile_t>& tiles) {
    Json::Value payload(Json::arrayValue);
    for (mahjong::tile_t tile : tiles) {
        payload.append(Json::UInt(static_cast<unsigned int>(tile)));
    }
    return payload;
}

Json::Value SerializeMeld(const MeldWrapper& wrapper) {
    Json::Value payload(Json::objectValue);
    payload["tile"] = Json::UInt(static_cast<unsigned int>(wrapper.meld_value.tile()));
    payload["type"] = std::string(MeldTypeName(wrapper.meld_value.type()));
    payload["concealed"] = wrapper.meld_value.concealed();
    payload["chow_mode"] = wrapper.chow_mode;
    payload["meld_from_rel"] = wrapper.meld_from_rel;
    return payload;
}

Json::Value SerializeWinData(const WinData& data) {
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

Json::Value SerializeAvailableActions(const Seat& seat,
                 PendingStatus pending,
                 bool include_discard,
                 int relative_to_target,
                 std::optional<mahjong::tile_t> reaction_tile) {
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

Json::Value SerializeVisibleEvent(const Event& event,
                       int viewer_seat,
                       std::uint64_t stage_counter) {
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

Json::Value SerializeSpectatorEvent(const Event& event, std::uint64_t stage_counter) {
    Json::Value payload = SerializeVisibleEvent(event, -1, stage_counter);
    if (event.kind != EventKind::kDiscardWin &&
        event.kind != EventKind::kRobAddedKongWin &&
        event.kind != EventKind::kSelfDrawnWin) {
        payload.removeMember("revealed_hand_tiles");
    }
    return payload;
}

std::array<MsgPolicy, 4> MsgOnClaim(const Event& event,
             const std::array<Seat, 4>& seats,
             const std::array<bool, 4>& interval_delayed_seats,
             int meld_offset_ms,
             std::int64_t dispatch_now_ms) {
    std::array<MsgPolicy, 4> policies{};
    const bool apply_interval_delay =
        event.kind != EventKind::kPlayerLeft && event.kind != EventKind::kPlayerResumed;

    for (int seat = 0; seat < 4; ++seat) {
        std::int64_t deliver_at_ms = dispatch_now_ms;
        if (apply_interval_delay && interval_delayed_seats[seat]) {
            deliver_at_ms += meld_offset_ms;
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
                if (seats[seat].pending == PendingStatus::kPendingSlept) {
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

std::array<MsgPolicy, 4> MsgOnTransition(const Event& transition,
                  const std::array<Seat, 4>& seats,
                  const std::array<bool, 4>& interval_delayed_seats,
                  const std::array<bool, 4>& next_interval_delayed_seats,
                  const std::optional<Event>& previous_transition,
                  int current_meld_offset_ms,
                  int next_meld_offset_ms,
                  std::int64_t last_claim_event_ms,
                  std::int64_t dispatch_now_ms,
                  int random_pause_ms) {
    std::array<MsgPolicy, 4> policies{};
    const std::int64_t previous_transition_ms =
        previous_transition.has_value() ? previous_transition->timestamp_ms : 0;
    const std::int64_t last_interval_event_ms =
        std::max(previous_transition_ms, last_claim_event_ms);

    for (int seat = 0; seat < 4; ++seat) {
        std::int64_t deliver_at_ms = dispatch_now_ms;
        if (IsSyncCheckpoint(transition.kind)) {
            if (seat != transition.actor_seat && interval_delayed_seats[seat]) {
                deliver_at_ms = std::max<std::int64_t>(
                    deliver_at_ms,
                    last_interval_event_ms + current_meld_offset_ms + GameConfig::minimal_transition_ms);
            }
            if (next_interval_delayed_seats[seat] && seat != transition.actor_seat) {
                deliver_at_ms += next_meld_offset_ms;
            }
            if (seat != transition.actor_seat &&
                (transition.kind == EventKind::kDiscardTile ||
                 transition.kind == EventKind::kAddedKong)) {
                deliver_at_ms += random_pause_ms;
            }
        } else if (transition.kind != EventKind::kEnd &&
                   transition.kind != EventKind::kStart &&
                   interval_delayed_seats[seat]) {
            deliver_at_ms += current_meld_offset_ms;
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
                if (seats[seat].pending == PendingStatus::kPendingSlept) {
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

auto ActiveSession::build_snapshot_for_player(
    int seat,
    const Event* context_event) const -> Json::Value {
    const bool spectator = seat < 0;
    Json::Value payload(Json::objectValue);
    payload["phase"] = "active";
    payload["session_id"] = Json::Int64(identity_.id);
    payload["spectator"] = spectator;
    payload["abandon_game"] = config_.abandon_game;
    payload["duplicate_mode"] = config_.duplicate_mode;

    Json::Value state_payload(Json::objectValue);
    state_payload["round_counter"] = Json::UInt64(state_.round_counter);
    state_payload["stage_counter"] = Json::UInt64(state_.stage_counter);
    state_payload["remaining_tile_count"] = Json::UInt64(static_cast<Json::UInt64>(wall_size()));
    state_payload["ended"] = lifecycle_.ended;
    if (lifecycle_.ended) {
        state_payload["final_scores"] = SerializeScores(lifecycle_.final_scores);
    } else {
        state_payload["final_scores"] = Json::Value(Json::nullValue);
    }
    auto last_transition = transition_queue_.empty() ? std::nullopt : std::make_optional(transition_queue_.back());
    if (context_event != nullptr &&
        (context_event->kind == EventKind::kDiscardWin ||
         context_event->kind == EventKind::kRobAddedKongWin) &&
        context_event->result_source_actor.has_value()) {
        // Live win messages are built before the win is appended to the
        // transition queue, so derive the shooter from the event itself.
        state_payload["result_source_actor"] = *context_event->result_source_actor;
    } else if (last_transition.has_value() &&
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
        seat_payload["abandoned"] = current.abandoned;
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

        if (!spectator && static_cast<int>(index) == seat) {
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

    const PendingStatus pending =
        (spectator || lifecycle_.ended)
            ? PendingStatus::kPendingNone
            : scheduled_pending_[seat].value_or(seats_[seat].pending);
    auto decision_timer_ms = [&]() -> std::optional<int> {
        if (spectator || lifecycle_.ended) {
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
        !spectator && !lifecycle_.ended &&
        state_.next_transition.has_value() &&
        state_.next_transition->kind == EventKind::kDiscardTile &&
        state_.next_transition->actor_seat == seat;

    Json::Value viewer(Json::objectValue);
    viewer["seat_index"] = spectator ? 0 : seat;
    viewer["spectator"] = spectator;
    viewer["pending"] = std::string(PendingStatusName(pending));
    if (decision_timer_ms.has_value()) {
        viewer["decision_timer_ms"] = std::max(0, *decision_timer_ms - GameConfig::network_delay_ms);
    } else {
        viewer["decision_timer_ms"] = Json::Value(Json::nullValue);
    }
    std::optional<mahjong::tile_t> reaction_tile;
    int relative_to_target = 0;
    const Event* reaction_source = nullptr;
    if (!lifecycle_.ended && context_event != nullptr &&
        (context_event->kind == EventKind::kDiscardTile ||
         context_event->kind == EventKind::kAddedKong) &&
        context_event->actor_seat != seat &&
        context_event->tile.has_value()) {
        reaction_source = context_event;
    } else if (!lifecycle_.ended && !transition_queue_.empty()) {
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
    if (spectator || lifecycle_.ended) {
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
    if (!spectator && !lifecycle_.ended && context_event == nullptr &&
        pending_start_timers_[seat].isRunning()) {
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
        payload["result_event"] = spectator
            ? SerializeSpectatorEvent(*last_transition, state_.stage_counter)
            : SerializeVisibleEvent(*last_transition, seat, state_.stage_counter);
    } else {
        payload["result_event"] = Json::Value(Json::nullValue);
    }

    Json::Value ratings_array(Json::arrayValue);
    for (const auto& r : recording_.ratings) {
        ratings_array.append(r.ToJson());
    }
    payload["ratings"] = std::move(ratings_array);

    return payload;
}

auto ActiveSession::build_spectator_hand_payload(int seat) const
    -> util::StatusOr<Json::Value> {
    std::lock_guard lock(state_.mutex);
    if (seat < 0 || seat >= static_cast<int>(seats_.size())) {
        return util::Status::InvalidArgument("invalid spectator hand seat");
    }

    const Seat& target = seats_[seat];
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(identity_.id);
    payload["seat_index"] = seat;
    payload["stage_counter"] = Json::UInt64(state_.stage_counter);
    payload["hand_tiles"] = SerializeTiles(target.hand_tiles);
    if (target.has_drawn_tile()) {
        payload["drawn_tile"] = static_cast<Json::UInt>(target.drawn_tile);
    } else {
        payload["drawn_tile"] = Json::Value(Json::nullValue);
    }
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
        for (const auto& r : recording_.ratings) {
            ratings_array.append(r.ToJson());
        }
        payload["ratings"] = std::move(ratings_array);
    }
    if (event.kind == EventKind::kEnd && !recording_.final_ratings.empty()) {
        Json::Value final_arr(Json::arrayValue);
        for (const auto& r : recording_.final_ratings) {
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
        seat_payload["player_id"] = current["player_id"];
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

auto ActiveSession::build_event_message_for_spectator(
    const Event& event,
    std::string_view category) const -> Json::Value {
    Json::Value message = build_event_message_for_player(-1, event, category);
    message["payload"]["event"] = SerializeSpectatorEvent(event, state_.stage_counter);
    message["payload"]["spectator"] = true;
    return message;
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
