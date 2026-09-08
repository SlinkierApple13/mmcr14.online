#include "server/runtime_internal.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <drogon/drogon.h>

#include "external/qingque/rules/qingque.h"
#include "external/qingque/rules/w_data.h"

namespace mmcr::server {

const std::string_view kDefaultBindAddress = "0.0.0.0";
const std::string_view kDefaultDatabasePath = "mmcr_backend.sqlite3";
const std::string_view kDefaultDebugLogDir = "";
const std::string_view kDefaultCorsAllowHeaders = "Authorization, Content-Type";
const std::string_view kDefaultCorsAllowMethods = "GET, POST, OPTIONS";

std::size_t ResolveWorkerThreadCount(std::size_t configured_thread_count) {
    if (configured_thread_count != 0) {
        return configured_thread_count;
    }

    const auto hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        return 1;
    }

    return static_cast<std::size_t>(hardware_threads);
}

std::size_t ResolveDatabasePoolSize(std::size_t configured_thread_count) {
    const auto worker_threads = ResolveWorkerThreadCount(configured_thread_count);
    return std::max<std::size_t>(worker_threads + 2, 4);
}

std::int64_t CurrentUnixTimeMs() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string JsonToCompactString(const Json::Value& value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

std::string PointerHandleString(const void* pointer) {
    return std::to_string(reinterpret_cast<std::uintptr_t>(pointer));
}

bool IsSensitiveFieldName(std::string_view name) {
    static constexpr std::string_view kSensitiveNames[] = {
        "authorization",
        "token",
        "access_token",
        "session_token",
        "secret",
        "password",
        "current_password",
        "new_password",
    };

    auto equals_case_insensitive = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            const auto left_char = static_cast<unsigned char>(left[index]);
            const auto right_char = static_cast<unsigned char>(right[index]);
            if (std::tolower(left_char) != std::tolower(right_char)) {
                return false;
            }
        }
        return true;
    };

    for (const auto sensitive_name : kSensitiveNames) {
        if (equals_case_insensitive(name, sensitive_name)) {
            return true;
        }
    }

    return false;
}

Json::Value RedactSensitiveJson(const Json::Value& value) {
    if (value.isObject()) {
        Json::Value redacted(Json::objectValue);
        for (const auto& member : value.getMemberNames()) {
            if (IsSensitiveFieldName(member)) {
                redacted[member] = "[REDACTED]";
                continue;
            }
            redacted[member] = RedactSensitiveJson(value[member]);
        }
        return redacted;
    }

    if (value.isArray()) {
        Json::Value redacted(Json::arrayValue);
        for (const auto& entry : value) {
            redacted.append(RedactSensitiveJson(entry));
        }
        return redacted;
    }

    return value;
}

std::optional<Json::Value> ParseJsonText(std::string_view raw_text) {
    if (raw_text.empty()) {
        return std::nullopt;
    }

    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (reader == nullptr) {
        return std::nullopt;
    }
    if (!reader->parse(raw_text.data(), raw_text.data() + raw_text.size(), &parsed, &errors)) {
        return std::nullopt;
    }
    return parsed;
}

Json::Value SanitizeTextPayload(std::string_view raw_text) {
    if (raw_text.empty()) {
        return Json::Value(Json::nullValue);
    }

    auto parsed = ParseJsonText(raw_text);
    if (!parsed.has_value()) {
        return Json::Value(std::string(raw_text));
    }

    return RedactSensitiveJson(*parsed);
}

std::string SanitizeQueryString(std::string_view raw_query) {
    std::string sanitized;
    while (!raw_query.empty()) {
        const auto separator = raw_query.find('&');
        const std::string_view pair =
            separator == std::string_view::npos ? raw_query : raw_query.substr(0, separator);

        const auto equals = pair.find('=');
        const std::string_view key = equals == std::string_view::npos ? pair : pair.substr(0, equals);
        const std::string_view value =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);

        if (!sanitized.empty()) {
            sanitized.push_back('&');
        }
        sanitized.append(key);
        if (equals != std::string_view::npos) {
            sanitized.push_back('=');
            if (IsSensitiveFieldName(key)) {
                sanitized.append("[REDACTED]");
            } else {
                sanitized.append(value);
            }
        }

        if (separator == std::string_view::npos) {
            break;
        }
        raw_query.remove_prefix(separator + 1);
    }

    return sanitized;
}

util::Status DebugTrafficLogger::Initialize(const std::filesystem::path& dir) {
    dir_ = dir;
    if (!dir_.empty()) {
        std::error_code error_code;
        std::filesystem::create_directories(dir_, error_code);
        if (error_code) {
            return util::Status::Internal(
                "failed to create debug log directory: " + error_code.message());
        }
    }
    return RotateFile();
}

void DebugTrafficLogger::Log(Json::Value entry) {
    std::lock_guard lock(mutex_);
    if (!stream_.is_open() || TodayUtcDate() != current_log_date_) {
        if (auto status = RotateFile(); !status.ok()) {
            return;
        }
    }
    if (!stream_.is_open()) {
        return;
    }

    entry["timestamp_ms"] = Json::Int64(CurrentUnixTimeMs());
    const auto line = JsonToCompactString(entry);
    stream_ << line << '\n';
    stream_.flush();
}

const std::filesystem::path& DebugTrafficLogger::dir() const {
    return dir_;
}

std::string DebugTrafficLogger::TodayUtcDate() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time_t_now, &utc);
    std::ostringstream oss;
    oss << std::put_time(&utc, "%Y-%m-%d");
    return oss.str();
}

util::Status DebugTrafficLogger::RotateFile() {
    if (stream_.is_open()) {
        stream_.close();
    }
    if (dir_.empty()) {
        return util::Status::Ok();
    }
    current_log_date_ = TodayUtcDate();
    const auto file_path = dir_ / (current_log_date_ + ".jsonl");
    stream_.open(file_path, std::ios::out | std::ios::app);
    if (!stream_.is_open()) {
        return util::Status::Internal(
            "failed to open debug log file: " + file_path.string());
    }
    return util::Status::Ok();
}

Json::Value BuildHttpRequestLogEntry(const drogon::HttpRequestPtr& request) {
    Json::Value entry(Json::objectValue);
    entry["transport"] = "http";
    entry["direction"] = "in";
    entry["kind"] = "request";
    entry["request_handle"] = PointerHandleString(request.get());
    entry["method"] = request->methodString();
    entry["path"] = request->path();
    entry["query"] = SanitizeQueryString(request->query());
    entry["body"] = SanitizeTextPayload(request->body());
    return entry;
}

Json::Value BuildHttpResponseLogEntry(const drogon::HttpRequestPtr& request,
                       const drogon::HttpResponsePtr& response) {
    Json::Value entry(Json::objectValue);
    entry["transport"] = "http";
    entry["direction"] = "out";
    entry["kind"] = "response";
    entry["request_handle"] = PointerHandleString(request.get());
    entry["method"] = request->methodString();
    entry["path"] = request->path();
    entry["query"] = SanitizeQueryString(request->query());
    entry["status_code"] = static_cast<int>(response->statusCode());
    entry["content_type"] = response->contentTypeString();
    entry["body"] = SanitizeTextPayload(response->body());
    return entry;
}

std::string_view TrimString(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool EqualsCaseInsensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_char = static_cast<unsigned char>(left[index]);
        const auto right_char = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_char) != std::tolower(right_char)) {
            return false;
        }
    }

    return true;
}

util::StatusOr<std::uint64_t> ParseUnsignedInteger(std::string_view raw_value,
                          std::string_view variable_name) {
    if (raw_value.empty()) {
        return util::Status::InvalidArgument(std::string(variable_name) + " must not be empty");
    }

    std::uint64_t parsed_value = 0;
    const auto result = std::from_chars(
        raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
    if (result.ec != std::errc() || result.ptr != raw_value.data() + raw_value.size()) {
        return util::Status::InvalidArgument(
            std::string(variable_name) + " must be an unsigned integer");
    }

    return parsed_value;
}

util::StatusOr<std::int64_t> ParseInt64String(std::string_view raw_value,
                      std::string_view label) {
    if (raw_value.empty()) {
        return util::Status::InvalidArgument(std::string(label) + " must not be empty");
    }

    std::int64_t parsed_value = 0;
    const auto result = std::from_chars(
        raw_value.data(), raw_value.data() + raw_value.size(), parsed_value);
    if (result.ec != std::errc() || result.ptr != raw_value.data() + raw_value.size()) {
        return util::Status::InvalidArgument(std::string(label) + " must be an integer");
    }

    return parsed_value;
}

std::optional<std::string> ResolveCorsAllowOrigin() {
    if (const char* raw_value = std::getenv("MMCR_BACKEND_CORS_ALLOW_ORIGIN");
        raw_value != nullptr) {
        const std::string_view trimmed = TrimString(raw_value);
        if (!trimmed.empty()) {
            return std::string(trimmed);
        }
    }

    return std::nullopt;
}

void ApplyCorsHeaders(const drogon::HttpResponsePtr& response) {
    const auto allow_origin = ResolveCorsAllowOrigin();
    if (!allow_origin.has_value()) {
        return;
    }
    response->addHeader("Access-Control-Allow-Origin", *allow_origin);
    if (*allow_origin != "*") {
        response->addHeader("Access-Control-Allow-Credentials", "true");
    }
    response->addHeader("Access-Control-Allow-Headers", std::string(kDefaultCorsAllowHeaders));
    response->addHeader("Access-Control-Allow-Methods", std::string(kDefaultCorsAllowMethods));
    response->addHeader("Access-Control-Max-Age", "600");
    response->addHeader("Vary", "Origin");
}

std::string_view StatusCodeName(util::StatusCode code) {
    switch (code) {
        case util::StatusCode::kOk:
            return "ok";
        case util::StatusCode::kInvalidArgument:
            return "invalid_argument";
        case util::StatusCode::kNotFound:
            return "not_found";
        case util::StatusCode::kInternal:
            return "internal";
        case util::StatusCode::kNotImplemented:
            return "not_implemented";
    }

    return "internal";
}

drogon::HttpStatusCode ToHttpStatus(util::StatusCode code) {
    switch (code) {
        case util::StatusCode::kOk:
            return drogon::k200OK;
        case util::StatusCode::kInvalidArgument:
            return drogon::k400BadRequest;
        case util::StatusCode::kNotFound:
            return drogon::k404NotFound;
        case util::StatusCode::kInternal:
            return drogon::k500InternalServerError;
        case util::StatusCode::kNotImplemented:
            return drogon::k501NotImplemented;
    }

    return drogon::k500InternalServerError;
}

drogon::HttpResponsePtr NewJsonResponse(Json::Value payload,
                 drogon::HttpStatusCode status_code) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(payload));
    response->setStatusCode(status_code);
    ApplyCorsHeaders(response);
    return response;
}

drogon::HttpResponsePtr NewErrorResponse(drogon::HttpStatusCode status_code,
                  std::string_view code,
                  std::string message) {
    Json::Value payload(Json::objectValue);
    Json::Value error(Json::objectValue);
    error["code"] = std::string(code);
    error["message"] = std::move(message);
    payload["error"] = std::move(error);
    return NewJsonResponse(std::move(payload), status_code);
}

drogon::HttpResponsePtr NewStatusErrorResponse(const util::Status& status) {
    return NewErrorResponse(
        ToHttpStatus(status.code()),
        StatusCodeName(status.code()),
        status.message());
}

drogon::HttpResponsePtr NewUnauthorizedResponse(std::string_view message) {
    auto response = NewErrorResponse(drogon::k401Unauthorized, "unauthorized", std::string(message));
    response->addHeader("WWW-Authenticate", "Bearer");
    return response;
}

drogon::HttpResponsePtr NewNoContentResponse() {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    ApplyCorsHeaders(response);
    return response;
}

drogon::HttpResponsePtr NewOkResponse() {
    Json::Value payload(Json::objectValue);
    payload["ok"] = true;
    return NewJsonResponse(std::move(payload));
}

drogon::HttpResponsePtr NewTextResponse(std::string payload,
                 drogon::HttpStatusCode status_code) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(status_code);
    response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
    response->setBody(std::move(payload));
    ApplyCorsHeaders(response);
    return response;
}

std::string EvaluateCalculatorExpression(std::string_view expression) {
    try {
        auto hand = mahjong::utils::parse_hand(
            std::string(expression),
            0u,
            0u,
            true,
            [](char marker, mahjong::win_t& win_type) {
                switch (marker) {
                    case '%':
                        win_type |= mahjong::win_type::self_drawn;
                        return true;
                    case '^':
                        win_type |= mahjong::win_type::kong_related;
                        return true;
                    case '&':
                        win_type |= mahjong::win_type::final_tile;
                        return true;
                    case '*':
                        win_type |= mahjong::win_type::heavenly_or_earthly_hand;
                        return true;
                    case '!':
                        win_type = (win_type & 0b1111111111111100u) | 0b00u;
                        return true;
                    case '@':
                        win_type = (win_type & 0b1111111111111100u) | 0b01u;
                        return true;
                    case '#':
                        win_type = (win_type & 0b1111111111111100u) | 0b10u;
                        return true;
                    case '$':
                        win_type = (win_type & 0b1111111111111100u) | 0b11u;
                        return true;
                    default:
                        return false;
                }
            });

        if (!qingque::input_verifier(hand)) {
            return "无效输入.";
        }
        if (!qingque::is_winning_hand(hand)) {
            return "此牌不能和牌.";
        }

        const auto [fan, fan_code] = qingque::get_fan(qingque_wd::get_wd(), hand);
        const auto readable_fan_code = qingque::dedupe(fan_code);

        std::ostringstream stream;
        bool first_fan = true;
        int fan_count = 0;
        for (std::size_t index = 0; index < qingque::fans.size(); ++index) {
            if (!readable_fan_code[index]) {
                continue;
            }
            if (!first_fan) {
                stream << ", ";
            }
            stream << qingque::fans[index].name;
            first_fan = false;
            ++fan_count;
        }
        if (fan_count == 0) {
            stream << "平和; \n";
        } else {
            stream << "; \n";
        }

        const int self_drawn_point = static_cast<int>(std::round(fan * fan));
        const int discard_win_point = 3 * self_drawn_point;
        const std::string point_display = hand.winning_type()(mahjong::win_type::self_drawn)
            ? ("各 " + std::to_string(self_drawn_point) + "'")
            : (std::to_string(discard_win_point) + "'");
        stream << "共 " << std::fixed << std::setprecision(2) << fan << " 番 (" << point_display << ").";
        return stream.str();
    } catch (...) {
        return "无效输入.";
    }
}

const Json::Value* FindField(const Json::Value& object,
           std::initializer_list<std::string_view> names) {
    if (!object.isObject()) {
        return nullptr;
    }

    for (std::string_view name : names) {
        const std::string key(name);
        if (object.isMember(key)) {
            return &object[key];
        }
    }

    return nullptr;
}

util::StatusOr<std::shared_ptr<Json::Value>> ParseJsonBody(const drogon::HttpRequestPtr& request) {
    const auto& json = request->getJsonObject();
    if (!json) {
        const std::string error = request->getJsonError().empty()
                              ? "request body must be a JSON object"
                              : request->getJsonError();
        return util::Status::InvalidArgument(error);
    }
    if (!json->isObject()) {
        return util::Status::InvalidArgument("request body must be a JSON object");
    }

    return json;
}

util::StatusOr<std::string> ReadRequiredString(const Json::Value& object,
                std::initializer_list<std::string_view> names,
                std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr || !value->isString()) {
        return util::Status::InvalidArgument(std::string(label) + " must be a string");
    }
    return value->asString();
}

util::StatusOr<bool> ReadRequiredBool(const Json::Value& object,
              std::initializer_list<std::string_view> names,
              std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr || !value->isBool()) {
        return util::Status::InvalidArgument(std::string(label) + " must be a boolean");
    }
    return value->asBool();
}

util::StatusOr<int> ReadRequiredInt(const Json::Value& object,
             std::initializer_list<std::string_view> names,
             std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr) {
        return util::Status::InvalidArgument(std::string(label) + " is required");
    }

    std::int64_t parsed_value = 0;
    if (value->isInt() || value->isInt64()) {
        parsed_value = value->asInt64();
    } else if (value->isUInt() || value->isUInt64()) {
        const auto unsigned_value = value->asUInt64();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return util::Status::InvalidArgument(std::string(label) + " is out of range");
        }
        parsed_value = static_cast<std::int64_t>(unsigned_value);
    } else {
        return util::Status::InvalidArgument(std::string(label) + " must be an integer");
    }

    if (parsed_value < std::numeric_limits<int>::min() ||
        parsed_value > std::numeric_limits<int>::max()) {
        return util::Status::InvalidArgument(std::string(label) + " is out of range");
    }
    return static_cast<int>(parsed_value);
}

util::StatusOr<bool> ReadOptionalBool(const Json::Value& object,
              std::initializer_list<std::string_view> names,
              std::string_view label,
              bool default_value) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr) {
        return default_value;
    }
    if (!value->isBool()) {
        return util::Status::InvalidArgument(std::string(label) + " must be a boolean");
    }
    return value->asBool();
}

util::StatusOr<int> ReadOptionalInt(const Json::Value& object,
             std::initializer_list<std::string_view> names,
             std::string_view label,
             int default_value) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr) {
        return default_value;
    }

    std::int64_t parsed_value = 0;
    if (value->isInt() || value->isInt64()) {
        parsed_value = value->asInt64();
    } else if (value->isUInt() || value->isUInt64()) {
        const auto unsigned_value = value->asUInt64();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return util::Status::InvalidArgument(std::string(label) + " is out of range");
        }
        parsed_value = static_cast<std::int64_t>(unsigned_value);
    } else {
        return util::Status::InvalidArgument(std::string(label) + " must be an integer");
    }

    if (parsed_value < std::numeric_limits<int>::min() ||
        parsed_value > std::numeric_limits<int>::max()) {
        return util::Status::InvalidArgument(std::string(label) + " is out of range");
    }

    return static_cast<int>(parsed_value);
}

util::StatusOr<std::string> ExtractBearerToken(const drogon::HttpRequestPtr& request) {
    std::string authorization = request->getHeader("authorization");
    if (authorization.empty()) {
        authorization = request->getHeader("Authorization");
    }
    if (authorization.empty()) {
        return util::Status::InvalidArgument("missing Authorization header");
    }

    const std::string_view trimmed = TrimString(authorization);
    const auto separator = trimmed.find(' ');
    if (separator == std::string_view::npos ||
        !EqualsCaseInsensitive(trimmed.substr(0, separator), "Bearer")) {
        return util::Status::InvalidArgument("Authorization header must use Bearer token");
    }

    const std::string_view token = TrimString(trimmed.substr(separator + 1));
    if (token.empty()) {
        return util::Status::InvalidArgument("Bearer token must not be empty");
    }

    return std::string(token);
}

util::StatusOr<std::string> ExtractWebSocketToken(const drogon::HttpRequestPtr& request) {
    auto header_token = ExtractBearerToken(request);
    if (header_token.ok()) {
        return header_token;
    }

    std::string query_token = request->getParameter("access_token");
    if (query_token.empty()) {
        query_token = request->getParameter("token");
    }
    if (query_token.empty()) {
        return util::Status::InvalidArgument(
            "missing Authorization header or websocket access token query parameter");
    }

    const std::string_view trimmed = TrimString(query_token);
    if (trimmed.empty()) {
        return util::Status::InvalidArgument("websocket access token must not be empty");
    }

    return std::string(trimmed);
}

util::StatusOr<std::optional<std::int64_t>> ExtractWebSocketSessionId(
    const drogon::HttpRequestPtr& request) {
    const std::string raw_session_id = request->getParameter("session_id");
    if (raw_session_id.empty()) {
        return std::optional<std::int64_t>{};
    }

    auto session_id = ParseInt64String(raw_session_id, "session_id");
    if (!session_id.ok()) {
        return session_id.status();
    }

    return std::optional<std::int64_t>(session_id.value());
}

Json::Value SerializePlayer(const auth::PlayerProfile& player) {
    Json::Value payload(Json::objectValue);
    payload["player_id"] = Json::Int64(player.player_id);
    payload["username"] = player.username;
    return payload;
}

Json::Value SerializeSession(const auth::SessionInfo& session) {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = session.session_id;
    payload["token"] = session.token;
    payload["created_at_ms"] = Json::Int64(session.created_at_ms);
    payload["expires_at_ms"] = Json::Int64(session.expires_at_ms);
    return payload;
}

Json::Value SerializeAuthenticatedSession(const auth::AuthenticatedSession& session) {
    Json::Value payload(Json::objectValue);
    payload["player"] = SerializePlayer(session.player);
    payload["session"] = SerializeSession(session.session);
    return payload;
}

Json::Value SerializeActiveSummary(const game::ActiveSessionSummary& summary) {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(summary.session_id);
    payload["primary_timer_ms"] = summary.primary_timer_ms;
    payload["secondary_timer_ms"] = summary.secondary_timer_ms;
    payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
    payload["round_count"] = summary.round_count;
    payload["forced_end_floor"] = summary.forced_end_floor.has_value()
        ? Json::Value(*summary.forced_end_floor)
        : Json::Value(Json::nullValue);
    payload["round_counter"] = Json::UInt64(summary.round_counter);
    payload["recorded"] = summary.recorded;
    payload["debug_mode"] = summary.debug_mode;
    payload["unranked"] = summary.unranked;
    payload["singleplayer"] = summary.singleplayer;
    payload["ended"] = summary.ended;
    payload["public_session"] = summary.public_session;
    payload["abandon_game"] = summary.abandon_game;
    payload["duplicate_mode"] = summary.duplicate_mode;
    Json::Value names(Json::arrayValue);
    for (const auto& name : summary.names) {
        names.append(name);
    }
    payload["names"] = std::move(names);
    return payload;
}

Json::Value SerializeActiveSummaryList(const std::vector<game::ActiveSessionSummary>& sessions) {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        payload.append(SerializeActiveSummary(session));
    }
    return payload;
}

Json::Value SerializeReplayInfo(const replay::ReplayInfo& replay_info) {
    Json::Value payload(Json::objectValue);
    payload["session_identifier"] = replay_info.session_identifier;
    payload["timestamp_ns"] = std::to_string(replay_info.timestamp_ns);
    payload["timestamp_ms"] = Json::UInt64(replay_info.timestamp_ns / 1000000ULL);
    payload["round_count"] = Json::UInt64(replay_info.round_count);
    Json::Value names(Json::arrayValue);
    for (const auto& name : replay_info.player_names) {
        names.append(name);
    }
    payload["player_names"] = std::move(names);
    return payload;
}

std::string FormatHexSeed(std::uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

void NormalizeReplaySeedFields(Json::Value& round_record) {
    Json::Value& round_start_snapshot = round_record["round_start_snapshot"];
    if (!round_start_snapshot.isObject()) {
        return;
    }

    Json::Value& seat_shuffle_seed = round_start_snapshot["seat_shuffle_seed"];
    if (seat_shuffle_seed.isUInt64()) {
        seat_shuffle_seed = FormatHexSeed(seat_shuffle_seed.asUInt64());
    } else if (seat_shuffle_seed.isUInt()) {
        seat_shuffle_seed = FormatHexSeed(seat_shuffle_seed.asUInt());
    }

    Json::Value& wall_seeds = round_start_snapshot["wall_seeds"];
    if (!wall_seeds.isArray()) {
        return;
    }

    for (Json::ArrayIndex index = 0; index < wall_seeds.size(); ++index) {
        if (wall_seeds[index].isUInt64()) {
            wall_seeds[index] = FormatHexSeed(wall_seeds[index].asUInt64());
        } else if (wall_seeds[index].isUInt()) {
            wall_seeds[index] = FormatHexSeed(wall_seeds[index].asUInt());
        }
    }
}

// Rebuild the full round-start wall state from the stored wall seeds and
// attach it to the payload. The wall is deterministic given the seeds, so
// replays can show the wall without persisting all 136 tiles per round.
void AttachReplayWallState(Json::Value& round_record) {
    Json::Value& round_start_snapshot = round_record["round_start_snapshot"];
    if (!round_start_snapshot.isObject()) {
        return;
    }

    const Json::Value& wall_seeds = round_start_snapshot["wall_seeds"];
    if (!wall_seeds.isArray() || wall_seeds.empty()) {
        return;
    }

    std::vector<std::uint64_t> seeds;
    seeds.reserve(wall_seeds.size());
    for (const auto& seed : wall_seeds) {
        if (seed.isUInt64()) {
            seeds.push_back(seed.asUInt64());
        } else if (seed.isUInt()) {
            seeds.push_back(seed.asUInt());
        } else {
            return;
        }
    }

    Json::Value wall_tiles(Json::arrayValue);
    const auto& game_config = round_record["header"]["game_config"];
    if (game_config.isObject() && game_config["duplicate_mode"].isBool() &&
        game_config["duplicate_mode"].asBool()) {
        game::DuplicateWall wall;
        wall.prepare(std::move(seeds), {});
        for (const auto tile : wall.tiles()) {
            wall_tiles.append(Json::UInt(static_cast<unsigned int>(tile)));
        }
        round_start_snapshot["wall_tiles"] = std::move(wall_tiles);
        round_start_snapshot["duplicate_mode"] = true;
        Json::Value front_indices(Json::arrayValue);
        for (const auto index : wall.front_stack_indices()) {
            front_indices.append(Json::UInt64(index));
        }
        round_start_snapshot["wall_front_indices"] = std::move(front_indices);
        return;
    }

    game::Wall wall;
    wall.prepare(std::move(seeds), {});
    for (const auto tile : wall.tiles()) {
        wall_tiles.append(Json::UInt(static_cast<unsigned int>(tile)));
    }
    round_start_snapshot["wall_tiles"] = std::move(wall_tiles);
    round_start_snapshot["wall_front_index"] = Json::UInt64(wall.front_stack_index());
    round_start_snapshot["wall_back_index"] = Json::UInt64(wall.back_stack_index());
}

util::Status RejectImmutableGameConfigField(const Json::Value& object,
                        std::initializer_list<std::string_view> names,
                        std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr || value->isNull()) {
        return util::Status::Ok();
    }

    return util::Status::InvalidArgument(
        std::string(label) + " is fixed by the server and cannot be overridden");
}

util::Status ValidateGameConfigBounds(const game::GameConfig& config) {
    if (config.primary_timer_ms < game::GameConfig::primary_timer_min_ms ||
        config.primary_timer_ms > game::GameConfig::primary_timer_max_ms) {
        return util::Status::InvalidArgument("primary_timer_ms must be between 3000 and 15000");
    }

    if (config.secondary_timer_ms < game::GameConfig::secondary_timer_min_ms ||
        config.secondary_timer_ms > config.primary_timer_ms) {
        return util::Status::InvalidArgument("secondary_timer_ms must be between 3000 and primary_timer_ms");
    }

    if (config.auxiliary_timer_ms < game::GameConfig::auxiliary_timer_min_ms ||
        config.auxiliary_timer_ms > game::GameConfig::auxiliary_timer_max_ms) {
        return util::Status::InvalidArgument("auxiliary_timer_ms must be between 0 and 45000");
    }

    if (config.round_count < game::GameConfig::round_count_min ||
        config.round_count > game::GameConfig::round_count_max) {
        return util::Status::InvalidArgument("round_count must be between 1 and 32");
    }

    return util::Status::Ok();
}

util::StatusOr<game::GameConfig> ParseGameConfig(const Json::Value& object) {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("game_config must be a JSON object");
    }

    game::GameConfig config;

    auto primary_timer_ms = ReadOptionalInt(
        object, {"primary_timer_ms", "primaryTimerMs"}, "primary_timer_ms", config.primary_timer_ms);
    if (!primary_timer_ms.ok()) {
        return primary_timer_ms.status();
    }
    config.primary_timer_ms = primary_timer_ms.value();

    auto secondary_timer_ms = ReadOptionalInt(
        object, {"secondary_timer_ms", "secondaryTimerMs"}, "secondary_timer_ms", config.secondary_timer_ms);
    if (!secondary_timer_ms.ok()) {
        return secondary_timer_ms.status();
    }
    config.secondary_timer_ms = secondary_timer_ms.value();

    auto auxiliary_timer_ms = ReadOptionalInt(
        object, {"auxiliary_timer_ms", "auxiliaryTimerMs"}, "auxiliary_timer_ms", config.auxiliary_timer_ms);
    if (!auxiliary_timer_ms.ok()) {
        return auxiliary_timer_ms.status();
    }
    config.auxiliary_timer_ms = auxiliary_timer_ms.value();

    auto immutable_status = RejectImmutableGameConfigField(
        object, {"afk_timeout_times", "afkTimeoutTimes"}, "afk_timeout_times");
    if (!immutable_status.ok()) {
        return immutable_status;
    }

    immutable_status = RejectImmutableGameConfigField(
        object,
        {"minimal_transition_ms", "minimalTransitionMs"},
        "minimal_transition_ms");
    if (!immutable_status.ok()) {
        return immutable_status;
    }

    immutable_status = RejectImmutableGameConfigField(
        object, {"network_delay_ms", "networkDelayMs"}, "network_delay_ms");
    if (!immutable_status.ok()) {
        return immutable_status;
    }

    auto round_count = ReadOptionalInt(
        object, {"round_count", "roundCount"}, "round_count", config.round_count);
    if (!round_count.ok()) {
        return round_count.status();
    }
    config.round_count = round_count.value();

    auto recorded = ReadOptionalBool(object, {"recorded"}, "recorded", config.recorded);
    if (!recorded.ok()) {
        return recorded.status();
    }
    config.recorded = recorded.value();

    auto debug_mode = ReadOptionalBool(object, {"debug_mode", "debugMode"}, "debug_mode", config.debug_mode);
    if (!debug_mode.ok()) {
        return debug_mode.status();
    }
    config.debug_mode = debug_mode.value();
    if (config.debug_mode) {
        config.recorded = false;
    }

    auto unranked = ReadOptionalBool(object, {"unranked"}, "unranked", config.unranked);
    if (!unranked.ok()) {
        return unranked.status();
    }
    config.unranked = unranked.value();
    if (!config.recorded) {
        config.unranked = true;
    }

    auto public_session = ReadOptionalBool(
        object,
        {"public_session", "public", "is_public", "isPublic"},
        "public_session",
        config.public_session);
    if (!public_session.ok()) {
        return public_session.status();
    }
    config.public_session = public_session.value();

    auto abandon_game = ReadOptionalBool(
        object, {"abandon_game", "abandonGame"}, "abandon_game", config.abandon_game);
    if (!abandon_game.ok()) {
        return abandon_game.status();
    }
    config.abandon_game = abandon_game.value();

    auto seat_shuffle_period = ReadOptionalInt(
        object, {"seat_shuffle_period", "seatShufflePeriod"}, "seat_shuffle_period", config.seat_shuffle_period);
    if (!seat_shuffle_period.ok()) {
        return seat_shuffle_period.status();
    }
    config.seat_shuffle_period = seat_shuffle_period.value();

    const Json::Value* forced_end_floor = FindField(object, {"forced_end_floor", "forcedEndFloor"});
    if (forced_end_floor != nullptr && !forced_end_floor->isNull()) {
        if (!forced_end_floor->isInt()) {
            return util::Status::InvalidArgument("forced_end_floor must be null, -1500 or -2000");
        }
        const int floor_value = forced_end_floor->asInt();
        if (floor_value != -1500 && floor_value != -2000) {
            return util::Status::InvalidArgument("forced_end_floor must be null, -1500 or -2000");
        }
        config.forced_end_floor = floor_value;
    }

    auto duplicate_mode = ReadOptionalBool(
        object, {"duplicate_mode", "duplicateMode"}, "duplicate_mode", config.duplicate_mode);
    if (!duplicate_mode.ok()) {
        return duplicate_mode.status();
    }
    config.duplicate_mode = duplicate_mode.value();

    const Json::Value* duplicate_token = FindField(object, {"duplicate_token", "duplicateToken"});
    if (duplicate_token != nullptr && duplicate_token->isString() && !duplicate_token->asString().empty()) {
        config.duplicate_token = duplicate_token->asString();
    }

    if (config.duplicate_mode) {
        if (!config.duplicate_token.has_value()) {
            return util::Status::InvalidArgument("duplicate mode requires duplicate_token");
        }
        if (config.debug_mode) {
            return util::Status::InvalidArgument("debug mode is not allowed with duplicate mode");
        }
        if (config.forced_end_floor.has_value()) {
            return util::Status::InvalidArgument("forced_end_floor is not allowed with duplicate mode");
        }
        if (config.abandon_game) {
            return util::Status::InvalidArgument("abandon_game is not allowed with duplicate mode");
        }
        // Duplicate games always preserve records and never affect rankings.
        config.recorded = true;
        config.unranked = true;
        config.seat_shuffle_period = 4;
    } else if (config.duplicate_token.has_value()) {
        return util::Status::InvalidArgument("duplicate_token requires duplicate mode");
    }

    auto bounds_status = ValidateGameConfigBounds(config);
    if (!bounds_status.ok()) {
        return bounds_status;
    }

    return config;
}

util::StatusOr<game::QueueConfig> ParseQueueConfig(const Json::Value& object) {
    if (!object.isObject()) {
        return util::Status::InvalidArgument("queue_config must be a JSON object");
    }

    game::QueueConfig config;

    auto empty_timeout_ms = ReadOptionalInt(
        object, {"empty_timeout_ms", "emptyTimeoutMs"}, "empty_timeout_ms", config.empty_timeout_ms);
    if (!empty_timeout_ms.ok()) {
        return empty_timeout_ms.status();
    }
    config.empty_timeout_ms = empty_timeout_ms.value();

    auto public_session = ReadOptionalBool(
        object,
        {"public_session", "public", "is_public", "isPublic"},
        "public_session",
        config.public_session);
    if (!public_session.ok()) {
        return public_session.status();
    }
    config.public_session = public_session.value();

    auto singleplayer = ReadOptionalBool(
        object,
        {"singleplayer", "single_player", "singlePlayer"},
        "singleplayer",
        config.singleplayer);
    if (!singleplayer.ok()) {
        return singleplayer.status();
    }
    config.singleplayer = singleplayer.value();

    return config;
}

game::PendingSessionSummary BuildPendingSummary(const game::PendingSession& session) {
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

    return game::PendingSessionSummary{
        .session_id = session.session_id(),
        .occupied_seat_count = occupied_seat_count,
        .ready_seat_count = ready_seat_count,
        .primary_timer_ms = session.game_config().primary_timer_ms,
        .secondary_timer_ms = session.game_config().secondary_timer_ms,
        .auxiliary_timer_ms = session.game_config().auxiliary_timer_ms,
        .round_count = session.game_config().round_count,
        .forced_end_floor = session.game_config().forced_end_floor,
        .recorded = session.game_config().recorded,
        .public_session = session.game_config().public_session,
        .abandon_game = session.game_config().abandon_game,
        .can_join = occupied_seat_count < static_cast<int>(session.seats().size()),
        .can_start = occupied_seat_count == static_cast<int>(session.seats().size()) &&
                     ready_seat_count == static_cast<int>(session.seats().size()),
        .names = std::move(names),
    };
}

Json::Value SerializePendingSummary(const game::PendingSessionSummary& summary) {
    Json::Value payload(Json::objectValue);
    payload["session_id"] = Json::Int64(summary.session_id);
    payload["occupied_seat_count"] = summary.occupied_seat_count;
    payload["ready_seat_count"] = summary.ready_seat_count;
    payload["member_count"] = summary.member_count;
    payload["round_count"] = summary.round_count;
    payload["forced_end_floor"] = summary.forced_end_floor.has_value()
        ? Json::Value(*summary.forced_end_floor)
        : Json::Value(Json::nullValue);
    payload["primary_timer_ms"] = summary.primary_timer_ms;
    payload["secondary_timer_ms"] = summary.secondary_timer_ms;
    payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
    payload["recorded"] = summary.recorded;
    payload["unranked"] = summary.unranked;
    payload["singleplayer"] = summary.singleplayer;
    payload["public_session"] = summary.public_session;
    payload["abandon_game"] = summary.abandon_game;
    payload["duplicate_mode"] = summary.duplicate_mode;
    payload["can_join"] = summary.can_join;
    payload["can_start"] = summary.can_start;
    Json::Value names(Json::arrayValue);
    for (const auto& name : summary.names) {
        names.append(name);
    }
    payload["names"] = std::move(names);
    return payload;
}

Json::Value SerializePendingSummaryList(
    const std::vector<game::PendingSessionSummary>& sessions) {
    Json::Value payload(Json::arrayValue);
    for (const auto& session : sessions) {
        payload.append(SerializePendingSummary(session));
    }
    return payload;
}

Json::Value SerializePendingSeat(const game::PendingSeat& seat) {
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

Json::Value SerializePendingSnapshot(const game::PendingSession& session,
                  const Json::Value& ratings) {
    Json::Value payload(Json::objectValue);
    payload["phase"] = "pending";
    payload["summary"] = SerializePendingSummary(BuildPendingSummary(session));

    Json::Value seats(Json::arrayValue);
    for (const auto& seat : session.seats()) {
        seats.append(SerializePendingSeat(seat));
    }
    payload["seats"] = std::move(seats);
    if (!ratings.isNull()) {
        payload["ratings"] = ratings;
    }
    return payload;
}

bool PendingSessionContainsPlayer(const game::PendingSession& session, std::int64_t player_id) {
    return std::any_of(session.seats().begin(), session.seats().end(), [player_id](const game::PendingSeat& seat) {
        return seat.player.matches(player_id);
    });
}

bool ActiveSessionContainsPlayer(const game::ActiveSession& session, std::int64_t player_id) {
    return std::any_of(session.seats().begin(), session.seats().end(), [player_id](const game::Seat& seat) {
        return seat.player.matches(player_id);
    });
}

bool CanViewPendingSession(const game::PendingSession& session,
                   std::optional<std::int64_t> viewer_player_id) {
    return session.game_config().public_session ||
        (viewer_player_id.has_value() && PendingSessionContainsPlayer(session, *viewer_player_id));
}

bool CanViewActiveSession(const game::ActiveSession& session,
                  std::optional<std::int64_t> viewer_player_id) {
    (void)session;
    (void)viewer_player_id;
    return true;
}

Json::Value BuildGameMessage(std::string_view type, Json::Value payload) {
    Json::Value message(Json::objectValue);
    message["type"] = std::string(type);
    message["payload"] = std::move(payload);
    return message;
}

Json::Value MakeWebSocketEnvelope(std::string_view type,
               Json::Value payload,
               std::string_view request_id) {
    Json::Value envelope(Json::objectValue);
    envelope["version"] = 1;
    envelope["type"] = std::string(type);
    if (request_id.empty()) {
        envelope["requestId"] = Json::Value(Json::nullValue);
    } else {
        envelope["requestId"] = std::string(request_id);
    }
    envelope["payload"] = std::move(payload);
    return envelope;
}

Json::Value MakeWebSocketAck(std::string_view request_id) {
    Json::Value payload(Json::objectValue);
    payload["ok"] = true;
    return MakeWebSocketEnvelope("ack", std::move(payload), request_id);
}

Json::Value MakeWebSocketError(std::string_view code,
                std::string message,
                std::string_view request_id) {
    Json::Value payload(Json::objectValue);
    payload["code"] = std::string(code);
    payload["message"] = std::move(message);
    return MakeWebSocketEnvelope("error", std::move(payload), request_id);
}

util::StatusOr<Json::Value> ParseWebSocketMessage(std::string_view message) {
    Json::CharReaderBuilder builder;
    Json::Value payload;
    JSONCPP_STRING errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const bool parsed =
        reader->parse(message.data(), message.data() + message.size(), &payload, &errors);
    if (!parsed) {
        return util::Status::InvalidArgument(errors.empty() ? "invalid JSON message" : errors);
    }
    if (!payload.isObject()) {
        return util::Status::InvalidArgument("message must be a JSON object");
    }
    return payload;
}

std::filesystem::path AuthMigrationsPath() {
    return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "auth" / "migrations";
}

std::filesystem::path StatsMigrationsPath() {
    return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "stats" / "migrations";
}

std::filesystem::path DuplicateMigrationsPath() {
    return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "duplicate" / "migrations";
}

std::filesystem::path RatingMigrationsPath() {
    return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "ranking" / "migrations";
}

std::string_view WebSocketRoutePath(WebSocketRoute route) {
    switch (route) {
        case WebSocketRoute::kLobby:
            return "/ws/lobby";
        case WebSocketRoute::kGame:
            return "/ws/game";
        case WebSocketRoute::kSpectate:
            return "/ws/spectate";
        case WebSocketRoute::kReplay:
            return "/ws/replay";
    }

    return "/ws/lobby";
}

WebSocketRoute ResolveWebSocketRoute(const drogon::HttpRequestPtr& request) {
    if (request->path() == WebSocketRoutePath(WebSocketRoute::kSpectate)) {
        return WebSocketRoute::kSpectate;
    }
    if (request->path() == WebSocketRoutePath(WebSocketRoute::kReplay)) {
        return WebSocketRoute::kReplay;
    }
    if (request->path() == WebSocketRoutePath(WebSocketRoute::kGame)) {
        return WebSocketRoute::kGame;
    }
    return WebSocketRoute::kLobby;
}

std::optional<std::string> FindMessageTypeString(const Json::Value& message) {
    const Json::Value* type = FindField(message, {"type"});
    if (type == nullptr || !type->isString()) {
        return std::nullopt;
    }
    return type->asString();
}

std::optional<std::string> FindPayloadPhaseString(const Json::Value& message) {
    const Json::Value* payload = FindField(message, {"payload"});
    if (payload == nullptr || !payload->isObject()) {
        return std::nullopt;
    }

    const Json::Value* phase = FindField(*payload, {"phase"});
    if (phase == nullptr || !phase->isString()) {
        return std::nullopt;
    }
    return phase->asString();
}

std::optional<WebSocketRoute> ClassifyInboundMessageRoute(std::string_view type) {
    if (type == "lobby.list") {
        return WebSocketRoute::kLobby;
    }

    if (type == "session.join" || type == "session.leave" || type == "queue.ready" ||
            type == "game.input" || type == "resume.ack" || type == "spectator.hand.revoke") {
        return WebSocketRoute::kGame;
    }

    return std::nullopt;
}

std::optional<WebSocketRoute> ClassifyOutboundMessageRoute(const Json::Value& message) {
    auto type = FindMessageTypeString(message);
    if (!type.has_value()) {
        return std::nullopt;
    }
    if (*type == "spectator.hand.management") {
        return WebSocketRoute::kGame;
    }
    if (type->rfind("spectator.hand.", 0) == 0) {
        return WebSocketRoute::kSpectate;
    }

    if (*type == "session.snapshot") {
        auto phase = FindPayloadPhaseString(message);
        if (phase == "pending" || phase == "active") {
            const Json::Value* payload = FindField(message, {"payload"});
            if (payload != nullptr && payload->isObject() &&
                (*payload)["spectator"].asBool()) {
                return WebSocketRoute::kSpectate;
            }
            return WebSocketRoute::kGame;
        }
    }

    if (*type == "game.event") {
        const Json::Value* payload = FindField(message, {"payload"});
        if (payload != nullptr && payload->isObject() &&
            (*payload)["spectator"].asBool()) {
            return WebSocketRoute::kSpectate;
        }
    }

    if (*type == "lobby.list.snapshot" || type->rfind("lobby.", 0) == 0) {
        return WebSocketRoute::kLobby;
    }

    if (*type == "resume.required" || type->rfind("game.", 0) == 0 ||
        type->rfind("resume.", 0) == 0 || type->rfind("rating.", 0) == 0) {
        return WebSocketRoute::kGame;
    }

    return std::nullopt;
}

void LogDroppedOutboundMessage(std::int64_t player_id, const Json::Value& message) {
    std::cerr << "dropping unclassified websocket message for player_id=" << player_id;
    auto type = FindMessageTypeString(message);
    if (type.has_value()) {
        std::cerr << " type=" << *type;
    }
    std::cerr << '\n';
}

stats::StatsFilter ParseStatsFilterFromJson(const Json::Value& json) {
    stats::StatsFilter filter;

    const Json::Value* fan_positive = FindField(json, {"fan_filter_positive"});
    if (fan_positive != nullptr && fan_positive->isArray()) {
        for (const auto& fan : *fan_positive) {
            if (fan.isInt()) {
                filter.fan_filter_positive.push_back(fan.asInt());
            }
        }
    }

    const Json::Value* fan_negative = FindField(json, {"fan_filter_negative"});
    if (fan_negative != nullptr && fan_negative->isArray()) {
        for (const auto& fan : *fan_negative) {
            if (fan.isInt()) {
                filter.fan_filter_negative.push_back(fan.asInt());
            }
        }
    }

    const Json::Value* player_positive = FindField(json, {"player_filter_positive"});
    if (player_positive != nullptr && player_positive->isArray()) {
        for (const auto& player_id : *player_positive) {
            if (player_id.isInt64()) {
                filter.player_filter_positive.push_back(player_id.asInt64());
            } else if (player_id.isInt()) {
                filter.player_filter_positive.push_back(player_id.asInt());
            }
        }
    }

    const Json::Value* player_negative = FindField(json, {"player_filter_negative"});
    if (player_negative != nullptr && player_negative->isArray()) {
        for (const auto& player_id : *player_negative) {
            if (player_id.isInt64()) {
                filter.player_filter_negative.push_back(player_id.asInt64());
            } else if (player_id.isInt()) {
                filter.player_filter_negative.push_back(player_id.asInt());
            }
        }
    }

    if (const Json::Value* player_name = FindField(json, {"player_name"}); player_name != nullptr) {
        if (player_name->isString() && !player_name->asString().empty()) {
            filter.player_id = std::stoll(player_name->asString());
        } else if (player_name->isInt64()) {
            filter.player_id = player_name->asInt64();
        } else if (player_name->isInt()) {
            filter.player_id = player_name->asInt();
        }
    }

    if (const Json::Value* win_player = FindField(json, {"win_player_filter_positive"}); win_player != nullptr) {
        if (win_player->isString() && !win_player->asString().empty()) {
            try { filter.win_player_id = std::stoll(win_player->asString()); } catch (...) {}
        } else if (win_player->isInt64()) {
            filter.win_player_id = win_player->asInt64();
        } else if (win_player->isInt()) {
            filter.win_player_id = win_player->asInt();
        }
    }

    if (const Json::Value* from_player = FindField(json, {"shoot_player_filter_positive"}); from_player != nullptr) {
        if (from_player->isString() && !from_player->asString().empty()) {
            try { filter.from_player_id = std::stoll(from_player->asString()); } catch (...) {}
        } else if (from_player->isInt64()) {
            filter.from_player_id = from_player->asInt64();
        } else if (from_player->isInt()) {
            filter.from_player_id = from_player->asInt();
        }
    }

    if (const Json::Value* self_drawn_positive = FindField(json, {"self_drawn_filter_positive"}); self_drawn_positive != nullptr) {
        if (self_drawn_positive->isBool() && self_drawn_positive->asBool()) {
            filter.self_drawn = true;
        }
    }
    if (const Json::Value* self_drawn_negative = FindField(json, {"self_drawn_filter_negative"}); self_drawn_negative != nullptr) {
        if (self_drawn_negative->isBool() && self_drawn_negative->asBool()) {
            filter.self_drawn = false;
        }
    }

    if (const Json::Value* min_fan = FindField(json, {"min_fan"}); min_fan != nullptr && min_fan->isDouble()) {
        filter.min_fan = min_fan->asDouble();
    }
    if (const Json::Value* max_fan = FindField(json, {"max_fan"}); max_fan != nullptr && max_fan->isDouble()) {
        filter.max_fan = max_fan->asDouble();
    }

    if (const Json::Value* time_start = FindField(json, {"time_start"}); time_start != nullptr && time_start->isInt64()) {
        filter.time_start = time_start->asInt64();
    }
    if (const Json::Value* time_end = FindField(json, {"time_end"}); time_end != nullptr && time_end->isInt64()) {
        filter.time_end = time_end->asInt64();
    }

    if (const Json::Value* exclude_superior = FindField(json, {"exclude_superior_fans"}); exclude_superior != nullptr && exclude_superior->isBool()) {
        filter.exclude_superior_fans = exclude_superior->asBool();
    }
    int mode = 0;
    if (const Json::Value* mode_value = FindField(json, {"mode"}); mode_value != nullptr) {
        if (mode_value->isInt()) {
            mode = mode_value->asInt();
        } else if (mode_value->isUInt()) {
            mode = static_cast<int>(mode_value->asUInt());
        }
    }
    // Legacy: the old 休闲模式 switch mapped onto unranked-only filtering.
    if (const Json::Value* nonstandard_only = FindField(json, {"nonstandard_only"}); nonstandard_only != nullptr && nonstandard_only->isBool() && nonstandard_only->asBool()) {
        mode = 1;
    }
    filter.mode_filter = (mode >= -1 && mode <= 2) ? mode : 0;

    return filter;
}

std::string StatsRecordSortKey(std::string_view sort_field, std::string_view sort_order) {
    const bool fan_sort = sort_field == "fan";
    const bool asc = sort_order == "asc";
    return std::string(fan_sort ? "fan" : "time") + ":" + (asc ? "asc" : "desc");
}

StatsRecordsRequest ParseStatsRecordsRequest(const Json::Value& root) {
    StatsRecordsRequest request;
    const Json::Value* sort_field_value = FindField(root, {"sort_field"});
    if (sort_field_value != nullptr && sort_field_value->isString()) {
        request.sort_field = sort_field_value->asString();
    }
    const Json::Value* sort_order_value = FindField(root, {"sort_order"});
    if (sort_order_value != nullptr && sort_order_value->isString()) {
        request.sort_order = sort_order_value->asString();
    }
    const Json::Value* offset_value = FindField(root, {"offset"});
    if (offset_value != nullptr) {
        if (offset_value->isUInt64()) {
            request.offset = static_cast<std::size_t>(offset_value->asUInt64());
        } else if (offset_value->isInt() && offset_value->asInt() >= 0) {
            request.offset = static_cast<std::size_t>(offset_value->asInt());
        }
    }
    const Json::Value* limit_value = FindField(root, {"limit"});
    if (limit_value != nullptr) {
        if (limit_value->isUInt64()) {
            request.limit = static_cast<std::size_t>(limit_value->asUInt64());
        } else if (limit_value->isInt() && limit_value->asInt() > 0) {
            request.limit = static_cast<std::size_t>(limit_value->asInt());
        }
    }
    return request;
}

std::vector<const stats::RoundEntry*> BuildFanSortedStatsRounds(
    const std::vector<const stats::RoundEntry*>& time_desc_rounds,
    bool descending) {
    struct FanBucket {
        double fan{0.0};
        std::vector<const stats::RoundEntry*> rounds_time_desc;
    };

    std::unordered_map<double, std::size_t> bucket_indices;
    bucket_indices.reserve(time_desc_rounds.size());
    std::vector<FanBucket> buckets;
    buckets.reserve(time_desc_rounds.size());
    for (const auto* round : time_desc_rounds) {
        const double fan = round == nullptr ? 0.0 : round->fan;
        auto [index_it, inserted] = bucket_indices.emplace(fan, buckets.size());
        if (inserted) {
            buckets.push_back(FanBucket{.fan = fan});
        }
        auto& bucket = buckets[index_it->second];
        bucket.rounds_time_desc.push_back(round);
    }

    std::sort(buckets.begin(), buckets.end(), [descending](const FanBucket& left, const FanBucket& right) {
        return descending ? (left.fan > right.fan) : (left.fan < right.fan);
    });

    std::vector<const stats::RoundEntry*> sorted;
    sorted.reserve(time_desc_rounds.size());
    for (auto& bucket : buckets) {
        if (descending) {
            sorted.insert(sorted.end(), bucket.rounds_time_desc.begin(), bucket.rounds_time_desc.end());
        } else {
            sorted.insert(sorted.end(), bucket.rounds_time_desc.rbegin(), bucket.rounds_time_desc.rend());
        }
    }
    return sorted;
}

std::vector<const stats::RoundEntry*> BuildStatsRecordOrder(
    const std::vector<const stats::RoundEntry*>& time_desc_rounds,
    std::string_view sort_field,
    std::string_view sort_order) {
    const bool descending = sort_order != "asc";
    if (sort_field == "fan") {
        return BuildFanSortedStatsRounds(time_desc_rounds, descending);
    }

    auto sorted = time_desc_rounds;
    if (!descending) {
        std::reverse(sorted.begin(), sorted.end());
    }
    return sorted;
}

std::string SerializeStatsRoundEntriesPayload(
    const std::vector<const stats::RoundEntry*>& entries_to_emit,
    std::size_t total,
    std::size_t offset,
    std::size_t limit) {
    Json::Value resp(Json::objectValue);
    resp["type"] = "records";
    resp["total"] = static_cast<Json::UInt64>(total);
    resp["offset"] = static_cast<Json::UInt64>(offset);
    resp["limit"] = static_cast<Json::UInt64>(limit);

    Json::Value entries(Json::arrayValue);
    for (const auto* round_entry : entries_to_emit) {
        Json::Value entry(Json::objectValue);
        entry["game_folder"] = round_entry->round_key.session_identifier;
        entry["game_index"] = static_cast<int>(round_entry->round_key.round_number);
        if (!round_entry->duplicate_token.empty()) {
            entry["display_folder"] = duplicate::TokenDisplayPrefix(round_entry->duplicate_token) +
                "." + std::to_string(round_entry->duplicate_session_number);
        }
        entry["drawn_game"] = round_entry->drawn_game;
        entry["winner"] = static_cast<int>(round_entry->winner_player_id());
        entry["from"] = static_cast<int>(round_entry->from_player_id());
        entry["fan"] = round_entry->fan;
        entry["time"] = Json::Int64(round_entry->timestamp_ms);
        entry["turn"] = Json::Int64(round_entry->turn);

        Json::Value all_players(Json::arrayValue);
        for (const auto& player : round_entry->players) {
            Json::Value player_obj(Json::objectValue);
            player_obj["player_id"] = Json::Int64(player.player_id);
            player_obj["username"] = player.username;
            all_players.append(std::move(player_obj));
        }
        entry["all_players"] = std::move(all_players);

        if (!round_entry->fan_results.empty()) {
            auto res0 = round_entry->fan_results.front();
            res0 = qingque::dedupe(res0);
            std::string fans_readable;
            bool first = true;
            for (std::size_t j = 0; j < qingque::fans.size(); ++j) {
                if (res0.test(j)) {
                    if (!first) {
                        fans_readable += ", ";
                    }
                    fans_readable += qingque::fans[j].name;
                    first = false;
                }
            }
            entry["fans_str"] = std::move(fans_readable);
        }
        entries.append(std::move(entry));
    }
    resp["round_entries"] = std::move(entries);

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, resp);
}

std::string SerializeStatsRoundEntriesPage(
    const std::vector<const stats::RoundEntry*>& sorted,
    std::size_t offset,
    std::size_t limit) {
    const std::size_t total = sorted.size();
    if (offset > total) {
        offset = total;
    }
    const std::size_t end = std::min(total, offset + limit);
    std::vector<const stats::RoundEntry*> page;
    page.assign(sorted.begin() + static_cast<std::ptrdiff_t>(offset),
        sorted.begin() + static_cast<std::ptrdiff_t>(end));
    return SerializeStatsRoundEntriesPayload(page, total, offset, limit);
}

std::optional<std::int64_t> ResolveLoggedPlayerId(
    const drogon::WebSocketConnectionPtr& connection,
    std::optional<std::int64_t> player_id_override) {
    if (player_id_override.has_value()) {
        return player_id_override;
    }

    auto context = connection->getContext<GameClientContext>();
    if (!context) {
        return std::nullopt;
    }

    const auto player_id = context->player_id();
    if (player_id == 0) {
        return std::nullopt;
    }
    return player_id;
}

std::optional<WebSocketRoute> ResolveLoggedRoute(
    const drogon::WebSocketConnectionPtr& connection,
    std::optional<WebSocketRoute> route_override) {
    if (route_override.has_value()) {
        return route_override;
    }

    auto context = connection->getContext<GameClientContext>();
    if (!context) {
        return std::nullopt;
    }
    return context->route();
}

void LogInboundWebSocketMessage(DebugTrafficLogger* logger,
                const drogon::WebSocketConnectionPtr& connection,
                std::string_view message,
                const drogon::WebSocketMessageType& type) {
    if (logger == nullptr) {
        return;
    }

    Json::Value entry(Json::objectValue);
    entry["transport"] = "websocket";
    entry["direction"] = "in";
    entry["kind"] = "message";
    entry["connection_handle"] = PointerHandleString(connection.get());
    if (const auto player_id = ResolveLoggedPlayerId(connection, std::nullopt); player_id.has_value()) {
        entry["player_id"] = Json::Int64(*player_id);
    } else {
        entry["player_id"] = Json::Value(Json::nullValue);
    }
    if (const auto route = ResolveLoggedRoute(connection, std::nullopt); route.has_value()) {
        entry["route"] = std::string(WebSocketRoutePath(*route));
    } else {
        entry["route"] = Json::Value(Json::nullValue);
    }
    entry["frame_type"] = type == drogon::WebSocketMessageType::Text ? "text" : "non_text";
    entry["message"] = type == drogon::WebSocketMessageType::Text
        ? SanitizeTextPayload(message)
        : Json::Value(std::string(message));
    logger->Log(std::move(entry));
}

void LogOutboundWebSocketMessage(DebugTrafficLogger* logger,
                 const drogon::WebSocketConnectionPtr& connection,
                 const Json::Value& message,
                 int delay_ms,
                 std::optional<std::int64_t> player_id,
                 std::optional<WebSocketRoute> route) {
    if (logger == nullptr) {
        return;
    }

    Json::Value entry(Json::objectValue);
    entry["transport"] = "websocket";
    entry["direction"] = "out";
    entry["kind"] = "message";
    entry["connection_handle"] = PointerHandleString(connection.get());
    if (const auto resolved_player_id = ResolveLoggedPlayerId(connection, player_id);
        resolved_player_id.has_value()) {
        entry["player_id"] = Json::Int64(*resolved_player_id);
    } else {
        entry["player_id"] = Json::Value(Json::nullValue);
    }
    if (const auto resolved_route = ResolveLoggedRoute(connection, route);
        resolved_route.has_value()) {
        entry["route"] = std::string(WebSocketRoutePath(*resolved_route));
    } else {
        entry["route"] = Json::Value(Json::nullValue);
    }
    if (delay_ms > 0) {
        entry["delay_ms"] = delay_ms;
    }
    if (auto message_type = FindMessageTypeString(message); message_type.has_value()) {
        entry["message_type"] = *message_type;
    }
    entry["message"] = RedactSensitiveJson(message);
    logger->Log(std::move(entry));
}

void SendLoggedWebSocketJson(DebugTrafficLogger* logger,
                 const drogon::WebSocketConnectionPtr& connection,
                 const Json::Value& message,
                 int delay_ms,
                 std::optional<std::int64_t> player_id,
                 std::optional<WebSocketRoute> route) {
    if (delay_ms <= 0) {
        if (!connection->connected()) {
            return;
        }
        LogOutboundWebSocketMessage(logger, connection, message, delay_ms, player_id, route);
        connection->sendJson(message);
        return;
    }

    auto* loop = drogon::app().getLoop();
    if (loop == nullptr) {
        if (!connection->connected()) {
            return;
        }
        LogOutboundWebSocketMessage(logger, connection, message, delay_ms, player_id, route);
        connection->sendJson(message);
        return;
    }

    std::weak_ptr<drogon::WebSocketConnection> weak_connection(connection);
    loop->runAfter(
        std::chrono::duration<double>(static_cast<double>(delay_ms) / 1000.0),
        [logger,
         weak_connection,
         delayed_message = Json::Value(message),
         delay_ms,
         player_id,
         route]() mutable {
            if (auto live_connection = weak_connection.lock()) {
                if (!live_connection->connected()) {
                    return;
                }
                LogOutboundWebSocketMessage(
                    logger, live_connection, delayed_message, delay_ms, player_id, route);
                live_connection->sendJson(delayed_message);
            }
        });
}

util::StatusOr<auth::AuthenticatedSession> AuthenticateRequest(
    ServerState& state, const drogon::HttpRequestPtr& request) {
    auto token = ExtractBearerToken(request);
    if (!token.ok()) {
        return token.status();
    }

    return state.Authenticate(token.value(), CurrentUnixTimeMs());
}

util::StatusOr<auth::AuthenticatedSession> AuthenticateWebSocketRequest(
    ServerState& state, const drogon::HttpRequestPtr& request) {
    auto token = ExtractWebSocketToken(request);
    if (!token.ok()) {
        return token.status();
    }

    return state.Authenticate(token.value(), CurrentUnixTimeMs());
}

util::StatusOr<std::optional<auth::AuthenticatedSession>> AuthenticateOptionalWebSocketRequest(
    ServerState& state, const drogon::HttpRequestPtr& request) {
    std::string authorization = request->getHeader("authorization");
    if (authorization.empty()) {
        authorization = request->getHeader("Authorization");
    }
    std::string query_token = request->getParameter("access_token");
    if (query_token.empty()) {
        query_token = request->getParameter("token");
    }
    if (authorization.empty() && query_token.empty()) {
        return std::optional<auth::AuthenticatedSession>{};
    }

    auto authenticated = AuthenticateWebSocketRequest(state, request);
    if (!authenticated.ok()) {
        return authenticated.status();
    }

    return std::optional<auth::AuthenticatedSession>(authenticated.value());
}

util::StatusOr<std::optional<auth::AuthenticatedSession>> AuthenticateOptionalRequest(
    ServerState& state, const drogon::HttpRequestPtr& request) {
    std::string authorization = request->getHeader("authorization");
    if (authorization.empty()) {
        authorization = request->getHeader("Authorization");
    }
    if (authorization.empty()) {
        return std::optional<auth::AuthenticatedSession>{};
    }

    auto authenticated = AuthenticateRequest(state, request);
    if (!authenticated.ok()) {
        return authenticated.status();
    }

    return std::optional<auth::AuthenticatedSession>(authenticated.value());
}

Json::Value BuildLobbyListPayload(const ServerState& state,
               std::optional<std::int64_t> viewer_player_id) {
    Json::Value payload(Json::objectValue);
    payload["sessions"] = SerializePendingSummaryList(state.ListVisibleJoinableSessions(viewer_player_id));
    payload["active_sessions"] = SerializeActiveSummaryList(state.ListVisibleActiveSessions(viewer_player_id));
    return payload;
}

util::StatusOr<Json::Value> BuildSessionSnapshotPayload(
    ServerState& state,
    std::int64_t session_id,
    std::optional<std::int64_t> viewer_player_id) {
    auto pending_session = state.FindPendingSession(session_id);
    if (pending_session.ok()) {
        if (!CanViewPendingSession(*pending_session.value(), viewer_player_id)) {
            return util::Status::NotFound("session not found");
        }
        std::array<std::int64_t, 4> player_ids{};
        const auto& seats = pending_session.value()->seats();
        for (std::size_t i = 0; i < seats.size(); ++i) {
            const auto player = seats[i].player.lock();
            player_ids[i] = (player != nullptr) ? player->player_id : 0;
        }
        auto ratings_json = state.get_player_ratings(player_ids);
        Json::Value ratings_arr(Json::arrayValue);
        for (const auto& r : ratings_json) {
            ratings_arr.append(r.ToJson());
        }
        return SerializePendingSnapshot(*pending_session.value(), ratings_arr);
    }
    if (pending_session.status().code() != util::StatusCode::kNotFound) {
        return pending_session.status();
    }

    auto active_session = state.FindActiveSession(session_id);
    if (!active_session.ok()) {
        return active_session.status();
    }
    if (!CanViewActiveSession(*active_session.value(), viewer_player_id)) {
        return util::Status::NotFound("session not found");
    }
    if (viewer_player_id.has_value() &&
        ActiveSessionContainsPlayer(*active_session.value(), *viewer_player_id)) {
        return active_session.value()->build_snapshot_for_player_id(*viewer_player_id);
    }
    return active_session.value()->build_snapshot_for_spectator();
}

util::StatusOr<Json::Value> BuildCreatedSessionSnapshotPayload(
    ServerState& state,
    std::int64_t session_id,
    std::int64_t owner_player_id) {
    auto pending_session = state.FindPendingSession(session_id);
    if (pending_session.ok()) {
        std::array<std::int64_t, 4> player_ids{};
        const auto& seats = pending_session.value()->seats();
        for (std::size_t i = 0; i < seats.size(); ++i) {
            const auto player = seats[i].player.lock();
            player_ids[i] = (player != nullptr) ? player->player_id : 0;
        }
        auto ratings_json = state.get_player_ratings(player_ids);
        Json::Value ratings_arr(Json::arrayValue);
        for (const auto& r : ratings_json) {
            ratings_arr.append(r.ToJson());
        }
        return SerializePendingSnapshot(*pending_session.value(), ratings_arr);
    }
    if (pending_session.status().code() != util::StatusCode::kNotFound) {
        return pending_session.status();
    }

    return BuildSessionSnapshotPayload(
        state,
        session_id,
        std::optional<std::int64_t>(owner_player_id));
}

util::StatusOr<Json::Value> BuildReplaySessionPayload(
    const ServerState& state,
    std::string_view session_identifier) {
    auto round_records = state.LoadReplaySession(session_identifier);
    if (!round_records.ok()) {
        return round_records.status();
    }

    auto replays = state.ListReplays();
    if (!replays.ok()) {
        return replays.status();
    }

    const auto it = std::find_if(
        replays.value().begin(),
        replays.value().end(),
        [session_identifier](const replay::ReplayInfo& replay_info) {
            return replay_info.session_identifier == session_identifier;
        });
    if (it == replays.value().end()) {
        return util::Status::NotFound("replay session not found");
    }

    Json::Value payload(Json::objectValue);
    payload["session_identifier"] = std::string(session_identifier);
    payload["round_count"] = Json::UInt64(it->round_count);
    Json::Value player_names(Json::arrayValue);
    for (const auto& name : it->player_names) {
        player_names.append(name);
    }
    payload["player_names"] = std::move(player_names);
    Json::Value round_records_payload(Json::arrayValue);
    for (auto& round_record : round_records.value()) {
        AttachReplayWallState(round_record);
        NormalizeReplaySeedFields(round_record);
        round_records_payload.append(std::move(round_record));
    }
    payload["round_records"] = std::move(round_records_payload);
    return payload;
}

util::StatusOr<std::string> ReadOptionalStringField(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr || value->isNull()) {
        return std::string();
    }
    if (!value->isString()) {
        return util::Status::InvalidArgument(std::string(label) + " must be a string");
    }
    const auto raw_value = value->asString();
    return std::string(TrimString(raw_value));
}

util::StatusOr<std::optional<std::int64_t>> ReadOptionalInt64Field(
    const Json::Value& object,
    std::initializer_list<std::string_view> names,
    std::string_view label) {
    const Json::Value* value = FindField(object, names);
    if (value == nullptr || value->isNull()) {
        return std::optional<std::int64_t>{};
    }

    std::int64_t parsed_value = 0;
    if (value->isInt() || value->isInt64()) {
        parsed_value = value->asInt64();
    } else if (value->isUInt() || value->isUInt64()) {
        const auto unsigned_value = value->asUInt64();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return util::Status::InvalidArgument(std::string(label) + " is out of range");
        }
        parsed_value = static_cast<std::int64_t>(unsigned_value);
    } else {
        return util::Status::InvalidArgument(std::string(label) + " must be an integer");
    }

    return std::optional<std::int64_t>(parsed_value);
}

std::string ToLowerCopy(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const unsigned char character : value) {
        lowered.push_back(static_cast<char>(std::tolower(character)));
    }
    return lowered;
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto lowered_haystack = ToLowerCopy(haystack);
    const auto lowered_needle = ToLowerCopy(needle);
    return lowered_haystack.find(lowered_needle) != std::string::npos;
}

bool EqualsCaseInsensitiveText(std::string_view left, std::string_view right) {
    return ToLowerCopy(left) == ToLowerCopy(right);
}

}  // namespace mmcr::server
