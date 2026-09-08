#include "server/runtime_internal.h"

#include <string>
#include <utility>

#include <drogon/drogon.h>

#include "external/qingque/rules/qingque.h"
#include "util/build_info.h"

namespace mmcr::server {

void RegisterHttpRoutes(const std::shared_ptr<ServerState>& state) {
    drogon::app().registerPreRoutingAdvice(
        [state](const drogon::HttpRequestPtr& request,
           drogon::AdviceCallback&& callback,
           drogon::AdviceChainCallback&& chain_callback) {
            if (request->path().rfind("/api/v1/", 0) == 0) {
                state->LogHttpRequest(request);
            }
            if (request->method() == drogon::Options &&
                request->path().rfind("/api/v1/", 0) == 0) {
                callback(NewNoContentResponse());
                return;
            }

            chain_callback();
        });

    drogon::app().registerPostHandlingAdvice(
        [state](const drogon::HttpRequestPtr& request,
               const drogon::HttpResponsePtr& response) {
            if (request->path().rfind("/api/v1/", 0) != 0) {
                return;
            }
            state->LogHttpResponse(request, response);
        });

    drogon::app().registerHandler(
        "/healthz",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            Json::Value payload(Json::objectValue);
            payload["project"] = std::string(mmcr::util::kProjectName);
            payload["version"] = std::string(mmcr::util::kBuildVersion);
            payload["status"] = "ok";
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/auth/register",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto body = ParseJsonBody(request);
            if (!body.ok()) {
                callback(NewStatusErrorResponse(body.status()));
                return;
            }

            auto username = ReadRequiredString(*body.value(), {"username"}, "username");
            if (!username.ok()) {
                callback(NewStatusErrorResponse(username.status()));
                return;
            }

            auto password = ReadRequiredString(*body.value(), {"password"}, "password");
            if (!password.ok()) {
                callback(NewStatusErrorResponse(password.status()));
                return;
            }

            auto registered = state->Register(
                {.username = username.value(),
                 .password = password.value(),
                 .now_ms = CurrentUnixTimeMs()});
            if (!registered.ok()) {
                callback(NewStatusErrorResponse(registered.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            payload["player"] = SerializePlayer(registered.value().player);
            callback(NewJsonResponse(std::move(payload), drogon::k201Created));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/login",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto body = ParseJsonBody(request);
            if (!body.ok()) {
                callback(NewStatusErrorResponse(body.status()));
                return;
            }

            auto identity = ReadRequiredString(
                *body.value(), {"identity", "username"}, "identity");
            if (!identity.ok()) {
                callback(NewStatusErrorResponse(identity.status()));
                return;
            }

            auto password = ReadRequiredString(*body.value(), {"password"}, "password");
            if (!password.ok()) {
                callback(NewStatusErrorResponse(password.status()));
                return;
            }

            auto session = state->Login(
                {.identity = identity.value(),
                 .password = password.value(),
                 .now_ms = CurrentUnixTimeMs()});
            if (!session.ok()) {
                callback(NewUnauthorizedResponse(session.status().message()));
                return;
            }

            callback(NewJsonResponse(SerializeAuthenticatedSession(session.value())));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/refresh",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto token = ExtractBearerToken(request);
            if (!token.ok()) {
                callback(NewUnauthorizedResponse(token.status().message()));
                return;
            }

            auto refreshed = state->RefreshSession(token.value(), CurrentUnixTimeMs());
            if (!refreshed.ok()) {
                callback(NewUnauthorizedResponse(refreshed.status().message()));
                return;
            }

            callback(NewJsonResponse(SerializeAuthenticatedSession(refreshed.value())));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/logout",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto token = ExtractBearerToken(request);
            if (!token.ok()) {
                callback(NewUnauthorizedResponse(token.status().message()));
                return;
            }

            auto status = state->Logout(token.value(), CurrentUnixTimeMs());
            if (!status.ok()) {
                callback(NewUnauthorizedResponse(status.message()));
                return;
            }

            callback(NewNoContentResponse());
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/auth/password/change",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto token = ExtractBearerToken(request);
            if (!token.ok()) {
                callback(NewUnauthorizedResponse(token.status().message()));
                return;
            }

            auto body = ParseJsonBody(request);
            if (!body.ok()) {
                callback(NewStatusErrorResponse(body.status()));
                return;
            }

            auto current_password = ReadRequiredString(
                *body.value(), {"current_password", "currentPassword"}, "current_password");
            if (!current_password.ok()) {
                callback(NewStatusErrorResponse(current_password.status()));
                return;
            }

            auto new_password = ReadRequiredString(
                *body.value(), {"new_password", "newPassword"}, "new_password");
            if (!new_password.ok()) {
                callback(NewStatusErrorResponse(new_password.status()));
                return;
            }

            auto status = state->ChangePassword(
                {.session_token = token.value(),
                 .current_password = current_password.value(),
                 .new_password = new_password.value(),
                 .now_ms = CurrentUnixTimeMs()});
            if (!status.ok()) {
                if (status.code() == util::StatusCode::kInvalidArgument ||
                    status.code() == util::StatusCode::kNotFound) {
                    callback(NewUnauthorizedResponse(status.message()));
                    return;
                }
                callback(NewStatusErrorResponse(status));
                return;
            }

            callback(NewOkResponse());
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/me",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }

            callback(NewJsonResponse(SerializeAuthenticatedSession(authenticated.value())));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/calc",
        [](const drogon::HttpRequestPtr& request,
           std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto body = ParseJsonBody(request);
            if (!body.ok()) {
                callback(NewStatusErrorResponse(body.status()));
                return;
            }

            auto expression = ReadRequiredString(*body.value(), {"expression"}, "expression");
            if (!expression.ok()) {
                callback(NewStatusErrorResponse(expression.status()));
                return;
            }

            callback(NewTextResponse(EvaluateCalculatorExpression(expression.value())));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/v1/replays",
        [state](const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto replays = state->ListReplays();
            if (!replays.ok()) {
                callback(NewStatusErrorResponse(replays.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            payload["replays"] = SerializeReplayInfoList(replays.value());
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandlerViaRegex(
        "^/api/v1/replay/([^/]+)/rounds-after$",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback,
                const std::string& session_identifier) {
            const std::string round_param = request->getParameter("round");
            std::uint64_t after_round = 0;
            if (!round_param.empty()) {
                after_round = static_cast<std::uint64_t>(std::stoull(round_param));
            }

            auto round_records = state->LoadReplaySession(session_identifier);
            if (!round_records.ok()) {
                callback(NewStatusErrorResponse(round_records.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            Json::Value new_rounds(Json::arrayValue);
            for (auto& round_record : round_records.value()) {
                const auto& header = round_record["header"];
                std::uint64_t rn = header.isObject() && header.isMember("round_number")
                    ? header["round_number"].asUInt64()
                    : 0;
                if (rn > after_round) {
                    NormalizeReplaySeedFields(round_record);
                    new_rounds.append(std::move(round_record));
                }
            }
            payload["round_records"] = std::move(new_rounds);
            payload["round_count"] = Json::UInt64(round_records.value().size());
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/stats/players",
        [state](const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto players = state->stats().ListPlayers();
            Json::Value payload(Json::objectValue);
            Json::Value players_array(Json::arrayValue);
            for (const auto& player : players) {
                Json::Value player_json(Json::objectValue);
                player_json["player_id"] = Json::Int64(player.player_id);
                player_json["username"] = player.username;
                players_array.append(std::move(player_json));
            }
            payload["players"] = std::move(players_array);
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/stats/big_wins",
        [state](const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            stats::StatsFilter filter;
            filter.min_fan = kLargeHandCutoff;
            auto result = state->stats().ListRounds(filter, "time", "desc", 0, kBigWinsMaxCount);
            if (!result.ok()) {
                callback(NewStatusErrorResponse(result.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            Json::Value big_wins(Json::arrayValue);
            for (const auto* round_entry : result.value().rounds) {
                Json::Value entry(Json::objectValue);
                entry["winner"] = std::string(round_entry->winner_username());
                entry["fan"] = round_entry->fan;
                entry["time"] = Json::Int64(round_entry->timestamp_ms);
                entry["game_folder"] = round_entry->round_key.session_identifier;
                entry["game_index"] = static_cast<int>(round_entry->round_key.round_number);
                entry["winner_seat"] = round_entry->winner_seat;

                if (!round_entry->fan_ids.empty()) {
                    std::string fans_str;
                    auto fan_code = qingque::dedupe(round_entry->fan_results[0]);
                    for (std::size_t j = 0, cnt = 0; j < round_entry->fan_ids.size(); ++j) {
                        if (!fan_code[round_entry->fan_ids[j]]) {
                            continue;
                        }
                        if (cnt > 0) {
                            fans_str += ", ";
                        }
                        ++cnt;
                        const int fan_id = round_entry->fan_ids[j];
                        if (fan_id >= 0 && fan_id < static_cast<int>(qingque::fans.size())) {
                            fans_str += qingque::fans[static_cast<std::size_t>(fan_id)].name;
                        }
                    }
                    entry["fans_str"] = std::move(fans_str);
                }
                big_wins.append(std::move(entry));
            }
            payload["big_wins"] = std::move(big_wins);
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/rating",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }
            const auto now_ts = CurrentUnixTimeMs() / 1000;
            auto rating = state->rating().GetRating(
                authenticated.value().player.player_id, now_ts);
            Json::Value payload(Json::objectValue);
            payload["player_id"] = Json::Int64(authenticated.value().player.player_id);
            if (rating.ok()) {
                payload["mu"] = rating.value().mu;
                payload["tau"] = rating.value().tau;
                payload["sigma"] = rating.value().sigma;
                payload["points"] = rating.value().points;
                payload["level"] = rating.value().level;
                payload["total_games"] = Json::Int64(rating.value().total_games);
            } else {
                payload["mu"] = 0.0;
                payload["tau"] = 15.0;
                payload["sigma"] = 300.0;
                payload["points"] = 0.0;
                payload["level"] = 0;
                payload["total_games"] = Json::Int64(0);
            }
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/lobby/sessions",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto maybe_authenticated = AuthenticateOptionalRequest(*state, request);
            if (!maybe_authenticated.ok()) {
                callback(NewUnauthorizedResponse(maybe_authenticated.status().message()));
                return;
            }

            const std::optional<std::int64_t> viewer_player_id =
                maybe_authenticated.value().has_value()
                    ? std::optional<std::int64_t>(maybe_authenticated.value()->player.player_id)
                    : std::nullopt;

            callback(NewJsonResponse(BuildLobbyListPayload(*state, viewer_player_id)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/v1/lobby/sessions",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }

            Json::Value empty_body(Json::objectValue);
            const Json::Value* body_value = &empty_body;
            const auto& body = request->getJsonObject();
            if (body) {
                if (!body->isObject()) {
                    callback(NewStatusErrorResponse(
                        util::Status::InvalidArgument("request body must be a JSON object")));
                    return;
                }
                body_value = body.get();
            } else if (!request->getJsonError().empty()) {
                callback(NewStatusErrorResponse(
                    util::Status::InvalidArgument(request->getJsonError())));
                return;
            }

            game::GameConfig game_config;
            const Json::Value* game_config_value = FindField(*body_value, {"game_config", "gameConfig"});
            if (game_config_value != nullptr) {
                auto parsed_game_config = ParseGameConfig(*game_config_value);
                if (!parsed_game_config.ok()) {
                    callback(NewStatusErrorResponse(parsed_game_config.status()));
                    return;
                }
                game_config = parsed_game_config.value();
            }

            game::QueueConfig queue_config;
            const Json::Value* queue_config_value = FindField(*body_value, {"queue_config", "queueConfig"});
            if (queue_config_value != nullptr) {
                auto parsed_queue_config = ParseQueueConfig(*queue_config_value);
                if (!parsed_queue_config.ok()) {
                    callback(NewStatusErrorResponse(parsed_queue_config.status()));
                    return;
                }
                queue_config = parsed_queue_config.value();
            }

            auto created = state->CreateSession(
                {.owner = authenticated.value().player,
                 .game_config = game_config,
                 .queue_config = queue_config});
            if (!created.ok()) {
                callback(NewStatusErrorResponse(created.status()));
                return;
            }

            auto snapshot = BuildCreatedSessionSnapshotPayload(
                *state,
                created.value().session_id,
                authenticated.value().player.player_id);
            if (!snapshot.ok()) {
                callback(NewStatusErrorResponse(snapshot.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            payload["session"] = std::move(snapshot.value());
            callback(NewJsonResponse(std::move(payload), drogon::k201Created));
        },
        {drogon::Post});

    drogon::app().registerHandlerViaRegex(
        "^/api/v1/lobby/sessions/([^/]+)$",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback,
                const std::string& raw_session_id) {
            auto maybe_authenticated = AuthenticateOptionalRequest(*state, request);
            if (!maybe_authenticated.ok()) {
                callback(NewUnauthorizedResponse(maybe_authenticated.status().message()));
                return;
            }

            auto session_id = ParseInt64String(raw_session_id, "session_id");
            if (!session_id.ok()) {
                callback(NewStatusErrorResponse(session_id.status()));
                return;
            }

            const std::optional<std::int64_t> viewer_player_id =
                maybe_authenticated.value().has_value()
                    ? std::optional<std::int64_t>(maybe_authenticated.value()->player.player_id)
                    : std::nullopt;

            auto snapshot = BuildSessionSnapshotPayload(*state, session_id.value(), viewer_player_id);
            if (!snapshot.ok()) {
                callback(NewStatusErrorResponse(snapshot.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            payload["session"] = std::move(snapshot.value());
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Get});

    drogon::app().registerHandlerViaRegex(
        "^/api/v1/lobby/sessions/([^/]+)/join$",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback,
                const std::string& raw_session_id) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }

            auto session_id = ParseInt64String(raw_session_id, "session_id");
            if (!session_id.ok()) {
                callback(NewStatusErrorResponse(session_id.status()));
                return;
            }

            auto status = state->ConnectPlayer(
                {.player = authenticated.value().player, .session_id = session_id.value()});
            if (!status.ok()) {
                callback(NewStatusErrorResponse(status));
                return;
            }

            auto snapshot = BuildSessionSnapshotPayload(
                *state,
                session_id.value(),
                std::optional<std::int64_t>(authenticated.value().player.player_id));
            if (!snapshot.ok()) {
                callback(NewStatusErrorResponse(snapshot.status()));
                return;
            }

            Json::Value payload(Json::objectValue);
            payload["session"] = std::move(snapshot.value());
            callback(NewJsonResponse(std::move(payload)));
        },
        {drogon::Post});

    drogon::app().registerHandlerViaRegex(
        "^/api/v1/lobby/sessions/([^/]+)/leave$",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback,
                const std::string& raw_session_id) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }

            auto session_id = ParseInt64String(raw_session_id, "session_id");
            if (!session_id.ok()) {
                callback(NewStatusErrorResponse(session_id.status()));
                return;
            }

            auto player = std::make_shared<auth::PlayerProfile>(authenticated.value().player);
            Json::Value payload(Json::objectValue);
            payload["session_id"] = Json::Int64(session_id.value());

            auto status = state->HandleGameMessage(
                {.player = auth::PlayerProfilePtr(player),
                 .message = BuildGameMessage("session.leave", std::move(payload))});
            if (!status.ok()) {
                callback(NewStatusErrorResponse(status));
                return;
            }

            callback(NewNoContentResponse());
        },
        {drogon::Post});

    drogon::app().registerHandlerViaRegex(
        "^/api/v1/lobby/sessions/([^/]+)/ready$",
        [state](const drogon::HttpRequestPtr& request,
                std::function<void(const drogon::HttpResponsePtr&)> &&callback,
                const std::string& raw_session_id) {
            auto authenticated = AuthenticateRequest(*state, request);
            if (!authenticated.ok()) {
                callback(NewUnauthorizedResponse(authenticated.status().message()));
                return;
            }

            auto session_id = ParseInt64String(raw_session_id, "session_id");
            if (!session_id.ok()) {
                callback(NewStatusErrorResponse(session_id.status()));
                return;
            }

            auto body = ParseJsonBody(request);
            if (!body.ok()) {
                callback(NewStatusErrorResponse(body.status()));
                return;
            }

            auto ready = ReadRequiredBool(*body.value(), {"ready"}, "ready");
            if (!ready.ok()) {
                callback(NewStatusErrorResponse(ready.status()));
                return;
            }

            auto player = std::make_shared<auth::PlayerProfile>(authenticated.value().player);
            Json::Value payload(Json::objectValue);
            payload["session_id"] = Json::Int64(session_id.value());
            payload["ready"] = ready.value();

            auto status = state->HandleGameMessage(
                {.player = auth::PlayerProfilePtr(player),
                 .message = BuildGameMessage("queue.ready", std::move(payload))});
            if (!status.ok()) {
                callback(NewStatusErrorResponse(status));
                return;
            }

            auto snapshot = BuildSessionSnapshotPayload(
                *state,
                session_id.value(),
                std::optional<std::int64_t>(authenticated.value().player.player_id));
            if (!snapshot.ok()) {
                callback(NewStatusErrorResponse(snapshot.status()));
                return;
            }

            Json::Value response_payload(Json::objectValue);
            response_payload["session"] = std::move(snapshot.value());
            callback(NewJsonResponse(std::move(response_payload)));
        },
        {drogon::Post});
}

}  // namespace mmcr::server
