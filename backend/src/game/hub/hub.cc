#include "game/hub/hub_internal.h"

#include <algorithm>
#include <string>
#include <utility>

#include "game/engine/session.h"

namespace mmcr::game {

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

// ---------------------------------------------------------------------------
// Session id allocation
// ---------------------------------------------------------------------------

util::StatusOr<std::int64_t> GameHub::allocate_session_id_locked() {
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

util::StatusOr<std::int64_t> GameHub::allocate_unranked_session_id_locked() {
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

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

util::StatusOr<CreateGameSessionResult> GameHub::create_session(
    const CreateGameSessionRequest& request) {
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

            // Singleplayer bypasses the pending room, so inject the team
            // assignment here (the owner on 虎, virtual players on 龙) for
            // team modes such as 过五关.
            if (game_config.mode == GameMode::kPassFiveGates) {
                if (!game_config.pass_five_gates.has_value()) {
                    game_config.pass_five_gates = PassFiveGatesConfig{};
                }
                for (std::size_t index = 0; index < players.size(); ++index) {
                    if (players[index].valid()) {
                        game_config.pass_five_gates->team[players[index].player_id()] =
                            (index == 0) ? 0 : 1;
                    }
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
            pending_it->second->ensure_empty_timer();
        }
        browsing_players_.erase(request.owner.player_id);
    }

    broadcast_joinable_sessions();
    return CreateGameSessionResult{session_id};
}

std::vector<ActiveSessionSummary> GameHub::list_active_sessions() const {
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

// ---------------------------------------------------------------------------
// Player connection management
// ---------------------------------------------------------------------------

util::Status GameHub::connect_player(const ConnectPlayerRequest& request) {
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

util::Status GameHub::disconnect_player(const DisconnectPlayerRequest& request) {
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

// ---------------------------------------------------------------------------
// Message routing
// ---------------------------------------------------------------------------

util::Status GameHub::handle_message(const RouteGameMessageRequest& request) {
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

    if (*message_type == "queue.ready" || *message_type == "queue.team") {
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

// ---------------------------------------------------------------------------
// Lookups and delivery
// ---------------------------------------------------------------------------

std::vector<PendingSessionSummary> GameHub::list_joinable_sessions() const {
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

util::StatusOr<const PendingSession*> GameHub::find_pending_session(std::int64_t session_id) const {
    std::shared_lock lock(mutex_);
    auto it = pending_sessions_.find(session_id);
    if (it == pending_sessions_.end()) {
        return util::Status::NotFound("pending session not found");
    }
    return it->second.get();
}

util::StatusOr<const ActiveSession*> GameHub::find_active_session(std::int64_t session_id) const {
    std::shared_lock lock(mutex_);
    auto it = active_sessions_.find(session_id);
    if (it == active_sessions_.end()) {
        return util::Status::NotFound("active session not found");
    }
    return it->second.get();
}

std::optional<std::int64_t> GameHub::find_player_pending_session_id(
    std::int64_t player_id) const {
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

std::optional<std::int64_t> GameHub::find_player_active_session_id(
    std::int64_t player_id) const {
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

void GameHub::send_to_spectators(std::int64_t session_id,
                                 const Json::Value& message,
                                 int delay_ms) {
    if (transport_ == nullptr) {
        return;
    }
    transport_->send_to_spectators(session_id, message, delay_ms);
}

void GameHub::broadcast_to_players(const std::vector<std::int64_t>& player_ids,
                                   const Json::Value& message,
                                   int delay_ms) {
    for (const auto player_id : player_ids) {
        send_to_player(player_id, message, delay_ms);
    }
}

// ---------------------------------------------------------------------------
// Session message routing and activation
// ---------------------------------------------------------------------------

util::Status GameHub::route_pending_message(const RouteGameMessageRequest& request,
                                    PendingSession& session) {
    const auto message_type = FindMessageType(request.message);
    if (!message_type.has_value()) {
        return util::Status::InvalidArgument("message type is required");
    }

    const Json::Value* payload = FindPayload(request.message);
    if (payload == nullptr) {
        return util::Status::InvalidArgument("payload must be a JSON object");
    }

    const auto player_id = request.player.player_id();

    if (*message_type == "queue.team") {
        auto team = ReadRequiredInt64(*payload, "team");
        if (!team.ok()) {
            return team.status();
        }
        if (team.value() < 0 || team.value() > 1) {
            return util::Status::InvalidArgument("team must be 0 (虎) or 1 (龙)");
        }
        const auto status = session.player_set_team(player_id, static_cast<int>(team.value()));
        if (!status.ok()) {
            return status;
        }
        BroadcastPendingSnapshot(*this, session);
        return util::Status::Ok();
    }

    if (*message_type != "queue.ready") {
        return util::Status::InvalidArgument("unsupported pending-session message type");
    }

    auto ready = ReadRequiredBool(*payload, "ready");
    if (!ready.ok()) {
        return ready.status();
    }

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

util::Status GameHub::route_active_message(const RouteGameMessageRequest& request,
                                   ActiveSession& session) {
    return session.handle_message(request.player.player_id(), request.message);
}

util::Status GameHub::start_active_session(std::int64_t session_id) {
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
        // Inject team assignments (keyed by player id) into the typed
        // pass-five-gates config. Server-side only; clients can never write
        // the team map (ParsePassFiveGatesConfig rejects the key).
        if (game_config.mode == GameMode::kPassFiveGates) {
            if (!game_config.pass_five_gates.has_value()) {
                game_config.pass_five_gates = PassFiveGatesConfig{};
            }
            for (const auto& seat : seats) {
                if (!seat.player.valid() || seat.team < 0) {
                    continue;
                }
                game_config.pass_five_gates->team[seat.player.player_id()] = seat.team;
            }
        }
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

}  // namespace mmcr::game
