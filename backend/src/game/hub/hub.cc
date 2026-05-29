#include "game/hub/hub.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "game/engine/session.h"

namespace mmcr::game {
namespace {

auto BuildEnvelope(std::string_view type, Json::Value payload) -> Json::Value {
    Json::Value envelope(Json::objectValue);
    envelope["version"] = 1;
    envelope["type"] = std::string(type);
    envelope["payload"] = std::move(payload);
    return envelope;
}

auto FindMessageType(const Json::Value& message) -> std::optional<std::string> {
    if (!message.isObject()) {
        return std::nullopt;
    }

    const Json::Value& value = message["type"];
    if (!value.isString()) {
        return std::nullopt;
    }
    return value.asString();
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

auto ReadRequiredInt64(const Json::Value& object, std::string_view name)
    -> util::StatusOr<std::int64_t> {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const Json::Value& value = object[std::string(name)];
    if (value.isInt64()) {
        return value.asInt64();
    }
    if (value.isInt()) {
        return static_cast<std::int64_t>(value.asInt());
    }
    if (value.isString()) {
        std::int64_t parsed_value = 0;
        const std::string raw_value = value.asString();
        const auto result = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
        if (result.ec == std::errc() && result.ptr == raw_value.data() + raw_value.size()) {
            return parsed_value;
        }
    }

    return util::Status::InvalidArgument(std::string(name) + " must be an integer");
}

auto ReadOptionalInt64(const Json::Value& object, std::string_view name)
    -> util::StatusOr<std::optional<std::int64_t>> {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const std::string key(name);
    if (!object.isMember(key) || object[key].isNull()) {
        return std::optional<std::int64_t>{};
    }

    const Json::Value& value = object[key];
    if (value.isInt64()) {
        return std::optional<std::int64_t>(value.asInt64());
    }
    if (value.isInt()) {
        return std::optional<std::int64_t>(static_cast<std::int64_t>(value.asInt()));
    }
    if (value.isString()) {
        std::int64_t parsed_value = 0;
        const std::string raw_value = value.asString();
        const auto result = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
        if (result.ec == std::errc() && result.ptr == raw_value.data() + raw_value.size()) {
            return std::optional<std::int64_t>(parsed_value);
        }
    }

    return util::Status::InvalidArgument(std::string(name) + " must be an integer");
}

auto ReadRequiredBool(const Json::Value& object, std::string_view name) -> util::StatusOr<bool> {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const Json::Value& value = object[std::string(name)];
    if (!value.isBool()) {
        return util::Status::InvalidArgument(std::string(name) + " must be a boolean");
    }
    return value.asBool();
}

auto BuildResumeRequiredEnvelope(std::int64_t session_id) -> Json::Value {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(session_id);
    return BuildEnvelope("resume.required", std::move(payload));
}

auto UpsertKnownPlayer(
    std::unordered_map<std::int64_t, std::shared_ptr<auth::PlayerProfile>>& known_players,
    const auth::PlayerProfile& player) -> std::shared_ptr<auth::PlayerProfile> {
    auto& slot = known_players[player.player_id];
    if (slot == nullptr) {
        slot = std::make_shared<auth::PlayerProfile>(player);
    } else {
        *slot = player;
    }
    return slot;
}

auto BuildPendingSummary(const PendingSession& session) -> PendingSessionSummary {
    int occupied_seat_count = 0;
    int ready_seat_count = 0;
    for (const auto& seat : session.seats()) {
        if (seat.player.valid()) {
            ++occupied_seat_count;
            if (seat.ready) {
                ++ready_seat_count;
            }
        }
    }

    std::vector<std::string> names;
    for (const auto& seat : session.seats()) {
        auto player = seat.player.lock();
        if (player) {
            names.push_back(player->username);
        } else {
            names.push_back("");
        }
    }

    return PendingSessionSummary{
        .session_id = session.session_id(),
        .occupied_seat_count = occupied_seat_count,
        .ready_seat_count = ready_seat_count,
        .primary_timer_ms = session.game_config().primary_timer_ms,
        .secondary_timer_ms = session.game_config().secondary_timer_ms,
        .auxiliary_timer_ms = session.game_config().auxiliary_timer_ms,
        .round_count = session.game_config().round_count,
        .recorded = session.game_config().recorded,
        .debug_mode = session.game_config().debug_mode,
        .public_session = session.queue_config().public_session,
        .can_join = occupied_seat_count < static_cast<int>(session.seats().size()),
        .can_start = occupied_seat_count == static_cast<int>(session.seats().size()) &&
                     ready_seat_count == static_cast<int>(session.seats().size()),
        .names = std::move(names),
    };
}

auto SerializePendingSummary(const PendingSessionSummary& summary) -> Json::Value {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(summary.session_id);
    payload["occupied_seat_count"] = summary.occupied_seat_count;
    payload["ready_seat_count"] = summary.ready_seat_count;
    payload["primary_timer_ms"] = summary.primary_timer_ms;
    payload["secondary_timer_ms"] = summary.secondary_timer_ms;
    payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
    payload["round_count"] = summary.round_count;
    payload["recorded"] = summary.recorded;
    payload["debug_mode"] = summary.debug_mode;
    payload["public_session"] = summary.public_session;
    payload["can_join"] = summary.can_join;
    payload["can_start"] = summary.can_start;
    Json::Value names(Json::arrayValue);
    for (const auto& name : summary.names) {
        names.append(name);
    }
    payload["names"] = std::move(names);
    return payload;
}

auto SerializePendingSummaryList(const std::vector<PendingSessionSummary>& sessions) -> Json::Value {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        payload.append(SerializePendingSummary(session));
    }
    return payload;
}

auto SerializeActiveSummaryList(const std::vector<ActiveSessionSummary>& sessions) -> Json::Value {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        Json::Value entry(Json::objectValue);
        entry["session_id"] = Json::Int64(session.session_id);
        entry["primary_timer_ms"] = session.primary_timer_ms;
        entry["secondary_timer_ms"] = session.secondary_timer_ms;
        entry["auxiliary_timer_ms"] = session.auxiliary_timer_ms;
        entry["round_count"] = session.round_count;
        entry["round_counter"] = Json::UInt64(session.round_counter);
        entry["recorded"] = session.recorded;
        entry["debug_mode"] = session.debug_mode;
        entry["ended"] = session.ended;
        entry["public_session"] = session.public_session;
        Json::Value names(Json::arrayValue);
        for (const auto& name : session.names) {
            names.append(name);
        }
        entry["names"] = std::move(names);
        payload.append(std::move(entry));
    }
    return payload;
}

auto SerializePendingSeat(const PendingSeat& seat) -> Json::Value {
    Json::Value payload(Json::objectValue);
    payload["seat_index"] = seat.seat_index;
    payload["ready"] = seat.ready;
    const auto player = seat.player.lock();
    if (player != nullptr) {
        payload["player_id"] = Json::Int64(player->player_id);
        payload["username"] = player->username;
    } else {
        payload["player_id"] = Json::Value(Json::nullValue);
        payload["username"] = Json::Value(Json::nullValue);
    }
    return payload;
}

auto BuildPendingSnapshot(const PendingSession& session) -> PendingSessionSnapshot {
    return PendingSessionSnapshot{
        .summary = BuildPendingSummary(session),
        .seats = session.seats(),
    };
}

auto SerializePendingSnapshot(const PendingSessionSnapshot& snapshot) -> Json::Value {
    Json::Value payload(Json::objectValue);
    payload["phase"] = "pending";
    payload["summary"] = SerializePendingSummary(snapshot.summary);

    Json::Value seats(Json::arrayValue);
    for (const auto& seat : snapshot.seats) {
        seats.append(SerializePendingSeat(seat));
    }
    payload["seats"] = std::move(seats);
    return payload;
}

void BroadcastPendingSnapshot(GameHub& hub, const PendingSession& session) {
    const auto snapshot = BuildPendingSnapshot(session);
    Json::Value payload = SerializePendingSnapshot(snapshot);
    
    // Include ratings for all players in the session
    std::array<std::int64_t, 4> player_ids{};
    const auto& seats = session.seats();
    for (std::size_t i = 0; i < seats.size(); ++i) {
        const auto player = seats[i].player.lock();
        player_ids[i] = (player != nullptr) ? player->player_id : 0;
    }
    if (auto* transport = hub.transport(); transport != nullptr) {
        auto ratings = transport->get_player_ratings(player_ids);
        if (!ratings.empty()) {
            Json::Value ratings_arr(Json::arrayValue);
            for (const auto& r : ratings) {
                ratings_arr.append(r.ToJson());
            }
            payload["ratings"] = std::move(ratings_arr);
        }
    }
    
    const Json::Value envelope = BuildEnvelope("session.snapshot", std::move(payload));
    for (const auto& seat : snapshot.seats) {
        const auto player = seat.player.lock();
        if (player != nullptr) {
            hub.send_to_player(player->player_id, envelope);
        }
    }
}

auto BuildActiveSummary(const ActiveSession& session) -> ActiveSessionSummary {
    ActiveSessionSummary summary;
    summary.session_id = session.session_id();
    summary.primary_timer_ms = session.config().primary_timer_ms;
    summary.secondary_timer_ms = session.config().secondary_timer_ms;
    summary.auxiliary_timer_ms = session.config().auxiliary_timer_ms;
    summary.round_count = session.config().round_count;
    summary.round_counter = session.state().round_counter;
    summary.recorded = session.config().recorded;
    summary.debug_mode = session.config().debug_mode;
    summary.ended = session.ended();
    summary.public_session = session.public_session();
    for (const auto& seat : session.seats()) {
        const auto player = seat.player.lock();
        if (player != nullptr) {
            summary.names.push_back(player->username);
        }
    }
    return summary;
}

}  // namespace

GameHub::GameHub(random::SeedContainer* seed_container,
                                 GameTransport* transport,
                                 storage::GameRecordManager* record_manager)
    : seed_container_(seed_container),
      transport_(transport),
            record_manager_(record_manager),
    session_id_rng_(seed_container != nullptr ? seed_container->Extract() : std::random_device{}()),
      gc_thread_(&GameHub::garbage_collect_loop, this) {}

GameHub::~GameHub() {
    {
        std::lock_guard lock(gc_mutex_);
        gc_shutdown_ = true;
    }
    gc_cv_.notify_one();
    if (gc_thread_.joinable()) {
        gc_thread_.join();
    }
}

void GameHub::notify_session_lists_changed() {
    broadcast_joinable_sessions();
}

auto GameHub::allocate_session_id_locked() -> util::StatusOr<std::int64_t> {
    constexpr std::int64_t kMinSessionId = 1;
    constexpr std::int64_t kMaxSessionId = 999999;
    constexpr std::int64_t kSessionIdCount = kMaxSessionId - kMinSessionId + 1;

    if (static_cast<std::int64_t>(pending_sessions_.size() + active_sessions_.size()) >=
        kSessionIdCount) {
        return util::Status::Internal("no session ids available in the configured range");
    }

    std::uniform_int_distribution<std::int64_t> distribution(kMinSessionId, kMaxSessionId);
    for (std::int64_t attempts = 0; attempts < kSessionIdCount; ++attempts) {
        const std::int64_t candidate = distribution(session_id_rng_);
        if (pending_sessions_.contains(candidate) || active_sessions_.contains(candidate)) {
            continue;
        }
        return candidate;
    }

    return util::Status::Internal("failed to allocate a unique session id");
}

auto GameHub::allocate_unranked_session_id_locked() -> util::StatusOr<std::int64_t> {
    constexpr std::int64_t kMinUnrankedId = 1'000'000;
    constexpr std::int64_t kMaxUnrankedId = 9'999'999;
    constexpr std::int64_t kUnrankedIdCount = kMaxUnrankedId - kMinUnrankedId + 1;

    if (static_cast<std::int64_t>(pending_sessions_.size() + active_sessions_.size()) >=
        kUnrankedIdCount) {
        return util::Status::Internal("no unranked session ids available");
    }

    std::uniform_int_distribution<std::int64_t> distribution(kMinUnrankedId, kMaxUnrankedId);
    for (std::int64_t attempts = 0; attempts < kUnrankedIdCount; ++attempts) {
        const std::int64_t candidate = distribution(session_id_rng_);
        if (pending_sessions_.contains(candidate) || active_sessions_.contains(candidate)) {
            continue;
        }
        return candidate;
    }

    return util::Status::Internal("failed to allocate a unique unranked session id");
}

auto GameHub::create_session(const CreateGameSessionRequest& request)
    -> util::StatusOr<CreateGameSessionResult> {
    if (request.owner.player_id <= 0) {
        return util::Status::InvalidArgument("owner player_id must be positive");
    }

    auto game_config = request.game_config;
    if (request.queue_config.singleplayer) {
        game_config.recorded = false;
        game_config.unranked = true;
    }
    if (game_config.debug_mode) {
        game_config.recorded = false;
    }
    if (!game_config.recorded) {
        game_config.unranked = true;
    }
    const bool use_unranked_id = game_config.unranked || !game_config.recorded;

    std::int64_t session_id = 0;
    {
        std::unique_lock lock(mutex_);
        if (player_pending_sessions_.contains(request.owner.player_id) ||
            player_active_sessions_.contains(request.owner.player_id)) {
            return util::Status::InvalidArgument("owner is already in a session");
        }

        auto owner = UpsertKnownPlayer(known_players_, request.owner);
        auto allocated_session_id = use_unranked_id
            ? allocate_unranked_session_id_locked()
            : allocate_session_id_locked();
        if (!allocated_session_id.ok()) {
            return allocated_session_id.status();
        }
        session_id = allocated_session_id.value();
        if (request.queue_config.singleplayer) {
            std::array<auth::PlayerProfilePtr, 4> players;
            players[0] = auth::PlayerProfilePtr(owner);
            for (std::size_t index = 1; index < players.size(); ++index) {
                auth::PlayerProfile virtual_player;
                virtual_player.player_id = -static_cast<std::int64_t>(session_id * 10 + static_cast<std::int64_t>(index));
                virtual_player.username = "";
                auto handle = UpsertKnownPlayer(known_players_, virtual_player);
                players[index] = auth::PlayerProfilePtr(handle);
            }

            active_sessions_.emplace(
                session_id,
                std::make_unique<ActiveSession>(
                    seed_container_,
                    this,
                    session_id,
                    players,
                    game_config,
                    false,
                    record_manager_));
            if (auto it = active_sessions_.find(session_id); it != active_sessions_.end() && transport_ != nullptr) {
                it->second->set_session_end_callback(
                    [transport = transport_](std::int64_t sid,
                        const std::array<std::int64_t, 4>& pids,
                        const std::array<int, 4>& scores,
                        int rounds) {
                        transport->on_session_ended(sid, pids, scores, rounds);
                    });
            }
            player_active_sessions_[request.owner.player_id] = session_id;
        } else {
            auto [pending_it, inserted] = pending_sessions_.emplace(
                session_id,
                std::make_unique<PendingSession>(
                    this, session_id, game_config, request.queue_config));
            (void)inserted;
            auto join_status = pending_it->second->join_player(auth::PlayerProfilePtr(owner));
            if (!join_status.ok()) {
                pending_sessions_.erase(session_id);
                return join_status;
            }
            player_pending_sessions_[request.owner.player_id] = session_id;
        }
        browsing_players_.erase(request.owner.player_id);
    }

    broadcast_joinable_sessions();
    return CreateGameSessionResult{session_id};
}

auto GameHub::list_active_sessions() const -> std::vector<ActiveSessionSummary> {
    std::shared_lock lock(mutex_);
    std::vector<ActiveSessionSummary> sessions;
    sessions.reserve(active_sessions_.size());
    for (const auto& [session_id, session] : active_sessions_) {
        if (session == nullptr) {
            continue;
        }
        sessions.push_back(BuildActiveSummary(*session));
    }
    std::sort(
        sessions.begin(),
        sessions.end(),
        [](const ActiveSessionSummary& left, const ActiveSessionSummary& right) {
            return left.session_id < right.session_id;
        });
    return sessions;
}

auto GameHub::connect_player(const ConnectPlayerRequest& request) -> util::Status {
    if (request.player.player_id <= 0) {
        return util::Status::InvalidArgument("player_id must be positive");
    }

    if (!request.session_id.has_value()) {
        {
            std::unique_lock lock(mutex_);
            browsing_players_.insert(request.player.player_id);
        }
        broadcast_joinable_sessions();
        return util::Status::Ok();
    }

    PendingSession* target_session = nullptr;
    PendingSession* previous_pending_session = nullptr;
    std::optional<std::int64_t> previous_pending_session_id;
    bool send_resume_required = false;
    std::int64_t resume_session_id = 0;
    int resume_delay_ms = 0;
    {
        std::unique_lock lock(mutex_);
        browsing_players_.erase(request.player.player_id);

        auto player_active_it = player_active_sessions_.find(request.player.player_id);
        if (player_active_it != player_active_sessions_.end() &&
            player_active_it->second != *request.session_id) {
            return util::Status::InvalidArgument("player is already in another active session");
        }

        auto active_it = active_sessions_.find(*request.session_id);
        if (active_it != active_sessions_.end()) {
            // Verify the player is actually a seat holder in this session
            if (!active_it->second->has_player(request.player.player_id)) {
                return util::Status::NotFound("player is not in this active session");
            }
            (void)UpsertKnownPlayer(known_players_, request.player);
            player_active_sessions_[request.player.player_id] = *request.session_id;
            send_resume_required = true;
            resume_session_id = *request.session_id;
            resume_delay_ms = active_it->second->config().network_delay_ms;
        } else {
            if (player_active_it != player_active_sessions_.end()) {
                return util::Status::InvalidArgument("player is already in an active session");
            }

            auto pending_it = pending_sessions_.find(*request.session_id);
            if (pending_it == pending_sessions_.end()) {
                return util::Status::NotFound("session not found");
            }

            target_session = pending_it->second.get();

            // Try joining the new session BEFORE mutating the old session's
            // bookkeeping.  If the join fails (e.g. session became full between
            // the lobby snapshot and now) we must keep the player in their
            // original session, not leave them in a ghost seat.
            const auto player = UpsertKnownPlayer(known_players_, request.player);
            const auto join_status = target_session->join_player(auth::PlayerProfilePtr(player));
            if (!join_status.ok()) {
                return join_status;
            }

            auto previous_it = player_pending_sessions_.find(request.player.player_id);
            if (previous_it != player_pending_sessions_.end() && previous_it->second != *request.session_id) {
                previous_pending_session_id = previous_it->second;
                auto previous_session_it = pending_sessions_.find(*previous_pending_session_id);
                if (previous_session_it != pending_sessions_.end()) {
                    previous_pending_session = previous_session_it->second.get();
                }
                player_pending_sessions_.erase(previous_it);
            }

            player_pending_sessions_[request.player.player_id] = *request.session_id;
        }
    }

    if (send_resume_required) {
        send_to_player(request.player.player_id,
                       BuildResumeRequiredEnvelope(resume_session_id),
                       resume_delay_ms);
        return util::Status::Ok();
    }

    if (previous_pending_session_id.has_value()) {
        if (previous_pending_session != nullptr) {
            const auto leave_status = previous_pending_session->player_leaves(request.player.player_id);
            if (!leave_status.ok() && leave_status.code() != util::StatusCode::kNotFound) {
                return leave_status;
            }
            BroadcastPendingSnapshot(*this, *previous_pending_session);
        }
    }

    BroadcastPendingSnapshot(*this, *target_session);

    broadcast_joinable_sessions();
    return util::Status::Ok();
}

auto GameHub::disconnect_player(const DisconnectPlayerRequest& request) -> util::Status {
    PendingSession* pending_session = nullptr;
    ActiveSession* active_session = nullptr;
    {
        std::unique_lock lock(mutex_);
        browsing_players_.erase(request.player_id);

        auto pending_it = player_pending_sessions_.find(request.player_id);
        if (pending_it != player_pending_sessions_.end()) {
            auto session_it = pending_sessions_.find(pending_it->second);
            if (session_it != pending_sessions_.end()) {
                pending_session = session_it->second.get();
            }
            player_pending_sessions_.erase(pending_it);
        }

        auto active_it = player_active_sessions_.find(request.player_id);
        if (active_it != player_active_sessions_.end()) {
            auto session_it = active_sessions_.find(active_it->second);
            if (session_it != active_sessions_.end()) {
                active_session = session_it->second.get();
            }
        }
    }

    if (pending_session != nullptr) {
        auto status = pending_session->player_leaves(request.player_id);
        if (!status.ok() && status.code() != util::StatusCode::kNotFound) {
            return status;
        }
        BroadcastPendingSnapshot(*this, *pending_session);
        broadcast_joinable_sessions();
    }

    if (active_session != nullptr) {
        auto status = active_session->player_leaves(request.player_id);
        if (!status.ok() && status.code() != util::StatusCode::kNotFound) {
            return status;
        }
    }

    return util::Status::Ok();
}

auto GameHub::handle_message(const RouteGameMessageRequest& request) -> util::Status {
    const auto player_id = request.player.player_id();
    if (player_id <= 0) {
        return util::Status::InvalidArgument("player_id must be positive");
    }

    const auto message_type = FindMessageType(request.message);
    if (!message_type.has_value()) {
        return util::Status::InvalidArgument("message type is required");
    }

    if (*message_type == "lobby.list") {
        {
            std::unique_lock lock(mutex_);
            browsing_players_.insert(player_id);
        }
        broadcast_joinable_sessions();
        return util::Status::Ok();
    }

    if (*message_type == "session.join") {
        const Json::Value* payload = FindPayload(request.message);
        if (payload == nullptr) {
            return util::Status::InvalidArgument("payload must be a JSON object");
        }
        auto session_id = ReadRequiredInt64(*payload, "session_id");
        if (!session_id.ok()) {
            return session_id.status();
        }

        const auto player = request.player.lock();
        if (player == nullptr) {
            return util::Status::NotFound("player profile is no longer available");
        }

        return connect_player(ConnectPlayerRequest{
            .player = *player,
            .session_id = session_id.value(),
        });
    }

    if (*message_type == "session.leave") {
        const Json::Value* payload = nullptr;
        if (request.message.isObject() && request.message.isMember("payload")) {
            payload = FindPayload(request.message);
            if (payload == nullptr) {
                return util::Status::InvalidArgument("payload must be a JSON object");
            }
        }

        std::optional<std::int64_t> requested_session_id;
        if (payload != nullptr) {
            auto session_id = ReadOptionalInt64(*payload, "session_id");
            if (!session_id.ok()) {
                return session_id.status();
            }
            requested_session_id = session_id.value();
        }

        PendingSession* pending_session = nullptr;
        {
            std::unique_lock lock(mutex_);
            auto active_it = player_active_sessions_.find(player_id);
            if (active_it != player_active_sessions_.end()) {
                if (requested_session_id.has_value() && active_it->second != *requested_session_id) {
                    return util::Status::NotFound("player is not in requested session");
                }
                return util::Status::InvalidArgument("player cannot leave an active session");
            }

            auto pending_it = player_pending_sessions_.find(player_id);
            if (pending_it != player_pending_sessions_.end()) {
                if (requested_session_id.has_value() && pending_it->second != *requested_session_id) {
                    return util::Status::NotFound("player is not in requested pending session");
                }
                auto session_it = pending_sessions_.find(pending_it->second);
                if (session_it != pending_sessions_.end()) {
                    pending_session = session_it->second.get();
                }
                player_pending_sessions_.erase(pending_it);
            } else if (requested_session_id.has_value()) {
                return util::Status::NotFound("player is not in requested pending session");
            }
            browsing_players_.insert(player_id);
        }

        if (pending_session != nullptr) {
            auto status = pending_session->player_leaves(player_id);
            if (!status.ok() && status.code() != util::StatusCode::kNotFound) {
                return status;
            }
            BroadcastPendingSnapshot(*this, *pending_session);
        }
        broadcast_joinable_sessions();
        return util::Status::Ok();
    }

    if (*message_type == "queue.ready") {
        const Json::Value* payload = FindPayload(request.message);
        if (payload == nullptr) {
            return util::Status::InvalidArgument("payload must be a JSON object");
        }

        auto requested_session_id = ReadOptionalInt64(*payload, "session_id");
        if (!requested_session_id.ok()) {
            return requested_session_id.status();
        }

        PendingSession* pending_session = nullptr;
        {
            std::shared_lock lock(mutex_);
            auto pending_it = player_pending_sessions_.find(player_id);
            if (pending_it != player_pending_sessions_.end()) {
                if (requested_session_id.value().has_value() &&
                    pending_it->second != *requested_session_id.value()) {
                    return util::Status::NotFound("player is not in requested pending session");
                }
                auto session_it = pending_sessions_.find(pending_it->second);
                if (session_it != pending_sessions_.end()) {
                    pending_session = session_it->second.get();
                }
            }
        }
        if (pending_session == nullptr) {
            return util::Status::NotFound("player is not in a pending session");
        }

        auto status = route_pending_message(request, *pending_session);
        if (!status.ok()) {
            return status;
        }

        broadcast_joinable_sessions();
        return util::Status::Ok();
    }

    if (*message_type == "game.input") {
        std::shared_lock lock(mutex_);
        auto active_it = player_active_sessions_.find(player_id);
        if (active_it == player_active_sessions_.end()) {
            return util::Status::NotFound("player is not in an active session");
        }
        auto session_it = active_sessions_.find(active_it->second);
        if (session_it == active_sessions_.end()) {
            return util::Status::NotFound("active session not found");
        }
        return route_active_message(request, *session_it->second);
    }

    if (*message_type == "resume.ack") {
        std::shared_lock lock(mutex_);
        auto active_it = player_active_sessions_.find(player_id);
        if (active_it == player_active_sessions_.end()) {
            return util::Status::NotFound("player is not in an active session");
        }
        auto session_it = active_sessions_.find(active_it->second);
        if (session_it == active_sessions_.end()) {
            return util::Status::NotFound("active session not found");
        }
        return session_it->second->player_resumes(player_id);
    }

    return util::Status::InvalidArgument("unsupported game message type");
}

auto GameHub::list_joinable_sessions() const -> std::vector<PendingSessionSummary> {
    std::shared_lock lock(mutex_);
    std::vector<PendingSessionSummary> sessions;
    sessions.reserve(pending_sessions_.size());
    for (const auto& [session_id, session] : pending_sessions_) {
        (void)session_id;
        sessions.push_back(BuildPendingSummary(*session));
    }
    std::sort(sessions.begin(), sessions.end(), [](const PendingSessionSummary& left,
                                                   const PendingSessionSummary& right) {
        return left.session_id < right.session_id;
    });
    return sessions;
}

auto GameHub::find_pending_session(std::int64_t session_id) const
    -> util::StatusOr<const PendingSession*> {
    std::shared_lock lock(mutex_);
    auto it = pending_sessions_.find(session_id);
    if (it == pending_sessions_.end()) {
        return util::Status::NotFound("pending session not found");
    }
    return it->second.get();
}

auto GameHub::find_active_session(std::int64_t session_id) const
    -> util::StatusOr<const ActiveSession*> {
    std::shared_lock lock(mutex_);
    auto it = active_sessions_.find(session_id);
    if (it == active_sessions_.end()) {
        return util::Status::NotFound("active session not found");
    }
    return it->second.get();
}

auto GameHub::find_player_pending_session_id(std::int64_t player_id) const
    -> std::optional<std::int64_t> {
    std::shared_lock lock(mutex_);
    auto player_it = player_pending_sessions_.find(player_id);
    if (player_it == player_pending_sessions_.end()) {
        return std::nullopt;
    }

    if (!pending_sessions_.contains(player_it->second)) {
        return std::nullopt;
    }

    return player_it->second;
}

auto GameHub::find_player_active_session_id(std::int64_t player_id) const
    -> std::optional<std::int64_t> {
    std::shared_lock lock(mutex_);
    auto player_it = player_active_sessions_.find(player_id);
    if (player_it == player_active_sessions_.end()) {
        return std::nullopt;
    }

    if (!active_sessions_.contains(player_it->second)) {
        return std::nullopt;
    }

    return player_it->second;
}

void GameHub::register_anonymous_browser() {
    std::unique_lock lock(mutex_);
    browsing_players_.insert(0);
}

void GameHub::send_to_player(std::int64_t player_id, const Json::Value& message, int delay_ms) {
    if (transport_ == nullptr) {
        return;
    }
    transport_->send_to_player(player_id, message, delay_ms);
}

void GameHub::broadcast_to_players(const std::vector<std::int64_t>& player_ids,
                                   const Json::Value& message,
                                   int delay_ms) {
    for (const auto player_id : player_ids) {
        send_to_player(player_id, message, delay_ms);
    }
}

auto GameHub::route_pending_message(const RouteGameMessageRequest& request,
                                    PendingSession& session) -> util::Status {
    const auto message_type = FindMessageType(request.message);
    if (!message_type.has_value()) {
        return util::Status::InvalidArgument("message type is required");
    }
    if (*message_type != "queue.ready") {
        return util::Status::InvalidArgument("unsupported pending-session message type");
    }

    const Json::Value* payload = FindPayload(request.message);
    if (payload == nullptr) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    auto ready = ReadRequiredBool(*payload, "ready");
    if (!ready.ok()) {
        return ready.status();
    }

    const auto player_id = request.player.player_id();
    const auto status = session.player_ready(player_id, ready.value());
    if (!status.ok()) {
        return status;
    }

    const auto session_id = session.session_id();
    if (session.is_full() && session.all_ready()) {
        return start_active_session(session_id);
    }

    BroadcastPendingSnapshot(*this, session);

    return util::Status::Ok();
}

auto GameHub::route_active_message(const RouteGameMessageRequest& request,
                                   ActiveSession& session) -> util::Status {
    return session.handle_message(request.player.player_id(), request.message);
}

auto GameHub::start_active_session(std::int64_t session_id) -> util::Status {
    std::array<auth::PlayerProfilePtr, 4> players;
    GameConfig game_config;

    {
        std::unique_lock lock(mutex_);
        if (active_sessions_.contains(session_id)) {
            return util::Status::InvalidArgument("session is already active");
        }

        auto pending_it = pending_sessions_.find(session_id);
        if (pending_it == pending_sessions_.end()) {
            return util::Status::NotFound("pending session not found");
        }

        PendingSession* pending_session = pending_it->second.get();
        if (!pending_session->is_full() || !pending_session->all_ready()) {
            return util::Status::InvalidArgument("pending session is not ready to start");
        }

        game_config = pending_session->game_config();
        const auto& seats = pending_session->seats();
        for (std::size_t index = 0; index < players.size(); ++index) {
            players[index] = seats[index].player;
            if (players[index].lock() == nullptr) {
                return util::Status::InvalidArgument("pending session contains invalid player wrapper");
            }

            if (players[index].player_id() > 0) {
                player_pending_sessions_.erase(players[index].player_id());
                player_active_sessions_[players[index].player_id()] = session_id;
            }
        }

        active_sessions_.emplace(
            session_id,
            std::make_unique<ActiveSession>(
                seed_container_,
                this,
                session_id,
                players,
                game_config,
                pending_session->queue_config().public_session,
                record_manager_));
        if (auto it = active_sessions_.find(session_id); it != active_sessions_.end() && transport_ != nullptr) {
            it->second->set_session_end_callback(
                [transport = transport_](std::int64_t sid,
                    const std::array<std::int64_t, 4>& pids,
                    const std::array<int, 4>& scores,
                    int rounds) {
                    transport->on_session_ended(sid, pids, scores, rounds);
                });
        }
        pending_sessions_.erase(pending_it);
    }

    const Json::Value envelope = BuildResumeRequiredEnvelope(session_id);
    for (const auto& player : players) {
        if (player.player_id() > 0) {
            send_to_player(player.player_id(), envelope, game_config.network_delay_ms);
        }
    }

    broadcast_joinable_sessions();
    return util::Status::Ok();
}

void GameHub::garbage_collect_loop() {
    std::unique_lock lock(gc_mutex_);
    while (!gc_shutdown_) {
        gc_cv_.wait_for(lock, std::chrono::seconds(1), [this] {
            return gc_shutdown_;
        });
        if (gc_shutdown_) {
            return;
        }

        lock.unlock();
        garbage_collect_active_sessions();
        garbage_collect_pending_sessions();
        lock.lock();
    }
}

void GameHub::garbage_collect_active_sessions() {
    const auto now_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::vector<std::int64_t> expired_session_ids;
    std::vector<std::int64_t> removed_player_ids;
    bool sessions_changed = false;
    {
        std::unique_lock lock(mutex_);
        for (const auto& [session_id, session] : active_sessions_) {
            if (session == nullptr) {
                continue;
            }

            if (session->ended()) {
                active_session_all_afk_since_ms_.erase(session_id);
                if (now_ms - session->ended_at_ms() >= GameConfig::dead_time) {
                    expired_session_ids.push_back(session_id);
                    for (const auto& seat : session->seats()) {
                        const auto player = seat.player.lock();
                        if (player != nullptr && player->player_id > 0) {
                            removed_player_ids.push_back(player->player_id);
                        }
                    }
                }
                continue;
            }

            bool all_afk = true;
            for (const auto& seat : session->seats()) {
                if (!seat.is_afk() && !seat.disconnected) {
                    all_afk = false;
                    break;
                }
            }

            if (!all_afk) {
                active_session_all_afk_since_ms_.erase(session_id);
                continue;
            }

            auto& since_ms = active_session_all_afk_since_ms_[session_id];
            if (since_ms == 0) {
                since_ms = now_ms;
                continue;
            }

            if (now_ms - since_ms < GameConfig::afk_tolerance_ms) {
                continue;
            }

            session->end_session(now_ms);
            active_session_all_afk_since_ms_.erase(session_id);
            sessions_changed = true;
        }

        for (const auto session_id : expired_session_ids) {
            active_sessions_.erase(session_id);
            active_session_all_afk_since_ms_.erase(session_id);
        }
        for (auto it = player_active_sessions_.begin(); it != player_active_sessions_.end();) {
            if (std::find(expired_session_ids.begin(), expired_session_ids.end(), it->second) != expired_session_ids.end()) {
                it = player_active_sessions_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto player_id : removed_player_ids) {
            browsing_players_.insert(player_id);
        }
    }

    if (sessions_changed || !expired_session_ids.empty()) {
        broadcast_joinable_sessions();
    }
}

void GameHub::garbage_collect_pending_sessions() {
    std::vector<std::int64_t> removed_player_ids;
    std::vector<std::int64_t> changed_session_ids;
    std::vector<std::int64_t> expired_session_ids;

    {
        std::shared_lock lock(mutex_);
        for (const auto& [session_id, session] : pending_sessions_) {
            auto invalid_players = session->collect_invalid_players();
            if (!invalid_players.empty()) {
                removed_player_ids.insert(
                    removed_player_ids.end(), invalid_players.begin(), invalid_players.end());
                changed_session_ids.push_back(session_id);
            }

            if (session->is_empty()) {
                session->ensure_empty_timer();
                if (session->empty_timeout_elapsed()) {
                    expired_session_ids.push_back(session_id);
                }
                continue;
            }

            session->reset_empty_timer();
        }
    }

    if (!removed_player_ids.empty()) {
        std::unique_lock lock(mutex_);
        for (const auto player_id : removed_player_ids) {
            auto it = player_pending_sessions_.find(player_id);
            if (it != player_pending_sessions_.end()) {
                player_pending_sessions_.erase(it);
            }
        }
    }

    bool removed_any_session = false;
    for (const auto session_id : expired_session_ids) {
        std::unique_lock lock(mutex_);
        auto it = pending_sessions_.find(session_id);
        if (it == pending_sessions_.end()) {
            continue;
        }
        if (!it->second->is_empty() || !it->second->empty_timeout_elapsed()) {
            continue;
        }

        for (auto pending_it = player_pending_sessions_.begin(); pending_it != player_pending_sessions_.end();) {
            if (pending_it->second == session_id) {
                pending_it = player_pending_sessions_.erase(pending_it);
            } else {
                ++pending_it;
            }
        }
        pending_sessions_.erase(it);
        removed_any_session = true;
    }

    for (const auto session_id : changed_session_ids) {
        std::shared_lock lock(mutex_);
        auto it = pending_sessions_.find(session_id);
        if (it == pending_sessions_.end()) {
            continue;
        }
        PendingSession* session = it->second.get();
        lock.unlock();
        BroadcastPendingSnapshot(*this, *session);
    }

    if (!removed_player_ids.empty() || removed_any_session) {
        broadcast_joinable_sessions();
    }
}

void GameHub::broadcast_joinable_sessions() {
    const auto sessions = list_joinable_sessions();
    const auto active_sessions = list_active_sessions();
    Json::Value payload(Json::objectValue);
    payload["sessions"] = SerializePendingSummaryList(sessions);
    payload["active_sessions"] = SerializeActiveSummaryList(active_sessions);
    const Json::Value envelope = BuildEnvelope("lobby.list.snapshot", std::move(payload));

    std::vector<std::int64_t> browsing_players;
    {
        std::shared_lock lock(mutex_);
        browsing_players.reserve(browsing_players_.size());
        for (const auto player_id : browsing_players_) {
            browsing_players.push_back(player_id);
        }
    }

    broadcast_to_players(browsing_players, envelope);
}

}  // namespace mmcr::game