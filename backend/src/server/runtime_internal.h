#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/WebSocketConnection.h>
#include <jsoncpp/json/json.h>

#include "auth/service.h"
#include "game/engine/session.h"
#include "game/hub/hub.h"
#include "game/mode/mode_registry.h"
#include "random/seed.h"
#include "ranking/service.h"
#include "replay/manager.h"
#include "stats/service.h"
#include "storage/database.h"
#include "storage/game_record.h"
#include "server/runtime.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::server {

// ---------------------------------------------------------------------------
// Configuration constants
// ---------------------------------------------------------------------------

extern const std::string_view kDefaultBindAddress;
extern const std::string_view kDefaultDatabasePath;
extern const std::string_view kDefaultDebugLogDir;
extern const std::string_view kDefaultCorsAllowHeaders;
extern const std::string_view kDefaultCorsAllowMethods;

constexpr std::uint16_t kDefaultPort = 8080;
constexpr std::size_t kDefaultThreadCount = 0;
constexpr double kLargeHandCutoff = 18.495;
constexpr std::size_t kBigWinsMaxCount = 10;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::size_t ResolveWorkerThreadCount(std::size_t configured_thread_count);
[[nodiscard]] std::size_t ResolveDatabasePoolSize(std::size_t configured_thread_count);
[[nodiscard]] std::int64_t CurrentUnixTimeMs();
[[nodiscard]] std::string JsonToCompactString(const Json::Value& value);
[[nodiscard]] std::string PointerHandleString(const void* pointer);
[[nodiscard]] bool IsSensitiveFieldName(std::string_view name);
[[nodiscard]] Json::Value RedactSensitiveJson(const Json::Value& value);
[[nodiscard]] std::optional<Json::Value> ParseJsonText(std::string_view raw_text);
[[nodiscard]] Json::Value SanitizeTextPayload(std::string_view raw_text);
[[nodiscard]] std::string SanitizeQueryString(std::string_view raw_query);

[[nodiscard]] Json::Value BuildHttpRequestLogEntry(const drogon::HttpRequestPtr& request);
[[nodiscard]] Json::Value BuildHttpResponseLogEntry(const drogon::HttpRequestPtr& request,
                                            const drogon::HttpResponsePtr& response);

[[nodiscard]] std::string_view TrimString(std::string_view value);
[[nodiscard]] bool EqualsCaseInsensitive(std::string_view left, std::string_view right);
[[nodiscard]] util::StatusOr<std::uint64_t> ParseUnsignedInteger(std::string_view raw_value,
                                                           std::string_view variable_name);
[[nodiscard]] util::StatusOr<std::int64_t> ParseInt64String(std::string_view raw_value,
                                                   std::string_view label);

[[nodiscard]] std::optional<std::string> ResolveCorsAllowOrigin();
void ApplyCorsHeaders(const drogon::HttpResponsePtr& response);

[[nodiscard]] std::string_view StatusCodeName(util::StatusCode code);
[[nodiscard]] drogon::HttpStatusCode ToHttpStatus(util::StatusCode code);
[[nodiscard]] drogon::HttpResponsePtr NewJsonResponse(Json::Value payload,
                                              drogon::HttpStatusCode status_code = drogon::k200OK);
[[nodiscard]] drogon::HttpResponsePtr NewErrorResponse(drogon::HttpStatusCode status_code,
                                               std::string_view code,
                                               std::string message);
[[nodiscard]] drogon::HttpResponsePtr NewStatusErrorResponse(const util::Status& status);
[[nodiscard]] drogon::HttpResponsePtr NewUnauthorizedResponse(std::string_view message);
[[nodiscard]] drogon::HttpResponsePtr NewNoContentResponse();
[[nodiscard]] drogon::HttpResponsePtr NewOkResponse();
[[nodiscard]] drogon::HttpResponsePtr NewTextResponse(std::string payload,
                                              drogon::HttpStatusCode status_code = drogon::k200OK);

[[nodiscard]] std::string EvaluateCalculatorExpression(std::string_view expression);

[[nodiscard]] const Json::Value* FindField(const Json::Value& object,
                                    std::initializer_list<std::string_view> names);
[[nodiscard]] util::StatusOr<std::shared_ptr<Json::Value>> ParseJsonBody(
    const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<std::string> ReadRequiredString(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label);
[[nodiscard]] util::StatusOr<bool> ReadRequiredBool(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label);
[[nodiscard]] util::StatusOr<bool> ReadOptionalBool(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label,
    bool default_value);
[[nodiscard]] util::StatusOr<int> ReadOptionalInt(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label,
    int default_value);

[[nodiscard]] util::StatusOr<std::string> ExtractBearerToken(
    const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<std::string> ExtractWebSocketToken(
    const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<std::optional<std::int64_t>> ExtractWebSocketSessionId(
    const drogon::HttpRequestPtr& request);

[[nodiscard]] Json::Value SerializePlayer(const auth::PlayerProfile& player);
[[nodiscard]] Json::Value SerializeSession(const auth::SessionInfo& session);
[[nodiscard]] Json::Value SerializeAuthenticatedSession(
    const auth::AuthenticatedSession& session);
[[nodiscard]] Json::Value SerializeActiveSummary(
    const game::ActiveSessionSummary& summary);
[[nodiscard]] Json::Value SerializeActiveSummaryList(
    const std::vector<game::ActiveSessionSummary>& sessions);
[[nodiscard]] Json::Value SerializeReplayInfo(const replay::ReplayInfo& replay_info);
[[nodiscard]] Json::Value SerializeReplayInfoList(
    const std::vector<replay::ReplayInfo>& sessions);

[[nodiscard]] std::string FormatHexSeed(std::uint64_t value);
void NormalizeReplaySeedFields(Json::Value& round_record);

// ---------------------------------------------------------------------------
// Game/queue config parsing
// ---------------------------------------------------------------------------

[[nodiscard]] util::Status RejectImmutableGameConfigField(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label);
[[nodiscard]] util::Status ValidateGameConfigBounds(const game::GameConfig& config);
[[nodiscard]] util::StatusOr<game::GameConfig> ParseGameConfig(const Json::Value& object);
[[nodiscard]] util::StatusOr<game::QueueConfig> ParseQueueConfig(const Json::Value& object);

[[nodiscard]] game::PendingSessionSummary BuildPendingSummary(
    const game::PendingSession& session);
[[nodiscard]] Json::Value SerializePendingSummary(
    const game::PendingSessionSummary& summary);
[[nodiscard]] Json::Value SerializePendingSummaryList(
    const std::vector<game::PendingSessionSummary>& sessions);
[[nodiscard]] Json::Value SerializePendingSeat(const game::PendingSeat& seat);
[[nodiscard]] Json::Value SerializePendingSnapshot(
    const game::PendingSession& session,
    const Json::Value& ratings = Json::Value(Json::nullValue));

[[nodiscard]] bool PendingSessionContainsPlayer(const game::PendingSession& session,
                                        std::int64_t player_id);
[[nodiscard]] bool ActiveSessionContainsPlayer(const game::ActiveSession& session,
                                       std::int64_t player_id);
[[nodiscard]] bool CanViewPendingSession(const game::PendingSession& session,
                                 std::optional<std::int64_t> viewer_player_id);
[[nodiscard]] bool CanViewActiveSession(const game::ActiveSession& session,
                                std::optional<std::int64_t> viewer_player_id);

// ---------------------------------------------------------------------------
// WebSocket message helpers
// ---------------------------------------------------------------------------

[[nodiscard]] Json::Value BuildGameMessage(std::string_view type, Json::Value payload);
[[nodiscard]] Json::Value MakeWebSocketEnvelope(std::string_view type,
                                        Json::Value payload,
                                        std::string_view request_id = {});
[[nodiscard]] Json::Value MakeWebSocketAck(std::string_view request_id);
[[nodiscard]] Json::Value MakeWebSocketError(std::string_view code,
                                     std::string message,
                                     std::string_view request_id);
[[nodiscard]] util::StatusOr<Json::Value> ParseWebSocketMessage(std::string_view message);

[[nodiscard]] std::filesystem::path AuthMigrationsPath();
[[nodiscard]] std::filesystem::path StatsMigrationsPath();
[[nodiscard]] std::filesystem::path RatingMigrationsPath();

enum class WebSocketRoute {
    kLobby,
    kGame,
    kSpectate,
    kReplay,
};

[[nodiscard]] std::string_view WebSocketRoutePath(WebSocketRoute route);
[[nodiscard]] WebSocketRoute ResolveWebSocketRoute(const drogon::HttpRequestPtr& request);
[[nodiscard]] std::optional<std::string> FindMessageTypeString(const Json::Value& message);
[[nodiscard]] std::optional<std::string> FindPayloadPhaseString(const Json::Value& message);
[[nodiscard]] std::optional<WebSocketRoute> ClassifyInboundMessageRoute(std::string_view type);
[[nodiscard]] std::optional<WebSocketRoute> ClassifyOutboundMessageRoute(
    const Json::Value& message);
void LogDroppedOutboundMessage(std::int64_t player_id, const Json::Value& message);

// ---------------------------------------------------------------------------
// Stats payload helpers
// ---------------------------------------------------------------------------

[[nodiscard]] stats::StatsFilter ParseStatsFilterFromJson(const Json::Value& json);
[[nodiscard]] std::string StatsRecordSortKey(std::string_view sort_field,
                                     std::string_view sort_order);

struct StatsRecordsRequest {
    std::string sort_field{"time"};
    std::string sort_order{"desc"};
    std::size_t offset{0};
    std::size_t limit{16};
};

[[nodiscard]] StatsRecordsRequest ParseStatsRecordsRequest(const Json::Value& root);
[[nodiscard]] std::vector<const stats::RoundEntry*> BuildFanSortedStatsRounds(
    const std::vector<const stats::RoundEntry*>& time_desc_rounds,
    bool descending);
[[nodiscard]] std::vector<const stats::RoundEntry*> BuildStatsRecordOrder(
    const std::vector<const stats::RoundEntry*>& time_desc_rounds,
    std::string_view sort_field,
    std::string_view sort_order);
[[nodiscard]] std::string SerializeStatsRoundEntriesPayload(
    const std::vector<const stats::RoundEntry*>& entries_to_emit,
    std::size_t total,
    std::size_t offset,
    std::size_t limit);
[[nodiscard]] std::string SerializeStatsRoundEntriesPage(
    const std::vector<const stats::RoundEntry*>& sorted,
    std::size_t offset,
    std::size_t limit);

// ---------------------------------------------------------------------------
// Traffic logging
// ---------------------------------------------------------------------------

class DebugTrafficLogger {
public:
    [[nodiscard]] util::Status Initialize(const std::filesystem::path& dir);
    void Log(Json::Value entry);
    [[nodiscard]] const std::filesystem::path& dir() const;

private:
    static std::string TodayUtcDate();
    [[nodiscard]] util::Status RotateFile();

    std::filesystem::path dir_;
    std::string current_log_date_;
    std::mutex mutex_;
    std::ofstream stream_;
};

class GameClientContext {
public:
    explicit GameClientContext(std::shared_ptr<auth::PlayerProfile> player,
                             WebSocketRoute route,
                             std::optional<std::int64_t> spectator_session_id = std::nullopt)
        : player_(std::move(player)),
          route_(route),
          spectator_session_id_(spectator_session_id) {}

    [[nodiscard]] const std::shared_ptr<auth::PlayerProfile>& player() const {
        return player_;
    }

    [[nodiscard]] std::int64_t player_id() const noexcept {
        return player_ != nullptr ? player_->player_id : 0;
    }

    [[nodiscard]] auth::PlayerProfilePtr player_handle() const {
        return auth::PlayerProfilePtr(player_);
    }

    [[nodiscard]] WebSocketRoute route() const noexcept {
        return route_;
    }

    [[nodiscard]] std::int64_t connection_key() const noexcept {
        return spectator_session_id_.value_or(player_id());
    }

    [[nodiscard]] std::optional<std::int64_t> spectator_session_id() const noexcept {
        return spectator_session_id_;
    }

    [[nodiscard]] std::int64_t last_hand_request_ms() const noexcept {
        return last_hand_request_ms_.load();
    }

    void mark_hand_request(std::int64_t now_ms) noexcept {
        last_hand_request_ms_.store(now_ms);
    }

    void mark_registered_with_hub() noexcept {
        registered_with_hub_ = true;
    }

    [[nodiscard]] bool registered_with_hub() const noexcept {
        return registered_with_hub_;
    }

private:
    std::shared_ptr<auth::PlayerProfile> player_;
    WebSocketRoute route_{WebSocketRoute::kLobby};
    std::optional<std::int64_t> spectator_session_id_;
    std::atomic<std::int64_t> last_hand_request_ms_{0};
    bool registered_with_hub_{false};
};

[[nodiscard]] std::optional<std::int64_t> ResolveLoggedPlayerId(
    const drogon::WebSocketConnectionPtr& connection,
    std::optional<std::int64_t> player_id_override);
[[nodiscard]] std::optional<WebSocketRoute> ResolveLoggedRoute(
    const drogon::WebSocketConnectionPtr& connection,
    std::optional<WebSocketRoute> route_override);
void LogInboundWebSocketMessage(DebugTrafficLogger* logger,
                        const drogon::WebSocketConnectionPtr& connection,
                        std::string_view message,
                        const drogon::WebSocketMessageType& type);
void LogOutboundWebSocketMessage(DebugTrafficLogger* logger,
                     const drogon::WebSocketConnectionPtr& connection,
                     const Json::Value& message,
                     int delay_ms,
                     std::optional<std::int64_t> player_id,
                     std::optional<WebSocketRoute> route);
void SendLoggedWebSocketJson(DebugTrafficLogger* logger,
                     const drogon::WebSocketConnectionPtr& connection,
                     const Json::Value& message,
                     int delay_ms = 0,
                     std::optional<std::int64_t> player_id = std::nullopt,
                     std::optional<WebSocketRoute> route = std::nullopt);

// ---------------------------------------------------------------------------
// Connection hub and server state
// ---------------------------------------------------------------------------

class GameSocketHub {
public:
    explicit GameSocketHub(DebugTrafficLogger* logger = nullptr);

    void AddConnection(const drogon::WebSocketConnectionPtr& connection,
                       std::int64_t player_id,
                       WebSocketRoute route);
    [[nodiscard]] bool RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
                                        std::int64_t player_id,
                                        WebSocketRoute route);
    void SendToPlayer(std::int64_t player_id, const Json::Value& message, int delay_ms);
    void SendToSpectators(std::int64_t session_id, const Json::Value& message, int delay_ms);
    [[nodiscard]] bool HasLiveConnection(std::int64_t player_id, WebSocketRoute route);
    void EvictPlayerFromRoute(std::int64_t player_id,
                  WebSocketRoute route,
                  DebugTrafficLogger* logger = nullptr,
                  const drogon::WebSocketConnection* keep_connection = nullptr);
        [[nodiscard]] std::vector<drogon::WebSocketConnectionPtr> LiveConnectionsForPlayer(
                std::int64_t player_id,
                std::optional<WebSocketRoute> route = std::nullopt);

private:
    struct ConnectionEntry {
            std::int64_t player_id{0};
            WebSocketRoute route{WebSocketRoute::kLobby};
            std::weak_ptr<drogon::WebSocketConnection> connection;
    };
    DebugTrafficLogger* logger_{nullptr};
    std::mutex mutex_;
    std::vector<ConnectionEntry> connections_;
};

class DatabasePool {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] storage::Database& database();

    private:
        friend class DatabasePool;

        Lease(DatabasePool* pool, std::size_t index, storage::Database* database);
        void Release();

        DatabasePool* pool_{nullptr};
        std::size_t index_{0};
        storage::Database* database_{nullptr};
    };

    DatabasePool(std::filesystem::path database_path, std::size_t size);
    [[nodiscard]] util::Status Initialize();
    [[nodiscard]] util::StatusOr<Lease> Acquire();
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Entry {
        storage::Database database;
        bool in_use{false};
    };

    void Release(std::size_t index);

    std::filesystem::path database_path_;
    std::size_t size_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Entry> entries_;
};

class ServerState final : public game::GameTransport {
public:
    explicit ServerState(RuntimeConfig config);
    [[nodiscard]] util::Status Initialize();

    [[nodiscard]] util::StatusOr<auth::RegisterResult> Register(
        const auth::RegisterRequest& request);
    [[nodiscard]] util::StatusOr<auth::AuthenticatedSession> Login(
        const auth::LoginRequest& request);
    [[nodiscard]] util::StatusOr<auth::AuthenticatedSession> Authenticate(
        std::string_view session_token, std::int64_t now_ms);
    [[nodiscard]] util::StatusOr<auth::AuthenticatedSession> RefreshSession(
        std::string_view session_token, std::int64_t now_ms);
    [[nodiscard]] util::Status Logout(std::string_view session_token, std::int64_t now_ms);
    [[nodiscard]] util::Status ChangePassword(const auth::ChangePasswordRequest& request);

    [[nodiscard]] util::StatusOr<game::CreateGameSessionResult> CreateSession(
        const game::CreateGameSessionRequest& request);
    [[nodiscard]] util::Status ConnectPlayer(const game::ConnectPlayerRequest& request);
    [[nodiscard]] util::Status DisconnectPlayer(const game::DisconnectPlayerRequest& request);
    [[nodiscard]] util::Status HandleGameMessage(const game::RouteGameMessageRequest& request);

    [[nodiscard]] util::StatusOr<const game::PendingSession*> FindPendingSession(
        std::int64_t session_id) const;
    [[nodiscard]] util::StatusOr<const game::ActiveSession*> FindActiveSession(
        std::int64_t session_id) const;
    [[nodiscard]] std::vector<game::PendingSessionSummary> ListVisibleJoinableSessions(
        std::optional<std::int64_t> viewer_player_id) const;
    [[nodiscard]] std::vector<game::ActiveSessionSummary> ListVisibleActiveSessions(
        std::optional<std::int64_t> viewer_player_id) const;
    [[nodiscard]] std::optional<std::int64_t> FindPlayerActiveSessionId(
        std::int64_t player_id) const;
    [[nodiscard]] std::optional<std::int64_t> FindPlayerPendingSessionId(
        std::int64_t player_id) const;

    void AddConnection(const drogon::WebSocketConnectionPtr& connection,
               const std::shared_ptr<auth::PlayerProfile>& player,
               WebSocketRoute route);
    void RegisterAnonymousLobbyBrowser();
    [[nodiscard]] bool RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
                                std::int64_t player_id,
                                WebSocketRoute route);

    [[nodiscard]] std::size_t resolved_thread_count() const noexcept;
    [[nodiscard]] std::size_t database_pool_size() const noexcept;
    [[nodiscard]] const std::filesystem::path& records_path() const;

    [[nodiscard]] util::StatusOr<std::vector<replay::ReplayInfo>> ListReplays() const;
    [[nodiscard]] util::StatusOr<Json::Value> LoadReplayRound(
        std::string_view session_identifier,
        std::uint64_t round_number) const;
    [[nodiscard]] util::StatusOr<std::vector<Json::Value>> LoadReplaySession(
        std::string_view session_identifier) const;

    void LogHttpRequest(const drogon::HttpRequestPtr& request);
    void LogHttpResponse(const drogon::HttpRequestPtr& request,
                 const drogon::HttpResponsePtr& response);
    void LogWebSocketInbound(const drogon::WebSocketConnectionPtr& connection,
                    std::string_view message,
                    const drogon::WebSocketMessageType& type);
    void SendWebSocketJson(const drogon::WebSocketConnectionPtr& connection,
                  const Json::Value& message,
                  int delay_ms = 0,
                  std::optional<std::int64_t> player_id = std::nullopt,
                  std::optional<WebSocketRoute> route = std::nullopt);

    void send_to_player(std::int64_t player_id,
                   const Json::Value& message,
                   int delay_ms = 0) override;
    void send_to_spectators(std::int64_t session_id,
                               const Json::Value& message,
                               int delay_ms = 0) override;
    void on_session_ended(std::int64_t session_id,
                  const std::array<std::int64_t, 4>& player_ids,
                  const std::array<int, 4>& final_scores,
                  int round_count) override;
    [[nodiscard]] std::vector<game::PlayerRatingSnapshot> get_player_ratings(
        const std::array<std::int64_t, 4>& player_ids) override;

    [[nodiscard]] stats::StatsService& stats();
    [[nodiscard]] const stats::StatsService& stats() const;
    [[nodiscard]] GameSocketHub& socket_hub();
    [[nodiscard]] DebugTrafficLogger* traffic_logger();
    [[nodiscard]] ranking::RatingService& rating();

private:
    [[nodiscard]] auth::AuthService MakeAuthService(storage::Database* database) const;

    RuntimeConfig config_;
    std::size_t resolved_thread_count_;
    DatabasePool database_pool_;
    DebugTrafficLogger traffic_logger_;
    storage::GameRecordManager record_manager_;
    storage::Database stats_database_;
    stats::StatsService stats_service_;
    storage::Database rating_database_;
    ranking::RatingService rating_service_;
    replay::ReplayManager replay_manager_;
    random::SeedContainer seed_container_;
    auth::AuthConfig auth_config_;
    GameSocketHub socket_hub_;
    game::GameHub game_hub_;
};

// ---------------------------------------------------------------------------
// Authentication wrappers and payload builders
// ---------------------------------------------------------------------------

[[nodiscard]] util::StatusOr<auth::AuthenticatedSession> AuthenticateRequest(
    ServerState& state, const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<auth::AuthenticatedSession> AuthenticateWebSocketRequest(
    ServerState& state, const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<std::optional<auth::AuthenticatedSession>>
    AuthenticateOptionalWebSocketRequest(ServerState& state,
                                         const drogon::HttpRequestPtr& request);
[[nodiscard]] util::StatusOr<std::optional<auth::AuthenticatedSession>>
    AuthenticateOptionalRequest(ServerState& state, const drogon::HttpRequestPtr& request);

[[nodiscard]] Json::Value BuildLobbyListPayload(
    const ServerState& state,
    std::optional<std::int64_t> viewer_player_id);
[[nodiscard]] util::StatusOr<Json::Value> BuildSessionSnapshotPayload(
    ServerState& state,
    std::int64_t session_id,
    std::optional<std::int64_t> viewer_player_id);
[[nodiscard]] util::StatusOr<Json::Value> BuildCreatedSessionSnapshotPayload(
    ServerState& state,
    std::int64_t session_id,
    std::int64_t owner_player_id);
[[nodiscard]] util::StatusOr<Json::Value> BuildReplaySessionPayload(
    const ServerState& state,
    std::string_view session_identifier);

// ---------------------------------------------------------------------------
// Replay list query
// ---------------------------------------------------------------------------

struct ReplayListQuery {
    int page{1};
    int page_size{10};
    std::string session_query;
    std::string player_query;
    bool exact_session_match{false};
    std::optional<std::int64_t> started_after_ms;
    std::optional<std::int64_t> started_before_ms;
};

[[nodiscard]] util::StatusOr<std::string> ReadOptionalStringField(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label);
[[nodiscard]] util::StatusOr<std::optional<std::int64_t>> ReadOptionalInt64Field(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label);
[[nodiscard]] std::string ToLowerCopy(std::string_view value);
[[nodiscard]] bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle);
[[nodiscard]] bool EqualsCaseInsensitiveText(std::string_view left, std::string_view right);
[[nodiscard]] util::StatusOr<ReplayListQuery> ParseReplayListQuery(
    const Json::Value& object);
[[nodiscard]] util::StatusOr<Json::Value> BuildReplayListPayload(
    const ServerState& state,
    const ReplayListQuery& query);

// ---------------------------------------------------------------------------
// Route registration (defined in http_routes.cc / ws_controllers.cc)
// ---------------------------------------------------------------------------

void RegisterHttpRoutes(const std::shared_ptr<ServerState>& state);
void RegisterWebSocketControllers(const std::shared_ptr<ServerState>& state);

}  // namespace mmcr::server
