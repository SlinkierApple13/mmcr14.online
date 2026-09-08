#include "server/runtime_internal.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

#include <drogon/drogon.h>

namespace mmcr::server {

// ---------------------------------------------------------------------------
// GameSocketHub
// ---------------------------------------------------------------------------

GameSocketHub::GameSocketHub(DebugTrafficLogger* logger)
    : logger_(logger) {}

void GameSocketHub::AddConnection(const drogon::WebSocketConnectionPtr& connection,
               std::int64_t player_id,
               WebSocketRoute route) {
    std::lock_guard lock(mutex_);
    connections_.erase(
        std::remove_if(
            connections_.begin(),
            connections_.end(),
            [](const ConnectionEntry& entry) {
                return entry.connection.expired();
            }),
        connections_.end());
    connections_.push_back(ConnectionEntry{player_id, route, connection});
}

bool GameSocketHub::RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
                       std::int64_t player_id,
                       WebSocketRoute route) {
    std::lock_guard lock(mutex_);
    connections_.erase(
        std::remove_if(
            connections_.begin(),
            connections_.end(),
            [&](const ConnectionEntry& entry) {
                const auto live_connection = entry.connection.lock();
                return !live_connection || live_connection.get() == connection.get();
            }),
        connections_.end());

    return std::any_of(
        connections_.begin(),
        connections_.end(),
        [player_id, route](const ConnectionEntry& entry) {
            return entry.player_id == player_id && entry.route == route &&
                !entry.connection.expired();
        });
}

void GameSocketHub::SendToPlayer(std::int64_t player_id, const Json::Value& message, int delay_ms) {
    const auto route = ClassifyOutboundMessageRoute(message);
    if (!route.has_value()) {
        LogDroppedOutboundMessage(player_id, message);
        return;
    }

    for (const auto& connection : LiveConnectionsForPlayer(player_id, route)) {
        SendLoggedWebSocketJson(logger_, connection, message, delay_ms, player_id, route);
    }
}

void GameSocketHub::SendToSpectators(std::int64_t session_id,
                                 const Json::Value& message,
                                 int delay_ms) {
    for (const auto& connection : LiveConnectionsForPlayer(session_id, WebSocketRoute::kSpectate)) {
        SendLoggedWebSocketJson(
            logger_, connection, message, delay_ms, std::nullopt, WebSocketRoute::kSpectate);
    }
}

bool GameSocketHub::HasLiveConnection(std::int64_t player_id, WebSocketRoute route) {
    return !LiveConnectionsForPlayer(player_id, route).empty();
}

void GameSocketHub::EvictPlayerFromRoute(std::int64_t player_id, WebSocketRoute route,
              DebugTrafficLogger* logger,
              const drogon::WebSocketConnection* keep_connection) {
    std::vector<drogon::WebSocketConnectionPtr> to_close;
    {
        std::lock_guard lock(mutex_);
        connections_.erase(
            std::remove_if(
                connections_.begin(),
                connections_.end(),
                [&](const ConnectionEntry& entry) {
                    const auto live = entry.connection.lock();
                    if (!live) {
                        return true;
                    }
                    if (entry.player_id == player_id && entry.route == route &&
                        live.get() != keep_connection) {
                        to_close.push_back(live);
                        return true;
                    }
                    return false;
                }),
            connections_.end());
    }

    for (const auto& live : to_close) {
        SendLoggedWebSocketJson(
            logger, live,
            MakeWebSocketError(
                "kicked",
                "您已在别处登录", ""),
            0, player_id, route);
        if (live->connected()) {
            live->forceClose();
        }
    }
}

std::vector<drogon::WebSocketConnectionPtr> GameSocketHub::LiveConnectionsForPlayer(
    std::int64_t player_id,
    std::optional<WebSocketRoute> route) {
    std::lock_guard lock(mutex_);
    connections_.erase(
        std::remove_if(
            connections_.begin(),
            connections_.end(),
            [](const ConnectionEntry& entry) {
                return entry.connection.expired();
            }),
        connections_.end());

    std::vector<drogon::WebSocketConnectionPtr> live_connections;
    for (const auto& entry : connections_) {
        if (entry.player_id != player_id) {
            continue;
        }
        if (route.has_value() && entry.route != *route) {
            continue;
        }
        if (auto connection = entry.connection.lock()) {
            if (!connection->connected()) {
                continue;
            }
            live_connections.push_back(std::move(connection));
        }
    }
    return live_connections;
}

// ---------------------------------------------------------------------------
// DatabasePool
// ---------------------------------------------------------------------------

DatabasePool::Lease::~Lease() {
    Release();
}

DatabasePool::Lease::Lease(Lease&& other) noexcept {
    *this = std::move(other);
}

DatabasePool::Lease& DatabasePool::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    Release();
    pool_ = other.pool_;
    index_ = other.index_;
    database_ = other.database_;
    other.pool_ = nullptr;
    other.database_ = nullptr;
    other.index_ = 0;
    return *this;
}

storage::Database& DatabasePool::Lease::database() {
    return *database_;
}

DatabasePool::Lease::Lease(DatabasePool* pool, std::size_t index, storage::Database* database)
    : pool_(pool), index_(index), database_(database) {}

void DatabasePool::Lease::Release() {
    if (pool_ == nullptr) {
        return;
    }

    pool_->Release(index_);
    pool_ = nullptr;
    database_ = nullptr;
    index_ = 0;
}

DatabasePool::DatabasePool(std::filesystem::path database_path, std::size_t size)
    : database_path_(std::move(database_path)), size_(size == 0 ? 1 : size) {}

util::Status DatabasePool::Initialize() {
    entries_.clear();
    entries_.reserve(size_);

    for (std::size_t index = 0; index < size_; ++index) {
        Entry entry;
        auto status = entry.database.Open({database_path_, true, true});
        if (!status.ok()) {
            return status;
        }
        entries_.push_back(std::move(entry));
    }

    return util::Status::Ok();
}

util::StatusOr<DatabasePool::Lease> DatabasePool::Acquire() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&]() {
        return std::any_of(entries_.begin(), entries_.end(), [](const Entry& entry) {
            return !entry.in_use;
        });
    });

    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].in_use) {
            continue;
        }

        entries_[index].in_use = true;
        return Lease(this, index, &entries_[index].database);
    }

    return util::Status::Internal("database pool has no available connections");
}

std::size_t DatabasePool::size() const noexcept {
    return entries_.size();
}

void DatabasePool::Release(std::size_t index) {
    std::lock_guard lock(mutex_);
    entries_[index].in_use = false;
    condition_.notify_one();
}

// ---------------------------------------------------------------------------
// ServerState
// ---------------------------------------------------------------------------

ServerState::ServerState(RuntimeConfig config)
    : config_(std::move(config)),
      resolved_thread_count_(ResolveWorkerThreadCount(config_.thread_count)),
      database_pool_(config_.database_path, ResolveDatabasePoolSize(config_.thread_count)),
      record_manager_(config_.records_path),
      duplicate_manager_(&duplicate_database_),
      stats_service_(&stats_database_),
      rating_service_(&rating_database_),
      replay_manager_(config_.records_path, config_.imported_records_path),
      auth_config_(),
      socket_hub_(&traffic_logger_),
      game_hub_(&seed_container_, this, &record_manager_, &duplicate_manager_) {}

util::Status ServerState::Initialize() {
    if (config_.database_path.empty()) {
        return util::Status::InvalidArgument("database path must not be empty");
    }
    if (config_.records_path.empty()) {
        return util::Status::InvalidArgument("records path must not be empty");
    }
    if (!std::filesystem::exists(config_.records_path)) {
        return util::Status::InvalidArgument("records path must exist");
    }
    if (!std::filesystem::is_directory(config_.records_path)) {
        return util::Status::InvalidArgument("records path must be a directory");
    }

    const auto parent = config_.database_path.parent_path();
    if (!parent.empty()) {
        std::error_code error_code;
        std::filesystem::create_directories(parent, error_code);
        if (error_code) {
            return util::Status::Internal(
                "failed to create database directory: " + error_code.message());
        }
    }

    storage::Database migration_database;
    auto status = migration_database.Open({config_.database_path, true, true});
    if (!status.ok()) {
        return status;
    }

    auto auth_service = MakeAuthService(&migration_database);
    status = auth_service.InitializeSchema(AuthMigrationsPath());
    if (!status.ok()) {
        return status;
    }

    migration_database.Close();

    status = stats_database_.Open({config_.database_path, true, true});
    if (!status.ok()) {
        return status;
    }
    status = stats_service_.InitializeSchema(StatsMigrationsPath());
    if (!status.ok()) {
        return status;
    }
    status = stats_service_.LoadFromDatabase();
    if (!status.ok()) {
        return status;
    }

    status = duplicate_database_.Open({config_.database_path, true, true});
    if (!status.ok()) {
        return status;
    }
    status = duplicate_manager_.InitializeSchema(DuplicateMigrationsPath());
    if (!status.ok()) {
        return status;
    }

    // Records released from a destroyed seed list flow straight into stats.
    duplicate_manager_.SetReleaseCallback([this](std::string payload_json) {
        auto parsed = ParseJsonText(payload_json);
        if (!parsed.has_value()) {
            return;
        }
        (void)stats_service_.UpsertRoundRecord(*parsed);
    });
    stats_service_.SetDuplicateActivityCheck(
        [this](std::string_view token) -> bool {
            const auto active = duplicate_manager_.IsTokenActive(
                token, CurrentUnixTimeMs());
            return active.ok() && active.value();
        });

    record_manager_.SetWriteObserver([this](const storage::GameRecordTask& task) {
        const Json::Value& header = task.payload["header"];
        const Json::Value& game_config = header["game_config"];
        if (game_config.isObject() && game_config["duplicate_mode"].isBool() &&
            game_config["duplicate_mode"].asBool()) {
            const std::string token = header["duplicate_token"].isString()
                ? header["duplicate_token"].asString()
                : std::string();
            if (token.empty()) {
                return stats_service_.UpsertRoundRecord(task.payload);
            }
            const std::string session_identifier = header["session_identifier"].isString()
                ? header["session_identifier"].asString()
                : std::string();
            const std::int64_t round_number = header["round_number"].isUInt64()
                ? static_cast<std::int64_t>(header["round_number"].asUInt64())
                : 0;
            auto staged = duplicate_manager_.StageRecord(
                token,
                session_identifier,
                round_number,
                JsonToCompactString(task.payload),
                CurrentUnixTimeMs());
            if (!staged.ok()) {
                return staged.status();
            }
            if (staged.value()) {
                return util::Status::Ok();
            }
            // Seed list already destroyed — release directly into stats.
            return stats_service_.UpsertRoundRecord(task.payload);
        }
        return stats_service_.UpsertRoundRecord(task.payload);
    });

    // Sessions do not survive a restart: expired seed lists can be removed
    // outright (releasing their staged records into stats).
    status = duplicate_manager_.OnServerStartup(CurrentUnixTimeMs());
    if (!status.ok()) {
        return status;
    }

    status = rating_database_.Open({config_.database_path, true, true});
    if (!status.ok()) {
        return status;
    }
    status = rating_service_.InitializeSchema(RatingMigrationsPath());

    status = database_pool_.Initialize();
    if (!status.ok()) {
        return status;
    }

    status = replay_manager_.Initialize();
    if (!status.ok()) {
        return status;
    }

    if (config_.debug_log_dir.empty()) {
        return util::Status::Ok();
    }

    return traffic_logger_.Initialize(config_.debug_log_dir);
}

util::StatusOr<auth::RegisterResult> ServerState::Register(
    const auth::RegisterRequest& request) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.Register(request);
}

util::StatusOr<auth::AuthenticatedSession> ServerState::Login(
    const auth::LoginRequest& request) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.Login(request);
}

util::StatusOr<auth::AuthenticatedSession> ServerState::Authenticate(
    std::string_view session_token, std::int64_t now_ms) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.Authenticate(session_token, now_ms);
}

util::StatusOr<auth::AuthenticatedSession> ServerState::RefreshSession(
    std::string_view session_token, std::int64_t now_ms) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.RefreshSession(session_token, now_ms);
}

util::Status ServerState::Logout(std::string_view session_token, std::int64_t now_ms) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.Logout(session_token, now_ms);
}

util::Status ServerState::ChangePassword(const auth::ChangePasswordRequest& request) {
    auto lease = database_pool_.Acquire();
    if (!lease.ok()) {
        return lease.status();
    }

    auto auth_service = MakeAuthService(&lease.value().database());
    return auth_service.ChangePassword(request);
}

util::StatusOr<game::CreateGameSessionResult> ServerState::CreateSession(
    const game::CreateGameSessionRequest& request) {
    return game_hub_.create_session(request);
}

util::Status ServerState::ConnectPlayer(const game::ConnectPlayerRequest& request) {
    return game_hub_.connect_player(request);
}

util::Status ServerState::DisconnectPlayer(const game::DisconnectPlayerRequest& request) {
    return game_hub_.disconnect_player(request);
}

util::Status ServerState::HandleGameMessage(const game::RouteGameMessageRequest& request) {
    return game_hub_.handle_message(request);
}

util::StatusOr<const game::PendingSession*> ServerState::FindPendingSession(
    std::int64_t session_id) const {
    return game_hub_.find_pending_session(session_id);
}

util::StatusOr<const game::ActiveSession*> ServerState::FindActiveSession(
    std::int64_t session_id) const {
    return game_hub_.find_active_session(session_id);
}

std::vector<game::PendingSessionSummary> ServerState::ListVisibleJoinableSessions(
    std::optional<std::int64_t> viewer_player_id) const {
    const auto sessions = game_hub_.list_joinable_sessions();
    if (!viewer_player_id.has_value()) {
        std::vector<game::PendingSessionSummary> visible_sessions;
        visible_sessions.reserve(sessions.size());
        for (const auto& session : sessions) {
            if (session.public_session) {
                visible_sessions.push_back(session);
            }
        }
        return visible_sessions;
    }

    std::vector<game::PendingSessionSummary> visible_sessions;
    visible_sessions.reserve(sessions.size());
    for (const auto& session : sessions) {
        if (session.public_session) {
            visible_sessions.push_back(session);
            continue;
        }

        auto pending_session = game_hub_.find_pending_session(session.session_id);
        if (!pending_session.ok()) {
            continue;
        }
        if (PendingSessionContainsPlayer(*pending_session.value(), *viewer_player_id)) {
            visible_sessions.push_back(session);
        }
    }
    return visible_sessions;
}

std::vector<game::ActiveSessionSummary> ServerState::ListVisibleActiveSessions(
    std::optional<std::int64_t> viewer_player_id) const {
    (void)viewer_player_id;
    return game_hub_.list_active_sessions();
}

std::optional<std::int64_t> ServerState::FindPlayerActiveSessionId(
    std::int64_t player_id) const {
    return game_hub_.find_player_active_session_id(player_id);
}

std::optional<std::int64_t> ServerState::FindPlayerPendingSessionId(
    std::int64_t player_id) const {
    return game_hub_.find_player_pending_session_id(player_id);
}

void ServerState::AddConnection(const drogon::WebSocketConnectionPtr& connection,
               const std::shared_ptr<auth::PlayerProfile>& player,
               WebSocketRoute route) {
    if (player == nullptr) {
        if (route == WebSocketRoute::kLobby) {
            socket_hub_.AddConnection(connection, 0, route);
        }
        return;
    }
    socket_hub_.AddConnection(connection, player->player_id, route);
}

void ServerState::RegisterAnonymousLobbyBrowser() {
    game_hub_.register_anonymous_browser();
}

bool ServerState::RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
                       std::int64_t player_id,
                       WebSocketRoute route) {
    return socket_hub_.RemoveConnection(connection, player_id, route);
}

std::size_t ServerState::resolved_thread_count() const noexcept {
    return resolved_thread_count_;
}

std::size_t ServerState::database_pool_size() const noexcept {
    return database_pool_.size();
}

const std::filesystem::path& ServerState::records_path() const {
    return config_.records_path;
}

util::StatusOr<std::vector<replay::ReplayInfo>> ServerState::ListReplays() const {
    return replay_manager_.ListSessions();
}

util::StatusOr<Json::Value> ServerState::LoadReplayRound(
    std::string_view session_identifier,
    std::uint64_t round_number) const {
    return replay_manager_.LoadRoundRecord(session_identifier, round_number);
}

util::StatusOr<std::vector<Json::Value>> ServerState::LoadReplaySession(
    std::string_view session_identifier) const {
    return replay_manager_.LoadSessionRecords(session_identifier);
}

void ServerState::LogHttpRequest(const drogon::HttpRequestPtr& request) {
    traffic_logger_.Log(BuildHttpRequestLogEntry(request));
}

void ServerState::LogHttpResponse(const drogon::HttpRequestPtr& request,
             const drogon::HttpResponsePtr& response) {
    traffic_logger_.Log(BuildHttpResponseLogEntry(request, response));
}

void ServerState::LogWebSocketInbound(const drogon::WebSocketConnectionPtr& connection,
                std::string_view message,
                const drogon::WebSocketMessageType& type) {
    LogInboundWebSocketMessage(&traffic_logger_, connection, message, type);
}

void ServerState::SendWebSocketJson(const drogon::WebSocketConnectionPtr& connection,
              const Json::Value& message,
              int delay_ms,
              std::optional<std::int64_t> player_id,
              std::optional<WebSocketRoute> route) {
    SendLoggedWebSocketJson(&traffic_logger_, connection, message, delay_ms, player_id, route);
}

void ServerState::send_to_player(std::int64_t player_id,
           const Json::Value& message,
           int delay_ms) {
    socket_hub_.SendToPlayer(player_id, message, delay_ms);
}

void ServerState::send_to_spectators(std::int64_t session_id,
                                 const Json::Value& message,
                                 int delay_ms) {
    socket_hub_.SendToSpectators(session_id, message, delay_ms);
}

void ServerState::on_session_ended(std::int64_t session_id,
                  const std::array<std::int64_t, 4>& player_ids,
                  const std::array<int, 4>& final_scores,
                  int round_count) {
    (void)session_id;
    if (round_count <= 0) return;

    const auto now_ts = CurrentUnixTimeMs() / 1000;

    // Snapshot initial ratings BEFORE update
    Json::Value initial_ratings(Json::arrayValue);
    for (int i = 0; i < 4; ++i) {
        Json::Value entry(Json::objectValue);
        entry["player_id"] = Json::Int64(player_ids[i]);
        if (player_ids[i] > 0) {
            auto rating = rating_service_.GetRating(player_ids[i], now_ts);
            if (rating.ok()) {
                entry["mu"] = rating.value().mu;
                entry["tau"] = rating.value().tau;
                entry["sigma"] = rating.value().sigma;
                entry["points"] = rating.value().points;
                entry["level"] = rating.value().level;
            }
        }
        initial_ratings.append(std::move(entry));
    }

    std::array<ranking::SessionScore, 4> scores{};
    for (int i = 0; i < 4; ++i) {
        scores[i] = {player_ids[i], static_cast<double>(final_scores[i])};
    }

    auto result = rating_service_.UpdateAfterSession(scores, round_count, now_ts);
    if (!result.ok()) return;

    // Snapshot final ratings AFTER update
    Json::Value final_ratings(Json::arrayValue);
    for (int i = 0; i < 4; ++i) {
        Json::Value entry(Json::objectValue);
        entry["player_id"] = Json::Int64(player_ids[i]);
        if (player_ids[i] > 0) {
            auto rating = rating_service_.GetRating(player_ids[i], now_ts);
            if (rating.ok()) {
                entry["mu"] = rating.value().mu;
                entry["tau"] = rating.value().tau;
                entry["sigma"] = rating.value().sigma;
                entry["points"] = rating.value().points;
                entry["level"] = rating.value().level;
            }
        }
        final_ratings.append(std::move(entry));
    }

    Json::Value envelope(Json::objectValue);
    envelope["version"] = 1;
    envelope["type"] = "rating.update";
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(session_id);
    payload["initial"] = std::move(initial_ratings);
    payload["final"] = std::move(final_ratings);
    Json::Value deltas(Json::arrayValue);
    for (int i = 0; i < 4; ++i) {
        Json::Value entry(Json::objectValue);
        entry["player_id"] = Json::Int64(player_ids[i]);
        entry["delta_mu"] = result.value().delta_mu[i];
        entry["delta_points"] = result.value().delta_points[i];
        deltas.append(std::move(entry));
    }
    payload["deltas"] = std::move(deltas);
    envelope["payload"] = std::move(payload);

    for (int i = 0; i < 4; ++i) {
        send_to_player(player_ids[i], envelope);
    }
}

std::vector<game::PlayerRatingSnapshot> ServerState::get_player_ratings(
    const std::array<std::int64_t, 4>& player_ids) {
    const auto now_ts = CurrentUnixTimeMs() / 1000;
    std::vector<game::PlayerRatingSnapshot> result;
    result.reserve(4);
    for (int i = 0; i < 4; ++i) {
        game::PlayerRatingSnapshot entry;
        entry.player_id = player_ids[i];
        if (player_ids[i] > 0) {
            auto rating = rating_service_.GetRating(player_ids[i], now_ts);
            if (rating.ok()) {
                entry.mu = rating.value().mu;
                entry.tau = rating.value().tau;
                entry.sigma = rating.value().sigma;
                entry.points = rating.value().points;
                entry.level = rating.value().level;
                entry.total_games = rating.value().total_games;
            }
        }
        result.push_back(std::move(entry));
    }
    return result;
}

stats::StatsService& ServerState::stats() {
    return stats_service_;
}

const stats::StatsService& ServerState::stats() const {
    return stats_service_;
}

duplicate::DuplicateManager& ServerState::duplicate_manager() {
    return duplicate_manager_;
}

const duplicate::DuplicateManager& ServerState::duplicate_manager() const {
    return duplicate_manager_;
}

GameSocketHub& ServerState::socket_hub() {
    return socket_hub_;
}

DebugTrafficLogger* ServerState::traffic_logger() {
    return &traffic_logger_;
}

ranking::RatingService& ServerState::rating() {
    return rating_service_;
}

auth::AuthService ServerState::MakeAuthService(storage::Database* database) const {
    return auth::AuthService(database, auth_config_, const_cast<random::SeedContainer*>(&seed_container_));
}

}  // namespace mmcr::server
