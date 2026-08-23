#include "server/runtime_internal.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>

namespace mmcr::server {
namespace {

struct StatsWsSession {
	bool has_request{false};
	bool has_cache{false};
	bool overall{false};
	std::string cache_key;
	stats::StatsFilter filter;
	std::uint64_t service_version{0};
	std::vector<const stats::RoundEntry*> time_desc_rounds;
	std::unordered_map<std::string, std::vector<const stats::RoundEntry*>> sorted_rounds;
	Json::Value stats_data;
	Json::Value fan_composition_data;
	std::size_t total_records{0};
};

class StatsWebSocketController final
	: public drogon::WebSocketController<StatsWebSocketController, false> {
public:
	explicit StatsWebSocketController(std::shared_ptr<ServerState> state)
		: state_(std::move(state)) {}

	void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
				 std::string&& message,
				 const drogon::WebSocketMessageType& type) override {
		state_->LogWebSocketInbound(connection, message, type);
		if (type != drogon::WebSocketMessageType::Text) {
			return;
		}

		auto parsed_message = ParseWebSocketMessage(message);
		if (!parsed_message.ok()) {
			state_->SendWebSocketJson(connection,
				MakeWebSocketError("invalid_message",
					parsed_message.status().message(), ""));
			return;
		}

		const Json::Value& root = parsed_message.value();
		std::string request_id;
		const Json::Value* request_id_value = FindField(root, {"requestId", "request_id"});
		if (request_id_value != nullptr && request_id_value->isString()) {
			request_id = request_id_value->asString();
		}

		auto type_value = FindMessageTypeString(root);
		if (!type_value.has_value()) {
			return;
		}

		if (*type_value == "ping") {
			state_->SendWebSocketJson(connection, MakeWebSocketAck(request_id));
			return;
		}

		if (*type_value == "overall_stats") {
			const std::string cache_key = "overall";
			StatsWsSession result;
			if (!GetReusableStatsCache(connection.get(), cache_key, result)) {
				auto built = BuildOverallStatsResult();
				if (!built.ok()) {
					state_->SendWebSocketJson(connection,
						MakeWebSocketError("stats_query_failed",
							built.status().message(), request_id));
					return;
				}
				result = std::move(built.value());
				StoreStatsCache(connection.get(), result);
			}

			Json::Value resp(Json::objectValue);
			resp["type"] = "stats";
			resp["data"] = result.stats_data;
			resp["tot_records"] = static_cast<Json::UInt64>(result.total_records);
			state_->SendWebSocketJson(connection,
				MakeWebSocketEnvelope("stats", std::move(resp), request_id));

			Json::Value fc_resp(Json::objectValue);
			fc_resp["type"] = "fan_composition";
			fc_resp["data"] = result.fan_composition_data;
			state_->SendWebSocketJson(connection,
				MakeWebSocketEnvelope("fan_composition", std::move(fc_resp), request_id));
			return;
		}

		if (*type_value == "filter") {
			const Json::Value* filter_json = FindField(root, {"filter"});
			if (filter_json == nullptr || !filter_json->isObject()) {
				state_->SendWebSocketJson(connection,
					MakeWebSocketError("invalid_request",
						"missing filter object", request_id));
				return;
			}
			auto filter = ParseStatsFilterFromJson(*filter_json);
			const auto records_request = ParseStatsRecordsRequest(root);
			const std::string cache_key = "filter:" + JsonToCompactString(*filter_json);
			StatsWsSession result;
			if (GetReusableStatsCache(connection.get(), cache_key, result)) {
				// Cached result is still current.
			} else {
				auto built = BuildStatsCache(filter, false, cache_key);
				if (!built.ok()) {
					state_->SendWebSocketJson(connection,
						MakeWebSocketError("stats_query_failed",
							built.status().message(), request_id));
					return;
				}
				result = std::move(built.value());
				StoreStatsCache(connection.get(), result);
			}

			Json::Value resp(Json::objectValue);
			resp["type"] = "stats";
			resp["data"] = result.stats_data;
			resp["tot_records"] = static_cast<Json::UInt64>(result.total_records);
			state_->SendWebSocketJson(connection,
				MakeWebSocketEnvelope("stats", std::move(resp), request_id));

			Json::Value fc_resp(Json::objectValue);
			fc_resp["type"] = "fan_composition";
			fc_resp["data"] = result.fan_composition_data;
			state_->SendWebSocketJson(connection,
				MakeWebSocketEnvelope("fan_composition", std::move(fc_resp), request_id));

			std::vector<const stats::RoundEntry*> sorted_rounds;
			const auto sorted_status = GetSortedStatsRounds(
				connection.get(), records_request.sort_field, records_request.sort_order, sorted_rounds);
			if (!sorted_status.ok()) {
				state_->SendWebSocketJson(connection,
					MakeWebSocketError(StatusCodeName(sorted_status.code()),
						sorted_status.message(), request_id));
				return;
			}
			auto records_result = SerializeStatsRoundEntriesPage(
				sorted_rounds,
				records_request.offset,
				records_request.limit);
			connection->send(std::move(records_result));
			return;
		}

		if (*type_value == "get_records") {
			const auto records_request = ParseStatsRecordsRequest(root);

			std::vector<const stats::RoundEntry*> sorted_rounds;
			const auto sorted_status = GetSortedStatsRounds(
				connection.get(), records_request.sort_field, records_request.sort_order, sorted_rounds);
			if (!sorted_status.ok()) {
				state_->SendWebSocketJson(connection,
					MakeWebSocketError(StatusCodeName(sorted_status.code()),
						sorted_status.message(), request_id));
				return;
			}

			auto result = SerializeStatsRoundEntriesPage(
				sorted_rounds,
				records_request.offset,
				records_request.limit);
			connection->send(std::move(result));
			return;
		}

		state_->SendWebSocketJson(connection,
			MakeWebSocketError("unsupported_message",
				"unknown stats message type", request_id));
	}

	void handleNewConnection(const drogon::HttpRequestPtr& request,
				const drogon::WebSocketConnectionPtr& connection) override {
		(void)request;
		std::lock_guard lock(ws_mutex_);
		ws_sessions_[connection.get()] = StatsWsSession{};
	}

	void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override {
		std::lock_guard lock(ws_mutex_);
		ws_sessions_.erase(connection.get());
	}

	WS_PATH_LIST_BEGIN
	WS_PATH_ADD("/ws/stats", drogon::Get);
	WS_PATH_LIST_END

private:
	[[nodiscard]] auto BuildOverallStatsResult() -> util::StatusOr<StatsWsSession> {
		auto& service = state_->stats();
		std::vector<const stats::RoundEntry*> rounds;
		std::uint64_t service_version = 0;
		for (;;) {
			const auto version_before = service.version();
			rounds = service.ListAllRounds();
			const auto version_after = service.version();
			if (version_before != version_after) {
				continue;
			}
			service_version = version_after;
			break;
		}

		stats::RoundCollection collection;
		for (const auto* entry : rounds) {
			collection.add_round(entry);
		}

		StatsWsSession result;
		result.has_request = true;
		result.has_cache = true;
		result.overall = true;
		result.cache_key = "overall";
		result.filter.exclude_superior_fans = false;
		result.filter.nonstandard_only = false;
		result.service_version = service_version;
		result.time_desc_rounds = std::move(rounds);
		result.stats_data = collection.ToJson(false);
		result.fan_composition_data = collection.FanCompositionStatsJson(false);
		result.total_records = collection.rounds.size();
		return result;
	}

	[[nodiscard]] auto BuildStatsCache(const stats::StatsFilter& filter,
					 bool overall,
					 std::string cache_key) -> util::StatusOr<StatsWsSession> {
		auto& service = state_->stats();
		stats::RoundCollection collection;
		std::vector<const stats::RoundEntry*> time_desc_rounds;
		std::uint64_t service_version = 0;
		for (;;) {
			const auto version_before = service.version();
			auto queried = service.Query(filter);
			if (!queried.ok()) {
				return queried.status();
			}
			auto queried_time_desc_rounds = overall ? service.ListAllRounds() : queried.value().rounds;
			const auto version_after = service.version();
			if (version_before != version_after) {
				continue;
			}
			collection = std::move(queried.value());
			time_desc_rounds = std::move(queried_time_desc_rounds);
			service_version = version_after;
			break;
		}

		StatsWsSession cache;
		cache.has_request = true;
		cache.has_cache = true;
		cache.overall = overall;
		cache.cache_key = std::move(cache_key);
		cache.filter = filter;
		cache.service_version = service_version;
		cache.time_desc_rounds = std::move(time_desc_rounds);
		cache.stats_data = collection.ToJson(filter.exclude_superior_fans);
		cache.fan_composition_data = collection.FanCompositionStatsJson(filter.exclude_superior_fans);
		cache.total_records = collection.rounds.size();

		if (filter.player_id.has_value()) {
			auto& rating_service = state_->rating();
			auto rating_result = rating_service.GetRating(*filter.player_id,
				std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
			if (rating_result.ok()) {
				const auto& rating = rating_result.value();
				cache.stats_data["player_mu"] = rating.mu;
				cache.stats_data["player_tau"] = rating.tau;
				cache.stats_data["player_sigma"] = rating.sigma;
				cache.stats_data["player_points"] = rating.points;
				cache.stats_data["player_level"] = rating.level;
			}
		}

		return cache;
	}

	[[nodiscard]] auto GetReusableStatsCache(const drogon::WebSocketConnection* connection,
					       const std::string& cache_key,
					       StatsWsSession& out) -> bool {
		std::lock_guard lock(ws_mutex_);
		auto it = ws_sessions_.find(connection);
		if (it == ws_sessions_.end() || !it->second.has_cache || it->second.cache_key != cache_key) {
			return false;
		}
		if (it->second.service_version != state_->stats().version()) {
			return false;
		}
		out = it->second;
		return true;
	}

	void StoreStatsCache(const drogon::WebSocketConnection* connection,
				     const StatsWsSession& cache) {
		std::lock_guard lock(ws_mutex_);
		ws_sessions_[connection] = cache;
	}

	[[nodiscard]] auto RefreshStatsCacheForRecords(const drogon::WebSocketConnection* connection)
		-> util::Status {
		StatsWsSession snapshot;
		{
			std::lock_guard lock(ws_mutex_);
			auto it = ws_sessions_.find(connection);
			if (it == ws_sessions_.end() || !it->second.has_cache) {
				return util::Status::InvalidArgument("send filter first");
			}
			if (it->second.service_version == state_->stats().version()) {
				return util::Status::Ok();
			}
			snapshot = it->second;
		}

		auto rebuilt = snapshot.overall
			? BuildOverallStatsResult()
			: BuildStatsCache(snapshot.filter, snapshot.overall, snapshot.cache_key);
		if (!rebuilt.ok()) {
			return rebuilt.status();
		}
		StoreStatsCache(connection, rebuilt.value());
		return util::Status::Ok();
	}

	[[nodiscard]] auto GetSortedStatsRounds(const drogon::WebSocketConnection* connection,
					      std::string_view sort_field,
					      std::string_view sort_order,
					      std::vector<const stats::RoundEntry*>& out)
		-> util::Status {
		auto refresh_status = RefreshStatsCacheForRecords(connection);
		if (!refresh_status.ok()) {
			return refresh_status;
		}

		std::lock_guard lock(ws_mutex_);
		auto it = ws_sessions_.find(connection);
		if (it == ws_sessions_.end() || !it->second.has_cache) {
			return util::Status::InvalidArgument("send filter first");
		}

		auto& session = it->second;
		const auto sort_key = StatsRecordSortKey(sort_field, sort_order);
		auto sorted_it = session.sorted_rounds.find(sort_key);
		if (sorted_it == session.sorted_rounds.end()) {
			auto sorted = BuildStatsRecordOrder(session.time_desc_rounds, sort_field, sort_order);
			sorted_it = session.sorted_rounds.emplace(sort_key, std::move(sorted)).first;
		}
		out = sorted_it->second;
		return util::Status::Ok();
	}

	std::shared_ptr<ServerState> state_;
	std::recursive_mutex ws_mutex_;
	std::unordered_map<const drogon::WebSocketConnection*, StatsWsSession> ws_sessions_;
};

class ReplayWebSocketController final
	: public drogon::WebSocketController<ReplayWebSocketController, false> {
public:
	explicit ReplayWebSocketController(std::shared_ptr<ServerState> state)
		: state_(std::move(state)) {}

	void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
				 std::string&& message,
				 const drogon::WebSocketMessageType& type) override {
		state_->LogWebSocketInbound(connection, message, type);
		if (type != drogon::WebSocketMessageType::Text) {
			return;
		}

		auto parsed_message = ParseWebSocketMessage(message);
		if (!parsed_message.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("invalid_message", parsed_message.status().message(), ""),
				0,
				std::nullopt,
				WebSocketRoute::kReplay);
			return;
		}

		auto type_value = FindMessageTypeString(parsed_message.value());
		if (type_value.has_value() && *type_value == "ping") {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketAck(""),
				0,
				std::nullopt,
				WebSocketRoute::kReplay);
			return;
		}

		state_->SendWebSocketJson(
			connection,
			MakeWebSocketError("unsupported_message", "replay websocket is read-only", ""),
			0,
			std::nullopt,
			WebSocketRoute::kReplay);
	}

	void handleNewConnection(const drogon::HttpRequestPtr& request,
				    const drogon::WebSocketConnectionPtr& connection) override {
		const std::string session_identifier = request->getParameter("session");
		if (session_identifier.empty()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("invalid_request", "session query parameter is required", ""),
				0,
				std::nullopt,
				WebSocketRoute::kReplay);
			connection->forceClose();
			return;
		}

		auto payload = BuildReplaySessionPayload(*state_, session_identifier);
		if (!payload.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("replay_load_failed", payload.status().message(), ""),
				0,
				std::nullopt,
				WebSocketRoute::kReplay);
			connection->forceClose();
			return;
		}

		state_->SendWebSocketJson(
			connection,
			MakeWebSocketEnvelope("replay.session", std::move(payload.value())),
			0,
			std::nullopt,
			WebSocketRoute::kReplay);
	}

	void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override {
		(void)connection;
	}

	WS_PATH_LIST_BEGIN
	WS_PATH_ADD("/ws/replay", drogon::Get);
	WS_PATH_LIST_END

private:
	std::shared_ptr<ServerState> state_;
};

class ReplayListWebSocketController final
	: public drogon::WebSocketController<ReplayListWebSocketController, false> {
public:
	explicit ReplayListWebSocketController(std::shared_ptr<ServerState> state)
		: state_(std::move(state)) {}

	void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
				 std::string&& message,
				 const drogon::WebSocketMessageType& type) override {
		state_->LogWebSocketInbound(connection, message, type);
		if (type != drogon::WebSocketMessageType::Text) {
			return;
		}

		auto parsed_message = ParseWebSocketMessage(message);
		if (!parsed_message.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("invalid_message", parsed_message.status().message(), ""));
			return;
		}

		const Json::Value& root = parsed_message.value();
		std::string request_id;
		const Json::Value* request_id_value = FindField(root, {"requestId", "request_id"});
		if (request_id_value != nullptr && request_id_value->isString()) {
			request_id = request_id_value->asString();
		}

		auto type_value = FindMessageTypeString(root);
		if (type_value.has_value() && *type_value == "ping") {
			state_->SendWebSocketJson(connection, MakeWebSocketAck(request_id));
			return;
		}
		if (!type_value.has_value() || *type_value != "replay.list.query") {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					"unsupported_message",
					"replay list websocket only accepts replay.list.query messages",
					request_id));
			return;
		}

		const Json::Value* payload_value = FindField(root, {"payload"});
		const Json::Value& payload_object =
			(payload_value != nullptr && payload_value->isObject())
				? *payload_value
				: Json::Value(Json::objectValue);
		auto query = ParseReplayListQuery(payload_object);
		if (!query.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("invalid_request", query.status().message(), request_id));
			return;
		}

		auto response_payload = BuildReplayListPayload(*state_, query.value());
		if (!response_payload.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					"replay_list_load_failed",
					response_payload.status().message(),
					request_id));
			return;
		}

		state_->SendWebSocketJson(
			connection,
			MakeWebSocketEnvelope("replay.list.page", std::move(response_payload.value()), request_id));
	}

	void handleNewConnection(const drogon::HttpRequestPtr& request,
				    const drogon::WebSocketConnectionPtr& connection) override {
		(void)request;
		auto payload = BuildReplayListPayload(*state_, ReplayListQuery{});
		if (!payload.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError("replay_list_load_failed", payload.status().message(), ""));
			connection->forceClose();
			return;
		}

		state_->SendWebSocketJson(
			connection,
			MakeWebSocketEnvelope("replay.list.page", std::move(payload.value())));
	}

	void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override {
		(void)connection;
	}

	WS_PATH_LIST_BEGIN
	WS_PATH_ADD("/ws/replays", drogon::Get);
	WS_PATH_LIST_END

private:
	std::shared_ptr<ServerState> state_;
};

class GameWebSocketController final
	: public drogon::WebSocketController<GameWebSocketController, false> {
public:
	explicit GameWebSocketController(std::shared_ptr<ServerState> state)
		: state_(std::move(state)) {}

	void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
				  std::string&& message,
				  const drogon::WebSocketMessageType& type) override {
		if (type != drogon::WebSocketMessageType::Text) {
			return;
		}

		state_->LogWebSocketInbound(connection, message, type);

		auto context = connection->getContext<GameClientContext>();
		if (!context) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					"unauthorized", "websocket client is not authenticated", {}));
			connection->forceClose();
			return;
		}

		auto parsed_message = ParseWebSocketMessage(message);
		if (!parsed_message.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					"invalid_request", parsed_message.status().message(), {}));
			return;
		}

		const Json::Value& root = parsed_message.value();
		std::string request_id;
		const Json::Value* request_id_value = FindField(root, {"requestId", "request_id"});
		if (request_id_value != nullptr && request_id_value->isString()) {
			request_id = request_id_value->asString();
		}

		auto message_type = FindMessageTypeString(root);
		if (message_type.has_value() && *message_type == "ping") {
			Json::Value pong_payload(Json::objectValue);
			const Json::Value* payload_value = FindField(root, {"payload"});
			if (payload_value != nullptr && payload_value->isObject()) {
				const Json::Value* identifier_value = FindField(*payload_value, {"identifier"});
				if (identifier_value != nullptr) {
					if (identifier_value->isUInt64()) {
						pong_payload["identifier"] = identifier_value->asUInt64();
					} else if (identifier_value->isInt64()) {
						pong_payload["identifier"] = identifier_value->asInt64();
					} else if (identifier_value->isInt()) {
						pong_payload["identifier"] = identifier_value->asInt();
					}
				}
			}
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketEnvelope("pong", std::move(pong_payload), request_id));
			return;
		}
		if (message_type.has_value()) {
			auto expected_route = ClassifyInboundMessageRoute(*message_type);
			if (expected_route.has_value() && *expected_route != context->route()) {
				state_->SendWebSocketJson(
					connection,
					MakeWebSocketError(
						"wrong_socket",
						"message type '" + *message_type + "' must be sent via " +
							std::string(WebSocketRoutePath(*expected_route)),
						request_id));
				return;
			}
		}

		auto status = state_->HandleGameMessage(
			game::RouteGameMessageRequest{
				.player = context->player_handle(),
				.message = root,
			});
		if (!status.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					StatusCodeName(status.code()), status.message(), request_id));
			return;
		}

		context->mark_registered_with_hub();
		state_->SendWebSocketJson(connection, MakeWebSocketAck(request_id));
	}

	void handleNewConnection(const drogon::HttpRequestPtr& request,
				 const drogon::WebSocketConnectionPtr& connection) override {
		const auto route = ResolveWebSocketRoute(request);
		if (route == WebSocketRoute::kGame) {
			auto authenticated = AuthenticateWebSocketRequest(*state_, request);
			if (!authenticated.ok()) {
				state_->SendWebSocketJson(
					connection,
					MakeWebSocketError(
						"unauthorized", authenticated.status().message(), {}),
					0,
					std::nullopt,
					route);
				connection->forceClose();
				return;
			}

			auto player = std::make_shared<auth::PlayerProfile>(authenticated.value().player);
			connection->setContext(std::make_shared<GameClientContext>(player, route));

			state_->AddConnection(connection, player, route);

			// Register the new connection first so eviction of older game sockets
			// cannot transiently disconnect the player from the hub.
			state_->socket_hub().EvictPlayerFromRoute(
				player->player_id, WebSocketRoute::kGame,
				state_->traffic_logger(), connection.get());

			auto requested_session_id = ExtractWebSocketSessionId(request);
			if (!requested_session_id.ok()) {
				state_->SendWebSocketJson(
					connection,
					MakeWebSocketError(
						StatusCodeName(requested_session_id.status().code()),
						requested_session_id.status().message(),
						{}),
					0,
					player->player_id,
					route);
				connection->forceClose();
				return;
			}

			auto context = connection->getContext<GameClientContext>();
			auto send_snapshot = [&](std::int64_t session_id) {
				auto snapshot = BuildSessionSnapshotPayload(
					*state_,
					session_id,
					std::optional<std::int64_t>(player->player_id));
				if (!snapshot.ok()) {
					state_->SendWebSocketJson(
						connection,
						MakeWebSocketError(
							StatusCodeName(snapshot.status().code()),
							snapshot.status().message(),
							{}),
						0,
						player->player_id,
						route);
					return false;
				}

				state_->SendWebSocketJson(
					connection,
					MakeWebSocketEnvelope("session.snapshot", std::move(snapshot.value())),
					0,
					player->player_id,
					route);
				if (context) {
					context->mark_registered_with_hub();
				}
				return true;
			};

			auto send_resume_required = [&](std::int64_t session_id) {
				Json::Value payload(Json::objectValue);
				payload["session_id"] = Json::Int64(session_id);
				state_->SendWebSocketJson(
					connection,
					MakeWebSocketEnvelope("resume.required", std::move(payload)),
					game::GameConfig::network_delay_ms,
					player->player_id,
					route);
				return true;
			};

			const auto active_session_id = state_->FindPlayerActiveSessionId(player->player_id);
			const auto pending_session_id = state_->FindPlayerPendingSessionId(player->player_id);

			if (requested_session_id.value().has_value()) {
				const auto target_session_id = *requested_session_id.value();
				if (active_session_id.has_value() && *active_session_id == target_session_id) {
					(void)send_resume_required(target_session_id);
					return;
				}

				auto status = state_->ConnectPlayer(
					{.player = *player, .session_id = target_session_id});
				if (!status.ok()) {
					state_->SendWebSocketJson(
						connection,
						MakeWebSocketError(
							StatusCodeName(status.code()),
							status.message(),
							{}),
						0,
						player->player_id,
						route);
					return;
				}
				if (context) {
					context->mark_registered_with_hub();
				}
				return;
			}

			if (active_session_id.has_value()) {
				(void)send_resume_required(*active_session_id);
				return;
			}

			if (pending_session_id.has_value()) {
				(void)send_snapshot(*pending_session_id);
				return;
			}

			return;
		}

		auto maybe_authenticated = AuthenticateOptionalWebSocketRequest(*state_, request);
		if (!maybe_authenticated.ok()) {
			state_->SendWebSocketJson(
				connection,
				MakeWebSocketError(
					"unauthorized", maybe_authenticated.status().message(), {}),
				0,
				std::nullopt,
				route);
			connection->forceClose();
			return;
		}

		std::shared_ptr<auth::PlayerProfile> player;
		std::optional<std::int64_t> viewer_player_id;
		if (maybe_authenticated.value().has_value()) {
			player = std::make_shared<auth::PlayerProfile>(maybe_authenticated.value()->player);
			viewer_player_id = player->player_id;
		}

		connection->setContext(std::make_shared<GameClientContext>(player, route));
		state_->AddConnection(connection, player, route);
		if (player == nullptr) {
			state_->RegisterAnonymousLobbyBrowser();
		}

		Json::Value payload = BuildLobbyListPayload(*state_, viewer_player_id);
		if (player != nullptr) {
			payload["player"] = SerializePlayer(*player);
		}
		state_->SendWebSocketJson(
			connection,
			MakeWebSocketEnvelope("lobby.list.snapshot", std::move(payload)),
			0,
			player != nullptr ? std::optional<std::int64_t>(player->player_id) : std::nullopt,
			route);
	}

	void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override {
		auto context = connection->getContext<GameClientContext>();
		if (!context) {
			return;
		}

		const bool still_connected = state_->RemoveConnection(
			connection,
			context->player_id(),
			context->route());
		// Only disconnect from the hub for game-route WebSocket drops.
		// Lobby-route connections close naturally when the player navigates
		// to the game page — we must not kick them from a pending session.
		if (!still_connected && context->registered_with_hub() &&
			context->route() == WebSocketRoute::kGame) {
			(void)state_->DisconnectPlayer(game::DisconnectPlayerRequest{
				.player_id = context->player_id(),
			});
		}
	}

	WS_PATH_LIST_BEGIN
	WS_PATH_ADD("/ws/lobby", drogon::Get);
	WS_PATH_ADD("/ws/game", drogon::Get);
	WS_PATH_LIST_END

private:
	std::shared_ptr<ServerState> state_;
};

}  // namespace

void RegisterWebSocketControllers(const std::shared_ptr<ServerState>& state) {
	drogon::app().registerController(std::make_shared<GameWebSocketController>(state));
	drogon::app().registerController(std::make_shared<ReplayListWebSocketController>(state));
	drogon::app().registerController(std::make_shared<ReplayWebSocketController>(state));
	drogon::app().registerController(std::make_shared<StatsWebSocketController>(state));
}

}  // namespace mmcr::server
