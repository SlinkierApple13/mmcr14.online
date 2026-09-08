#include "game/engine/session_internal.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>

#include "game/hub/hub.h"
#include "random/seed.h"
#include "external/qingque/rules/w_data.h"

namespace mmcr::game {

// ---------------------------------------------------------------------------
// Inbound message parsing helpers
// ---------------------------------------------------------------------------

const Json::Value* FindPayload(const Json::Value& message) {
    if (!message.isObject()) {
        return nullptr;
    }

    const Json::Value& payload = message["payload"];
    if (!payload.isObject()) {
        return nullptr;
    }
    return &payload;
}

std::optional<EventKind> ParseEventKind(std::string_view value) {
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

std::optional<bool> ReadOptionalBool(const Json::Value& object, std::string_view name) {
    const Json::Value& value = object[std::string(name)];
    if (!value.isBool()) {
        return std::nullopt;
    }
    return value.asBool();
}

std::optional<std::uint64_t> ReadOptionalUInt64(const Json::Value& object, std::string_view name) {
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

std::optional<mahjong::tile_t> ReadOptionalTile(const Json::Value& object, std::string_view name) {
    auto value = ReadOptionalUInt64(object, name);
    if (!value.has_value() || *value > std::numeric_limits<mahjong::tile_t>::max()) {
        return std::nullopt;
    }
    return static_cast<mahjong::tile_t>(*value);
}

bool IsPassMarginClaim(EventKind kind) {
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

Json::Value BuildPassAckEnvelope(std::uint64_t stage_counter) {
    Json::Value payload(Json::objectValue);
    payload["stage_counter"] = Json::UInt64(stage_counter);

    Json::Value envelope(Json::objectValue);
    envelope["version"] = 1;
    envelope["type"] = "game.pass.ack";
    envelope["payload"] = std::move(payload);
    return envelope;
}

// ---------------------------------------------------------------------------
// Construction, lifecycle and routing
// ---------------------------------------------------------------------------

ActiveSession::ActiveSession(random::SeedContainer* seed_container,
                             GameHub* hub,
                             std::int64_t session_id,
                             std::array<auth::PlayerProfilePtr, 4> players,
                             GameConfig config,
                             storage::GameRecordManager* record_manager)
    : config_(std::move(config)),
      seed_container_(seed_container),
      hub_(hub),
      record_manager_(record_manager),
      identity_(SessionIdentity{ .id = session_id }) {
    if (recording_enabled()) {
        identity_.init_timestamp_ns = now_ns();
        identity_.identifier =
            std::to_string(identity_.id) + "_" + std::to_string(identity_.init_timestamp_ns);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        seats_[i].player = players[i];
    }
    mode_ = CreateModeController(config_);
    init();
}

util::Status ActiveSession::handle_message(std::int64_t player_id, const Json::Value& message) {
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

util::Status ActiveSession::player_leaves(std::int64_t player_id) {
    const auto seat_index = find_seat_index(player_id);
    if (!seat_index.has_value()) {
        return util::Status::NotFound("player is not in the active session");
    }

    Event event;
    event.kind = EventKind::kPlayerLeft;
    event.actor_seat = *seat_index;
    event.timestamp_ms = now_ms();
    return handle_event(event);
}

util::Status ActiveSession::player_resumes(std::int64_t player_id) {
    const auto seat_index = find_seat_index(player_id);
    if (!seat_index.has_value()) {
        return util::Status::NotFound("player is not in the active session");
    }

    Event event;
    event.kind = EventKind::kPlayerResumed;
    event.actor_seat = *seat_index;
    event.timestamp_ms = now_ms();
    return handle_event(event);
}

util::StatusOr<Json::Value> ActiveSession::build_snapshot_for_player_id(std::int64_t player_id) const {
    std::lock_guard lock(state_.mutex);
    const auto seat_index = find_seat_index(player_id);
    if (!seat_index.has_value()) {
        return util::Status::NotFound("player is not in the active session");
    }

    return build_snapshot_for_player(*seat_index);
}

Json::Value ActiveSession::build_snapshot_for_spectator(int perspective_seat) const {
    std::lock_guard lock(state_.mutex);
    Json::Value snapshot = build_snapshot_for_player(-1);
    snapshot["viewer"]["seat_index"] = perspective_seat;
    return snapshot;
}

std::optional<int> ActiveSession::find_seat_index(std::int64_t player_id) const {
    for (std::size_t index = 0; index < seats_.size(); ++index) {
        if (seats_[index].player.matches(player_id)) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Delivery and timers
// ---------------------------------------------------------------------------

int ActiveSession::send_message(int target_seat, const Json::Value& message, int delay_ms) {
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
        lifecycle_.ended = false;
        lifecycle_.ended_at_ms = 0;
        lifecycle_.final_scores.fill(0);
    }
    // Fetch initial ratings for all players
    if (hub_ != nullptr && hub_->transport() != nullptr) {
        std::array<std::int64_t, 4> player_ids{};
        for (int i = 0; i < 4; ++i) {
            const auto player = seats_[i].player.lock();
            player_ids[i] = (player != nullptr) ? player->player_id : 0;
        }
    // Store ratings as vector; inject into first event below
        recording_.ratings = hub_->transport()->get_player_ratings(player_ids);
        // Inject usernames from seats
        for (std::size_t i = 0; i < recording_.ratings.size() && i < 4; ++i) {
            const auto player = seats_[i].player.lock();
            if (player != nullptr) {
                recording_.ratings[i].username = player->username;
            }
        }
    }
    random_pause_rng_.seed(seed_container_->Extract());
    if (mode_ != nullptr) {
        // Consumed BEFORE the first kStart's wall seeds so that mode data
        // (e.g. pass-five-gates targets) is reproducible in replays.
        mode_->OnSessionStart(seed_container_);
    }
    (void)handle_event(Event{ .kind = EventKind::kStart, .actor_seat = 0 });
    set_timer(GameConfig::minimal_transition_ms, 0);
}

void ActiveSession::end_session(std::int64_t timestamp_ms, bool enqueue_record) {
    std::lock_guard lock(state_.mutex);
    if (lifecycle_.ended) {
        return;
    }

    lifecycle_.ended = true;
    lifecycle_.ended_at_ms = timestamp_ms > 0 ? timestamp_ms : now_ms();
    state_.next_transition.reset();
    state_.current_player = -1;
    transition_timer_.stop();

    for (int seat = 0; seat < 4; ++seat) {
        scheduled_pending_[seat].reset();
        pending_start_timers_[seat].stop();
        end_wait(seat, lifecycle_.ended_at_ms);
        lifecycle_.final_scores[seat] = seats_[seat].score;
    }
    next_transition_not_before_ms_ = 0;

    if (session_end_callback_ && !config_.unranked) {
        std::array<std::int64_t, 4> player_ids{};
        for (int seat = 0; seat < 4; ++seat) {
            const auto player = seats_[seat].player.lock();
            player_ids[seat] = player != nullptr ? player->player_id : 0;
        }
        session_end_callback_(identity_.id, player_ids, lifecycle_.final_scores,
            static_cast<int>(state_.round_counter));
    }

    // Re-fetch final ratings for record storage
    if (hub_ != nullptr && hub_->transport() != nullptr && recording_enabled()) {
        std::array<std::int64_t, 4> player_ids{};
        for (int i = 0; i < 4; ++i) {
            const auto player = seats_[i].player.lock();
            player_ids[i] = (player != nullptr) ? player->player_id : 0;
        }
        recording_.final_ratings = hub_->transport()->get_player_ratings(player_ids);
    }

    if (enqueue_record) {
        const bool should_append_end_transition =
            recording_enabled() &&
            recording_.start_snapshot.has_value() &&
            (transition_queue_.empty() || transition_queue_.back().kind != EventKind::kEnd);
        if (should_append_end_transition) {
            Event transition{
                .kind = EventKind::kEnd,
                .actor_seat = 0,
                .timestamp_ms = lifecycle_.ended_at_ms,
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

// ---------------------------------------------------------------------------
// Wait management
// ---------------------------------------------------------------------------

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

PendingStatus ActiveSession::update_pending_status(int seat, std::int64_t now) {
    if (seat < 0 || seat >= 4) {
        return PendingStatus::kPendingNone;
    }
    Seat& s = seats_[seat];
    // A seat whose wait is scheduled but has not yet started (pending_from_ms == 0
    // while scheduled_pending_ still holds a value) must be reported as still
    // pending. Falling through would set pending = kPendingNone and trigger the
    // destructive end_wait() below, which resets scheduled_pending_[seat] and
    // prevents the per-seat start timer from ever opening the wait.
    if (scheduled_pending_[seat].has_value()) {
        return *scheduled_pending_[seat];
    }
    if (s.pending_from_ms <= 0) {
        s.pending = PendingStatus::kPendingNone;
    }
    if (s.pending == PendingStatus::kPendingPrimary) {
        if (now - s.pending_from_ms > 
            GameConfig::with_margin(config_.primary_timer_ms) + seats_[seat].auxiliary_ms) {
            this->end_wait(seat, now);
        }
    } else if (s.pending == PendingStatus::kPendingSecondary) {
        if (now - s.pending_from_ms > 
            GameConfig::with_margin(config_.secondary_timer_ms)) {
            this->end_wait(seat, now);
            s.pending = PendingStatus::kPendingSlept;
        }
    }
    return s.pending;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

util::Status ActiveSession::handle_event(const Event& event) {
    static constexpr int kPriorityStart = 100;
    static constexpr int kPrioritySelf = 90;
    static constexpr int kPriorityChow = 10;
    static constexpr int kPriorityPung = 20;
    static constexpr int kPriorityWinDiscard = 40;

    std::unique_lock lock(state_.mutex);
    const auto now = now_ms();

    auto claim_finalised = [this, &now]() {
        for (int i = 0; i < 4; ++i) {
            if (update_pending_status(i, now) != PendingStatus::kPendingNone) {
                return false;
            }
        }
        return true;
    };

    auto time_left = [this, &now](int minimum, int minimum_after_last) {
        int time_left_ms = minimum;
        // Last timestamp: the later of
        // 1. the last transition timestamp, or
        // 2. the most recent Chow/Pung/MeldedKong/DiscardWin/RobAddedKongWin event timestamp except the last 
        //    (since the current event is already added into event list), if any
        int64_t last_timestamp_ms = transition_queue_.empty() ? 0 : transition_queue_.back().timestamp_ms;
        for (auto it = event_queue_.end() - 1; it != event_queue_.begin() - 1; --it) {
            if (it->kind == EventKind::kChow || it->kind == EventKind::kPung ||
                it->kind == EventKind::kMeldedKong || it->kind == EventKind::kDiscardWin ||
                it->kind == EventKind::kRobAddedKongWin) {
                last_timestamp_ms = std::max(last_timestamp_ms, it->timestamp_ms);
                break;
            }
        }
        time_left_ms = std::max(time_left_ms, 
            minimum_after_last - static_cast<int>(now - last_timestamp_ms));
        for (int i = 0; i < 4; ++i) {
            auto pending_status = update_pending_status(i, now);
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
            if (update_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
                update_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
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
            if (update_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
                update_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
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
            if (update_pending_status(event.actor_seat, now) == PendingStatus::kPendingNone || 
                update_pending_status(event.actor_seat, now) == PendingStatus::kPendingSlept) {
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
    recorded_event.round_turn = recording_.turn;
    event_queue_.push_back(std::move(recorded_event));

    if (state_.stage_counter != event.stage_counter && event.stage_counter != 0) {
        return util::Status::Ok();
    }

    std::optional<EventKind> afk_status_broadcast;
    if (event.kind != EventKind::kPlayerLeft && event.kind != EventKind::kPlayerResumed) {
        const bool was_afk = seats_[event.actor_seat].is_afk();
        const bool was_disconnected = seats_[event.actor_seat].disconnected;
        seats_[event.actor_seat].afk_counter = 0;
        seats_[event.actor_seat].disconnected = false;
        if ((was_afk || was_disconnected) && !seats_[event.actor_seat].is_afk()) {
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
        Event event_{
            .kind = *afk_status_broadcast,
            .actor_seat = event.actor_seat,
            .timestamp_ms = now,
            .stage_counter = event.stage_counter,
        };
        event_queue_.push_back(event_);
        broadcast_claim(event_);
    }

    return util::Status::Ok();
}

// ---------------------------------------------------------------------------
// Transition execution
// ---------------------------------------------------------------------------

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
        recording_.turn = 0;
        recording_.turn_actor = 3;
        recording_.meld_count.fill(0);
    }
    transition.round_turn =
        (transition.kind == EventKind::kStart || transition.kind == EventKind::kPredraw)
            ? 0
            : recording_.turn;
    if (StartsStoredRoundTurn(transition.kind) &&
        transition.actor_seat >= 0 && transition.actor_seat < 4) {
        AdvanceStoredRoundTurn(
            transition.actor_seat,
            &recording_.turn_actor,
            &recording_.turn);
    }
    if (CountsAsStoredMeld(transition.kind) &&
        transition.actor_seat >= 0 && transition.actor_seat < 4) {
        ++recording_.meld_count[static_cast<std::size_t>(transition.actor_seat)];
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
        // 击飞 (forced end floor): any seat below the floor ends the session.
        if (config_.forced_end_floor.has_value()) {
            for (int seat = 0; seat < 4; ++seat) {
                if (seats_[seat].score < *config_.forced_end_floor) {
                    return Event{ .kind = EventKind::kEnd, .actor_seat = 0 };
                }
            }
        }
        const EventKind next_kind = (mode_ != nullptr)
            ? mode_->NextTransition(state_.round_counter, config_.round_count)
            : [this]() {
                  const auto configured_round_count =
                      static_cast<std::uint64_t>(std::max(config_.round_count, 0));
                  return (configured_round_count != 0 &&
                          state_.round_counter >= configured_round_count)
                      ? EventKind::kEnd
                      : EventKind::kStart;
              }();
        return Event{ .kind = next_kind, .actor_seat = 0 };
    };

    // Runs after a winning hand's scores have been applied. Lets the mode
    // decide whether the session ends immediately (e.g. all five gates
    // completed). raw_fans is the union of fan indices across every winning
    // decomposition ("以原始的为准", not the highest-fan split).
    auto settle_round = [this, &transition, &next_round_transition](int actor_seat,
                                                                    const std::vector<int>& raw_fans) {
        std::array<int, 4> scores{};
        for (int i = 0; i < 4; ++i) {
            scores[i] = seats_[i].score;
        }
        const TransitionDecision mode_decision = (mode_ != nullptr)
            ? mode_->OnRoundSettled(actor_seat, raw_fans, scores)
            : TransitionDecision::kContinue;
        state_.next_transition = (mode_decision == TransitionDecision::kEnd)
            ? Event{ .kind = EventKind::kEnd, .actor_seat = 0 }
            : next_round_transition();
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
            Event event{
                .kind = EventKind::kPlayerLeft,
                .actor_seat = transition.actor_seat,
                .timestamp_ms = transition.timestamp_ms,
                .stage_counter = state_.stage_counter,
            };
            event_queue_.push_back(event);
            broadcast_claim(event);
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
                std::rotate(seats_.rbegin(), seats_.rbegin() + 3, seats_.rend());
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
            if (mode_ != nullptr) {
                mode_->SetSeatPlayers(snapshot.player_ids);
                mode_->OnRoundStart(state_.round_counter);
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
            transition.winning_hand = HandWrapper{
                .melds = seats_[transition.actor_seat].melds,
                .hand_tiles = SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles),
                .winning_tile = ti,
                .winning_type = mahjong::win_type(wtype),
            };
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
            settle_round(transition.actor_seat, RawFanIndices(hand));
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
            transition.winning_hand = HandWrapper{
                .melds = seats_[transition.actor_seat].melds,
                .hand_tiles = SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles),
                .winning_tile = ti,
                .winning_type = mahjong::win_type(wtype),
            };
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
            settle_round(transition.actor_seat, RawFanIndices(hand));
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
            transition.winning_hand = HandWrapper{
                .melds = seats_[transition.actor_seat].melds,
                .hand_tiles = SortTilesForDisplay(seats_[transition.actor_seat].hand_tiles),
                .winning_tile = seats_[transition.actor_seat].drawn_tile,
                .winning_type = mahjong::win_type(wtype),
            };
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
            settle_round(transition.actor_seat, RawFanIndices(hand));
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
            if (mode_ != nullptr) {
                std::array<int, 4> final_scores{};
                for (int seat = 0; seat < 4; ++seat) {
                    final_scores[seat] = seats_[seat].score;
                }
                Json::Value mode_result = mode_->OnSessionEnd(final_scores);
                if (!mode_result.isNull() && !mode_result.empty()) {
                    transition.mode_result = std::move(mode_result);
                }
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

// ---------------------------------------------------------------------------
// Delivery flow
// ---------------------------------------------------------------------------

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
 * We use this algorithm to hide meld pauses:
 * 1. After the start transition, check which players have meld options.
 *    If no players or all players have meld options, do nothing special.
 *    Otherwise, mark those without meld options as "delayed."
 *    At the start transition, set a variable meld_offset_ms_ = Config::meld_offset_ms.
 * 2. Broadcast all claims and transitions in the basic interval meld_offset_ms_
 *    later for "delayed" players, INCLUDING the starting transition but EXCEPT the ending 
 *    transition, whose rules are listed below.
 * 3. If a player draws a tile during the basic interval, let T_last = [time of 
 *    the second to last transition or the last claim event (except player left 
 *    and player resumed), whichever is later]. Set new_offset = max(T_last + Config::
 *    minimal_transition_ms + meld_offset_ms_ - now_ms(), 0) before broadcasting. 
 * 4. Let T_crit = [(meld_offset_ms_ + 
 *    Config::minimal_transition_ms) after the second to last transition or the last claim 
 *    event (except player left and player resumed), whichever is later].
 *    If the ending transition is later than T_crit, broadcast it normally. Otherwise, still
 *    broadcast it at T_crit for the other players. 
 *    Note that since the ending transition also marks the start of
 *    the next basic interval, if that interval triggers the "delayed" players rule, the
 *    broadcast of the ending transition to those players will be further delayed.
 * 5. On top of the above rules, an extra delay of a random duration between 0 and 
 *    Config::random_pause_range is applied with probability Config::random_pause_prob
 *    to all players except the actor on discard tile or added kong transitions.
 * 6. Only start the wait window (start_primary_wait() or start_secondary_wait()) for a 
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
    auto policies = MsgOnClaim(event, seats_, interval_delayed_seats_, meld_offset_ms_, dispatch_now_ms);
    const bool track_claim_delivery =
        event.kind != EventKind::kPlayerLeft && event.kind != EventKind::kPlayerResumed;
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
        if (track_claim_delivery) {
            last_protection_claim_event_ms_ =
                std::max(last_protection_claim_event_ms_, event.timestamp_ms > 0 ? event.timestamp_ms : dispatch_now_ms);
        }
    }

    int spectator_delay_ms = 0;
    for (const auto& policy : policies) {
        spectator_delay_ms = std::max(spectator_delay_ms, policy.delay_ms);
    }
    if (hub_ != nullptr) {
        hub_->send_to_spectators(
            identity_.id,
            build_event_message_for_spectator(event, "claim"),
            spectator_delay_ms);
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
    if (transition.kind == EventKind::kDrawTile) {
        const int previous_meld_offset_ms = meld_offset_ms_;
        const std::int64_t previous_transition_ms =
            previous_transition.has_value() ? previous_transition->timestamp_ms : 0;
        const std::int64_t last_interval_event_ms =
            std::max(previous_transition_ms, last_protection_claim_event_ms_);
        meld_offset_ms_ = static_cast<int>(std::max<std::int64_t>(
            0,
            last_interval_event_ms + GameConfig::minimal_transition_ms + previous_meld_offset_ms - dispatch_now_ms));
    }
    auto policies = MsgOnTransition(
        transition,
        seats_,
        interval_delayed_seats_,
        next_interval_delayed_seats,
        previous_transition,
        meld_offset_ms_,
        GameConfig::meld_offset_ms,
        last_protection_claim_event_ms_,
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
        if (hub_ != nullptr) {
            hub_->send_to_spectators(
                identity_.id,
                build_event_message_for_spectator(transition, "transition"),
                max_delivery_delay_ms);
        }
    }

    transition_queue_.push_back(transition);
    if (IsSyncCheckpoint(transition.kind)) {
        interval_delayed_seats_ = next_interval_delayed_seats;
        meld_offset_ms_ = GameConfig::meld_offset_ms;
        last_protection_claim_event_ms_ = 0;
    } else if (transition.kind == EventKind::kStart || transition.kind == EventKind::kEnd) {
        interval_delayed_seats_.fill(false);
        meld_offset_ms_ = GameConfig::meld_offset_ms;
        last_protection_claim_event_ms_ = 0;
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
                fallback_delay_ms = static_cast<int>(GameConfig::minimal_transition_ms * 0.55);
            }
            break;

        default:
            break;
    }

    if (IsSyncCheckpoint(transition.kind)) {
        next_timer_delay_ms = std::max(next_timer_delay_ms, max_delivery_delay_ms + fallback_delay_ms);
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

}  // namespace mmcr::game
