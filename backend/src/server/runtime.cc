#include "server/runtime.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>
#include <jsoncpp/json/json.h>

#include "external/qingque/rules/qingque.h"
#include "external/qingque/rules/w_data.h"
#include "auth/service.h"
#include "game/engine/session.h"
#include "game/hub/hub.h"
#include "random/seed.h"
#include "ranking/service.h"
#include "replay/manager.h"
#include "stats/service.h"
#include "storage/database.h"
#include "storage/game_record.h"
#include "util/build_info.h"

namespace mmcr::server {
namespace {

constexpr std::string_view kDefaultBindAddress = "0.0.0.0";
constexpr std::uint16_t kDefaultPort = 8080;
constexpr std::size_t kDefaultThreadCount = 0;
constexpr std::string_view kDefaultDatabasePath = "mmcr_backend.sqlite3";
constexpr std::string_view kDefaultDebugLogDir = "";
constexpr std::string_view kDefaultCorsAllowHeaders = "Authorization, Content-Type";
constexpr std::string_view kDefaultCorsAllowMethods = "GET, POST, OPTIONS";
constexpr double kLargeHandCutoff = 18.495;
constexpr std::size_t kBigWinsMaxCount = 10;

auto ResolveWorkerThreadCount(std::size_t configured_thread_count) -> std::size_t {
	if (configured_thread_count != 0) {
		return configured_thread_count;
	}

	const auto hardware_threads = std::thread::hardware_concurrency();
	if (hardware_threads == 0) {
		return 1;
	}

	return static_cast<std::size_t>(hardware_threads);
}

auto ResolveDatabasePoolSize(std::size_t configured_thread_count) -> std::size_t {
	const auto worker_threads = ResolveWorkerThreadCount(configured_thread_count);
	return std::max<std::size_t>(worker_threads + 2, 4);
}

auto CurrentUnixTimeMs() -> std::int64_t {
	return static_cast<std::int64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

auto JsonToCompactString(const Json::Value& value) -> std::string {
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["commentStyle"] = "None";
	return Json::writeString(builder, value);
}

auto PointerHandleString(const void* pointer) -> std::string {
	return std::to_string(reinterpret_cast<std::uintptr_t>(pointer));
}

auto IsSensitiveFieldName(std::string_view name) -> bool {
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

auto RedactSensitiveJson(const Json::Value& value) -> Json::Value {
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

auto ParseJsonText(std::string_view raw_text) -> std::optional<Json::Value> {
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

auto SanitizeTextPayload(std::string_view raw_text) -> Json::Value {
	if (raw_text.empty()) {
		return Json::Value(Json::nullValue);
	}

	auto parsed = ParseJsonText(raw_text);
	if (!parsed.has_value()) {
		return Json::Value(std::string(raw_text));
	}

	return RedactSensitiveJson(*parsed);
}

auto SanitizeQueryString(std::string_view raw_query) -> std::string {
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

class DebugTrafficLogger {
public:
	[[nodiscard]] auto Initialize(const std::filesystem::path& dir) -> util::Status {
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

	void Log(Json::Value entry) {
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

	[[nodiscard]] auto dir() const -> const std::filesystem::path& {
		return dir_;
	}

private:
	static auto TodayUtcDate() -> std::string {
		const auto now = std::chrono::system_clock::now();
		const auto time_t_now = std::chrono::system_clock::to_time_t(now);
		std::tm utc{};
		gmtime_r(&time_t_now, &utc);
		std::ostringstream oss;
		oss << std::put_time(&utc, "%Y-%m-%d");
		return oss.str();
	}

	[[nodiscard]] auto RotateFile() -> util::Status {
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

	std::filesystem::path dir_;
	std::string current_log_date_;
	std::mutex mutex_;
	std::ofstream stream_;
};

auto BuildHttpRequestLogEntry(const drogon::HttpRequestPtr& request) -> Json::Value {
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

auto BuildHttpResponseLogEntry(const drogon::HttpRequestPtr& request,
					   const drogon::HttpResponsePtr& response) -> Json::Value {
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

auto TrimString(std::string_view value) -> std::string_view {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
		value.remove_prefix(1);
	}
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
		value.remove_suffix(1);
	}
	return value;
}

auto EqualsCaseInsensitive(std::string_view left, std::string_view right) -> bool {
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

auto ParseUnsignedInteger(std::string_view raw_value,
						  std::string_view variable_name) -> util::StatusOr<std::uint64_t> {
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

auto ParseInt64String(std::string_view raw_value,
					  std::string_view label) -> util::StatusOr<std::int64_t> {
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

auto ResolveCorsAllowOrigin() -> std::optional<std::string> {
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

auto StatusCodeName(util::StatusCode code) -> std::string_view {
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

auto ToHttpStatus(util::StatusCode code) -> drogon::HttpStatusCode {
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

auto NewJsonResponse(Json::Value payload,
				 drogon::HttpStatusCode status_code = drogon::k200OK)
	-> drogon::HttpResponsePtr {
	auto response = drogon::HttpResponse::newHttpJsonResponse(std::move(payload));
	response->setStatusCode(status_code);
	ApplyCorsHeaders(response);
	return response;
}

auto NewErrorResponse(drogon::HttpStatusCode status_code,
				  std::string_view code,
				  std::string message) -> drogon::HttpResponsePtr {
	Json::Value payload(Json::objectValue);
	Json::Value error(Json::objectValue);
	error["code"] = std::string(code);
	error["message"] = std::move(message);
	payload["error"] = std::move(error);
	return NewJsonResponse(std::move(payload), status_code);
}

auto NewStatusErrorResponse(const util::Status& status) -> drogon::HttpResponsePtr {
	return NewErrorResponse(
		ToHttpStatus(status.code()),
		StatusCodeName(status.code()),
		status.message());
}

auto NewUnauthorizedResponse(std::string_view message) -> drogon::HttpResponsePtr {
	auto response = NewErrorResponse(drogon::k401Unauthorized, "unauthorized", std::string(message));
	response->addHeader("WWW-Authenticate", "Bearer");
	return response;
}

auto NewNoContentResponse() -> drogon::HttpResponsePtr {
	auto response = drogon::HttpResponse::newHttpResponse();
	response->setStatusCode(drogon::k204NoContent);
	ApplyCorsHeaders(response);
	return response;
}

auto NewOkResponse() -> drogon::HttpResponsePtr {
	Json::Value payload(Json::objectValue);
	payload["ok"] = true;
	return NewJsonResponse(std::move(payload));
}

auto NewTextResponse(std::string payload,
			     drogon::HttpStatusCode status_code = drogon::k200OK)
	-> drogon::HttpResponsePtr {
	auto response = drogon::HttpResponse::newHttpResponse();
	response->setStatusCode(status_code);
	response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
	response->setBody(std::move(payload));
	ApplyCorsHeaders(response);
	return response;
}

auto EvaluateCalculatorExpression(std::string_view expression) -> std::string {
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
		const auto readable_fan_code = qingque::derepellenise(fan_code);

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

auto FindField(const Json::Value& object,
		   std::initializer_list<std::string_view> names) -> const Json::Value* {
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

auto ParseJsonBody(const drogon::HttpRequestPtr& request)
	-> util::StatusOr<std::shared_ptr<Json::Value>> {
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

auto ReadRequiredString(const Json::Value& object,
				std::initializer_list<std::string_view> names,
				std::string_view label) -> util::StatusOr<std::string> {
	const Json::Value* value = FindField(object, names);
	if (value == nullptr || !value->isString()) {
		return util::Status::InvalidArgument(std::string(label) + " must be a string");
	}
	return value->asString();
}

auto ReadRequiredBool(const Json::Value& object,
			  std::initializer_list<std::string_view> names,
			  std::string_view label) -> util::StatusOr<bool> {
	const Json::Value* value = FindField(object, names);
	if (value == nullptr || !value->isBool()) {
		return util::Status::InvalidArgument(std::string(label) + " must be a boolean");
	}
	return value->asBool();
}

auto ReadOptionalBool(const Json::Value& object,
			  std::initializer_list<std::string_view> names,
			  std::string_view label,
			  bool default_value) -> util::StatusOr<bool> {
	const Json::Value* value = FindField(object, names);
	if (value == nullptr) {
		return default_value;
	}
	if (!value->isBool()) {
		return util::Status::InvalidArgument(std::string(label) + " must be a boolean");
	}
	return value->asBool();
}

auto ReadOptionalInt(const Json::Value& object,
			 std::initializer_list<std::string_view> names,
			 std::string_view label,
			 int default_value) -> util::StatusOr<int> {
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

auto ExtractBearerToken(const drogon::HttpRequestPtr& request) -> util::StatusOr<std::string> {
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

auto ExtractWebSocketToken(const drogon::HttpRequestPtr& request) -> util::StatusOr<std::string> {
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

auto ExtractWebSocketSessionId(const drogon::HttpRequestPtr& request)
	-> util::StatusOr<std::optional<std::int64_t>> {
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

auto SerializePlayer(const auth::PlayerProfile& player) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["player_id"] = Json::Int64(player.player_id);
	payload["username"] = player.username;
	return payload;
}

auto SerializeSession(const auth::SessionInfo& session) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["session_id"] = session.session_id;
	payload["token"] = session.token;
	payload["created_at_ms"] = Json::Int64(session.created_at_ms);
	payload["expires_at_ms"] = Json::Int64(session.expires_at_ms);
	return payload;
}

auto SerializeAuthenticatedSession(const auth::AuthenticatedSession& session) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["player"] = SerializePlayer(session.player);
	payload["session"] = SerializeSession(session.session);
	return payload;
}

auto SerializeActiveSummary(const game::ActiveSessionSummary& summary) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["session_id"] = Json::Int64(summary.session_id);
	payload["primary_timer_ms"] = summary.primary_timer_ms;
	payload["secondary_timer_ms"] = summary.secondary_timer_ms;
	payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
	payload["round_count"] = summary.round_count;
	payload["round_counter"] = Json::UInt64(summary.round_counter);
	payload["recorded"] = summary.recorded;
	payload["ended"] = summary.ended;
	payload["public_session"] = summary.public_session;
	Json::Value names(Json::arrayValue);
	for (const auto& name : summary.names) {
		names.append(name);
	}
	payload["names"] = std::move(names);
	return payload;
}

auto SerializeActiveSummaryList(const std::vector<game::ActiveSessionSummary>& sessions)
	-> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto& session : sessions) {
		payload.append(SerializeActiveSummary(session));
	}
	return payload;
}

auto SerializeReplayInfo(const replay::ReplayInfo& replay_info) -> Json::Value {
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

auto SerializeReplayInfoList(const std::vector<replay::ReplayInfo>& sessions) -> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto& session : sessions) {
		payload.append(SerializeReplayInfo(session));
	}
	return payload;
}

auto FormatHexSeed(std::uint64_t value) -> std::string {
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

auto RejectImmutableGameConfigField(const Json::Value& object,
						std::initializer_list<std::string_view> names,
						std::string_view label) -> util::Status {
	const Json::Value* value = FindField(object, names);
	if (value == nullptr || value->isNull()) {
		return util::Status::Ok();
	}

	return util::Status::InvalidArgument(
		std::string(label) + " is fixed by the server and cannot be overridden");
}

auto ValidateGameConfigBounds(const game::GameConfig& config) -> util::Status {
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

auto ParseGameConfig(const Json::Value& object) -> util::StatusOr<game::GameConfig> {
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

	auto seat_shuffle_period = ReadOptionalInt(
		object, {"seat_shuffle_period", "seatShufflePeriod"}, "seat_shuffle_period", config.seat_shuffle_period);
	if (!seat_shuffle_period.ok()) {
		return seat_shuffle_period.status();
	}
	config.seat_shuffle_period = seat_shuffle_period.value();

	auto bounds_status = ValidateGameConfigBounds(config);
	if (!bounds_status.ok()) {
		return bounds_status;
	}

	return config;
}

auto ParseQueueConfig(const Json::Value& object) -> util::StatusOr<game::QueueConfig> {
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

auto BuildPendingSummary(const game::PendingSession& session) -> game::PendingSessionSummary {
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
		.recorded = session.game_config().recorded,
		.public_session = session.game_config().public_session,
		.can_join = occupied_seat_count < static_cast<int>(session.seats().size()),
		.can_start = occupied_seat_count == static_cast<int>(session.seats().size()) &&
					 ready_seat_count == static_cast<int>(session.seats().size()),
		.names = std::move(names),
	};
}

auto SerializePendingSummary(const game::PendingSessionSummary& summary) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["session_id"] = Json::Int64(summary.session_id);
	payload["occupied_seat_count"] = summary.occupied_seat_count;
	payload["ready_seat_count"] = summary.ready_seat_count;
	payload["round_count"] = summary.round_count;
	payload["primary_timer_ms"] = summary.primary_timer_ms;
	payload["secondary_timer_ms"] = summary.secondary_timer_ms;
	payload["auxiliary_timer_ms"] = summary.auxiliary_timer_ms;
	payload["recorded"] = summary.recorded;
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

auto SerializePendingSummaryList(const std::vector<game::PendingSessionSummary>& sessions)
	-> Json::Value {
	Json::Value payload(Json::arrayValue);
	for (const auto& session : sessions) {
		payload.append(SerializePendingSummary(session));
	}
	return payload;
}

auto SerializePendingSeat(const game::PendingSeat& seat) -> Json::Value {
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

auto SerializePendingSnapshot(const game::PendingSession& session,
			      const Json::Value& ratings = Json::Value(Json::nullValue)) -> Json::Value {
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

// auto EventKindName(game::EventKind kind) -> std::string_view {
// 	switch (kind) {
// 		case game::EventKind::kNone:
// 			return "none";
// 		case game::EventKind::kStart:
// 			return "start";
// 		case game::EventKind::kPredraw:
// 			return "predraw";
// 		case game::EventKind::kDrawTile:
// 			return "draw_tile";
// 		case game::EventKind::kDiscardTile:
// 			return "discard_tile";
// 		case game::EventKind::kChow:
// 			return "chow";
// 		case game::EventKind::kPung:
// 			return "pung";
// 		case game::EventKind::kMeldedKong:
// 			return "melded_kong";
// 		case game::EventKind::kAddedKong:
// 			return "added_kong";
// 		case game::EventKind::kConcealedKong:
// 			return "concealed_kong";
// 		case game::EventKind::kDiscardWin:
// 			return "discard_win";
// 		case game::EventKind::kRobAddedKongWin:
// 			return "rob_added_kong_win";
// 		case game::EventKind::kSelfDrawnWin:
// 			return "self_drawn_win";
// 		case game::EventKind::kPass:
// 			return "pass";
// 		case game::EventKind::kFinalPass:
// 			return "final_pass";
// 		case game::EventKind::kDrawnGame:
// 			return "drawn_game";
// 		case game::EventKind::kEnd:
// 			return "end";
// 		case game::EventKind::kPlayerLeft:
// 			return "player_left";
// 		case game::EventKind::kPlayerResumed:
// 			return "player_resumed";
// 	}

// 	return "unknown";
// }

auto PendingSessionContainsPlayer(const game::PendingSession& session, std::int64_t player_id) -> bool {
	return std::any_of(session.seats().begin(), session.seats().end(), [player_id](const game::PendingSeat& seat) {
		return seat.player.matches(player_id);
	});
}

auto ActiveSessionContainsPlayer(const game::ActiveSession& session, std::int64_t player_id) -> bool {
	return std::any_of(session.seats().begin(), session.seats().end(), [player_id](const game::Seat& seat) {
		return seat.player.matches(player_id);
	});
}

auto CanViewPendingSession(const game::PendingSession& session,
				   std::optional<std::int64_t> viewer_player_id) -> bool {
	return session.game_config().public_session ||
		(viewer_player_id.has_value() && PendingSessionContainsPlayer(session, *viewer_player_id));
}

auto CanViewActiveSession(const game::ActiveSession& session,
				  std::optional<std::int64_t> viewer_player_id) -> bool {
	return viewer_player_id.has_value() && ActiveSessionContainsPlayer(session, *viewer_player_id);
}

auto BuildGameMessage(std::string_view type, Json::Value payload) -> Json::Value {
	Json::Value message(Json::objectValue);
	message["type"] = std::string(type);
	message["payload"] = std::move(payload);
	return message;
}

auto MakeWebSocketEnvelope(std::string_view type,
			   Json::Value payload,
			   std::string_view request_id = {}) -> Json::Value {
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

auto MakeWebSocketAck(std::string_view request_id) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["ok"] = true;
	return MakeWebSocketEnvelope("ack", std::move(payload), request_id);
}

auto MakeWebSocketError(std::string_view code,
				std::string message,
				std::string_view request_id) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["code"] = std::string(code);
	payload["message"] = std::move(message);
	return MakeWebSocketEnvelope("error", std::move(payload), request_id);
}

auto ParseWebSocketMessage(std::string_view message) -> util::StatusOr<Json::Value> {
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

auto AuthMigrationsPath() -> std::filesystem::path {
	return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "auth" / "migrations";
}

auto StatsMigrationsPath() -> std::filesystem::path {
	return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "stats" / "migrations";
}

auto RatingMigrationsPath() -> std::filesystem::path {
	return std::filesystem::path(MMCR_SOURCE_DIR) / "src" / "ranking" / "migrations";
}

enum class WebSocketRoute {
	kLobby,
	kGame,
	kReplay,
};

auto WebSocketRoutePath(WebSocketRoute route) -> std::string_view {
	switch (route) {
		case WebSocketRoute::kLobby:
			return "/ws/lobby";
		case WebSocketRoute::kGame:
			return "/ws/game";
		case WebSocketRoute::kReplay:
			return "/ws/replay";
	}

	return "/ws/lobby";
}

auto ResolveWebSocketRoute(const drogon::HttpRequestPtr& request) -> WebSocketRoute {
	if (request->path() == WebSocketRoutePath(WebSocketRoute::kReplay)) {
		return WebSocketRoute::kReplay;
	}
	if (request->path() == WebSocketRoutePath(WebSocketRoute::kGame)) {
		return WebSocketRoute::kGame;
	}
	return WebSocketRoute::kLobby;
}

auto FindMessageTypeString(const Json::Value& message) -> std::optional<std::string> {
	const Json::Value* type = FindField(message, {"type"});
	if (type == nullptr || !type->isString()) {
		return std::nullopt;
	}
	return type->asString();
}

auto FindPayloadPhaseString(const Json::Value& message) -> std::optional<std::string> {
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

auto ClassifyInboundMessageRoute(std::string_view type) -> std::optional<WebSocketRoute> {
	if (type == "lobby.list") {
		return WebSocketRoute::kLobby;
	}

	if (type == "session.join" || type == "session.leave" || type == "queue.ready" ||
		type == "game.input" || type == "resume.ack") {
		return WebSocketRoute::kGame;
	}

	return std::nullopt;
}

auto ClassifyOutboundMessageRoute(const Json::Value& message) -> std::optional<WebSocketRoute> {
	auto type = FindMessageTypeString(message);
	if (!type.has_value()) {
		return std::nullopt;
	}

	if (*type == "session.snapshot") {
		auto phase = FindPayloadPhaseString(message);
		if (phase == "pending" || phase == "active") {
			return WebSocketRoute::kGame;
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

class GameClientContext {
public:
	explicit GameClientContext(std::shared_ptr<auth::PlayerProfile> player, WebSocketRoute route)
		: player_(std::move(player)), route_(route) {}

	[[nodiscard]] auto player() const -> const std::shared_ptr<auth::PlayerProfile>& {
		return player_;
	}

	[[nodiscard]] auto player_id() const noexcept -> std::int64_t {
		return player_ != nullptr ? player_->player_id : 0;
	}

	[[nodiscard]] auto player_handle() const -> auth::PlayerProfilePtr {
		return auth::PlayerProfilePtr(player_);
	}

	[[nodiscard]] auto route() const noexcept -> WebSocketRoute {
		return route_;
	}

	void mark_registered_with_hub() noexcept {
		registered_with_hub_ = true;
	}

	[[nodiscard]] auto registered_with_hub() const noexcept -> bool {
		return registered_with_hub_;
	}

private:
	std::shared_ptr<auth::PlayerProfile> player_;
	WebSocketRoute route_{WebSocketRoute::kLobby};
	bool registered_with_hub_{false};
};

auto ParseStatsFilterFromJson(const Json::Value& json) -> stats::StatsFilter {
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
	if (const Json::Value* include_nonstandard = FindField(json, {"include_nonstandard"}); include_nonstandard != nullptr && include_nonstandard->isBool()) {
		filter.include_nonstandard = include_nonstandard->asBool();
	}

	return filter;
}

auto StatsFilterPayloadIsTrivial(const Json::Value& json, const stats::StatsFilter& filter) -> bool {
	const bool has_no_record_criteria =
		filter.fan_filter_positive.empty() &&
		filter.fan_filter_negative.empty() &&
		filter.player_filter_positive.empty() &&
		filter.player_filter_negative.empty() &&
		!filter.player_id.has_value() &&
		!filter.win_player_id.has_value() &&
		filter.win_player_filter_negative.empty() &&
		!filter.from_player_id.has_value() &&
		filter.from_player_filter_negative.empty() &&
		filter.win_type_filter_positive.empty() &&
		filter.win_type_filter_negative.empty() &&
		!filter.self_drawn.has_value() &&
		filter.time_start == 0 &&
		filter.time_end == std::numeric_limits<std::int64_t>::max() &&
		filter.min_fan == 0.0 &&
		filter.max_fan == std::numeric_limits<double>::max();
	if (!has_no_record_criteria) {
		return false;
	}

	const Json::Value* exclude_superior = FindField(json, {"exclude_superior_fans"});
	const bool exclude_superior_fans =
		exclude_superior != nullptr && exclude_superior->isBool()
			? exclude_superior->asBool()
			: false;
	const Json::Value* include_nonstandard = FindField(json, {"include_nonstandard"});
	const bool include_nonstandard_rounds =
		include_nonstandard != nullptr && include_nonstandard->isBool()
			? include_nonstandard->asBool()
			: true;

	return !exclude_superior_fans && include_nonstandard_rounds;
}

auto StatsRecordSortKey(std::string_view sort_field, std::string_view sort_order) -> std::string {
	const bool fan_sort = sort_field == "fan";
	const bool asc = sort_order == "asc";
	return std::string(fan_sort ? "fan" : "time") + ":" + (asc ? "asc" : "desc");
}

struct StatsRecordsRequest {
	std::string sort_field{"time"};
	std::string sort_order{"desc"};
	std::size_t offset{0};
	std::size_t limit{16};
};

auto ParseStatsRecordsRequest(const Json::Value& root) -> StatsRecordsRequest {
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

auto BuildFanSortedStatsRounds(const std::vector<const stats::RoundEntry*>& time_desc_rounds,
					  bool descending) -> std::vector<const stats::RoundEntry*> {
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

auto BuildStatsRecordOrder(const std::vector<const stats::RoundEntry*>& time_desc_rounds,
				   std::string_view sort_field,
				   std::string_view sort_order) -> std::vector<const stats::RoundEntry*> {
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

auto SerializeStatsRoundEntriesPayload(const std::vector<const stats::RoundEntry*>& entries_to_emit,
					       std::size_t total,
					       std::size_t offset,
					       std::size_t limit) -> std::string {
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
			res0 = qingque::derepellenise(res0);
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

auto SerializeStatsRoundEntriesPage(const std::vector<const stats::RoundEntry*>& sorted,
					    std::size_t offset,
					    std::size_t limit) -> std::string {
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

auto ResolveLoggedPlayerId(const drogon::WebSocketConnectionPtr& connection,
				  std::optional<std::int64_t> player_id_override)
	-> std::optional<std::int64_t> {
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

auto ResolveLoggedRoute(const drogon::WebSocketConnectionPtr& connection,
			       std::optional<WebSocketRoute> route_override)
	-> std::optional<WebSocketRoute> {
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
				 int delay_ms = 0,
				 std::optional<std::int64_t> player_id = std::nullopt,
				 std::optional<WebSocketRoute> route = std::nullopt) {
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

class GameSocketHub {
public:
	explicit GameSocketHub(DebugTrafficLogger* logger = nullptr)
		: logger_(logger) {}

	void AddConnection(const drogon::WebSocketConnectionPtr& connection,
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

	[[nodiscard]] auto RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
					   std::int64_t player_id,
					   WebSocketRoute route) -> bool {
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

	void SendToPlayer(std::int64_t player_id, const Json::Value& message, int delay_ms) {
		const auto route = ClassifyOutboundMessageRoute(message);
		if (!route.has_value()) {
			LogDroppedOutboundMessage(player_id, message);
			return;
		}

		for (const auto& connection : LiveConnectionsForPlayer(player_id, route)) {
			SendLoggedWebSocketJson(logger_, connection, message, delay_ms, player_id, route);
		}
	}

	void EvictPlayerFromRoute(std::int64_t player_id, WebSocketRoute route,
				  DebugTrafficLogger* logger = nullptr,
				  const drogon::WebSocketConnection* keep_connection = nullptr) {
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

private:
	struct ConnectionEntry {
		std::int64_t player_id{0};
		WebSocketRoute route{WebSocketRoute::kLobby};
		std::weak_ptr<drogon::WebSocketConnection> connection;
	};

	auto LiveConnectionsForPlayer(std::int64_t player_id,
					  std::optional<WebSocketRoute> route = std::nullopt)
		-> std::vector<drogon::WebSocketConnectionPtr> {
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

	DebugTrafficLogger* logger_{nullptr};
	std::mutex mutex_;
	std::vector<ConnectionEntry> connections_;
};

class DatabasePool {
public:
	class Lease {
	public:
		Lease() = default;

		~Lease() {
			Release();
		}

		Lease(Lease&& other) noexcept {
			*this = std::move(other);
		}

		auto operator=(Lease&& other) noexcept -> Lease& {
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

		Lease(const Lease&) = delete;
		auto operator=(const Lease&) -> Lease& = delete;

		[[nodiscard]] auto database() -> storage::Database& {
			return *database_;
		}

	private:
		friend class DatabasePool;

		Lease(DatabasePool* pool, std::size_t index, storage::Database* database)
			: pool_(pool), index_(index), database_(database) {}

		void Release() {
			if (pool_ == nullptr) {
				return;
			}

			pool_->Release(index_);
			pool_ = nullptr;
			database_ = nullptr;
			index_ = 0;
		}

		DatabasePool* pool_{nullptr};
		std::size_t index_{0};
		storage::Database* database_{nullptr};
	};

	DatabasePool(std::filesystem::path database_path, std::size_t size)
		: database_path_(std::move(database_path)), size_(size == 0 ? 1 : size) {}

	[[nodiscard]] auto Initialize() -> util::Status {
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

	[[nodiscard]] auto Acquire() -> util::StatusOr<Lease> {
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

	[[nodiscard]] auto size() const noexcept -> std::size_t {
		return entries_.size();
	}

	private:
	struct Entry {
		storage::Database database;
		bool in_use{false};
	};

	void Release(std::size_t index) {
		std::lock_guard lock(mutex_);
		entries_[index].in_use = false;
		condition_.notify_one();
	}

	std::filesystem::path database_path_;
	std::size_t size_;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::vector<Entry> entries_;
};

class ServerState final : public game::GameTransport {
public:
	explicit ServerState(RuntimeConfig config)
		: config_(std::move(config)),
		  resolved_thread_count_(ResolveWorkerThreadCount(config_.thread_count)),
		  database_pool_(config_.database_path, ResolveDatabasePoolSize(config_.thread_count)),
		  record_manager_(config_.records_path),
		  stats_service_(&stats_database_),
		  rating_service_(&rating_database_),
		  replay_manager_(config_.records_path, config_.imported_records_path),
		  auth_config_(),
		  socket_hub_(&traffic_logger_),
		  game_hub_(&seed_container_, this, &record_manager_) {}

	[[nodiscard]] auto Initialize() -> util::Status {
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
		record_manager_.SetWriteObserver([this](const storage::GameRecordTask& task) {
			return stats_service_.UpsertRoundRecord(task.payload);
		});

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

	[[nodiscard]] auto Register(const auth::RegisterRequest& request)
		-> util::StatusOr<auth::RegisterResult> {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.Register(request);
	}

	[[nodiscard]] auto Login(const auth::LoginRequest& request)
		-> util::StatusOr<auth::AuthenticatedSession> {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.Login(request);
	}

	[[nodiscard]] auto Authenticate(std::string_view session_token, std::int64_t now_ms)
		-> util::StatusOr<auth::AuthenticatedSession> {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.Authenticate(session_token, now_ms);
	}

	[[nodiscard]] auto RefreshSession(std::string_view session_token, std::int64_t now_ms)
		-> util::StatusOr<auth::AuthenticatedSession> {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.RefreshSession(session_token, now_ms);
	}

	[[nodiscard]] auto Logout(std::string_view session_token, std::int64_t now_ms)
		-> util::Status {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.Logout(session_token, now_ms);
	}

	[[nodiscard]] auto ChangePassword(const auth::ChangePasswordRequest& request)
		-> util::Status {
		auto lease = database_pool_.Acquire();
		if (!lease.ok()) {
			return lease.status();
		}

		auto auth_service = MakeAuthService(&lease.value().database());
		return auth_service.ChangePassword(request);
	}

	[[nodiscard]] auto CreateSession(const game::CreateGameSessionRequest& request)
		-> util::StatusOr<game::CreateGameSessionResult> {
		return game_hub_.create_session(request);
	}

	[[nodiscard]] auto ConnectPlayer(const game::ConnectPlayerRequest& request) -> util::Status {
		return game_hub_.connect_player(request);
	}

	[[nodiscard]] auto DisconnectPlayer(const game::DisconnectPlayerRequest& request) -> util::Status {
		return game_hub_.disconnect_player(request);
	}

	[[nodiscard]] auto HandleGameMessage(const game::RouteGameMessageRequest& request) -> util::Status {
		return game_hub_.handle_message(request);
	}

	[[nodiscard]] auto FindPendingSession(std::int64_t session_id) const
		-> util::StatusOr<const game::PendingSession*> {
		return game_hub_.find_pending_session(session_id);
	}

	[[nodiscard]] auto FindActiveSession(std::int64_t session_id) const
		-> util::StatusOr<const game::ActiveSession*> {
		return game_hub_.find_active_session(session_id);
	}

	[[nodiscard]] auto ListVisibleJoinableSessions(
		std::optional<std::int64_t> viewer_player_id) const
		-> std::vector<game::PendingSessionSummary> {
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

	[[nodiscard]] auto ListVisibleActiveSessions(
		std::optional<std::int64_t> viewer_player_id) const
		-> std::vector<game::ActiveSessionSummary> {
		const auto sessions = game_hub_.list_active_sessions();
		if (!viewer_player_id.has_value()) {
			std::vector<game::ActiveSessionSummary> visible_sessions;
			visible_sessions.reserve(sessions.size());
			for (const auto& session : sessions) {
				if (session.public_session) {
					visible_sessions.push_back(session);
				}
			}
			return visible_sessions;
		}

		std::vector<game::ActiveSessionSummary> visible_sessions;
		visible_sessions.reserve(sessions.size());
		for (const auto& session : sessions) {
			if (session.public_session) {
				visible_sessions.push_back(session);
				continue;
			}

			auto active_session = game_hub_.find_active_session(session.session_id);
			if (!active_session.ok()) {
				continue;
			}
			if (ActiveSessionContainsPlayer(*active_session.value(), *viewer_player_id)) {
				visible_sessions.push_back(session);
			}
		}
		return visible_sessions;
	}

	[[nodiscard]] auto FindPlayerActiveSessionId(std::int64_t player_id) const
		-> std::optional<std::int64_t> {
		return game_hub_.find_player_active_session_id(player_id);
	}

	[[nodiscard]] auto FindPlayerPendingSessionId(std::int64_t player_id) const
		-> std::optional<std::int64_t> {
		return game_hub_.find_player_pending_session_id(player_id);
	}

	void AddConnection(const drogon::WebSocketConnectionPtr& connection,
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

	void RegisterAnonymousLobbyBrowser() {
		game_hub_.register_anonymous_browser();
	}

	[[nodiscard]] auto RemoveConnection(const drogon::WebSocketConnectionPtr& connection,
					   std::int64_t player_id,
					   WebSocketRoute route) -> bool {
		return socket_hub_.RemoveConnection(connection, player_id, route);
	}

	[[nodiscard]] auto resolved_thread_count() const noexcept -> std::size_t {
		return resolved_thread_count_;
	}

	[[nodiscard]] auto database_pool_size() const noexcept -> std::size_t {
		return database_pool_.size();
	}

	[[nodiscard]] auto records_path() const -> const std::filesystem::path& {
		return config_.records_path;
	}

	[[nodiscard]] auto ListReplays() const -> util::StatusOr<std::vector<replay::ReplayInfo>> {
		return replay_manager_.ListSessions();
	}

	[[nodiscard]] auto LoadReplayRound(std::string_view session_identifier,
						  std::uint64_t round_number) const
		-> util::StatusOr<Json::Value> {
		return replay_manager_.LoadRoundRecord(session_identifier, round_number);
	}

	[[nodiscard]] auto LoadReplaySession(std::string_view session_identifier) const
		-> util::StatusOr<std::vector<Json::Value>> {
		return replay_manager_.LoadSessionRecords(session_identifier);
	}

	void LogHttpRequest(const drogon::HttpRequestPtr& request) {
		traffic_logger_.Log(BuildHttpRequestLogEntry(request));
	}

	void LogHttpResponse(const drogon::HttpRequestPtr& request,
				 const drogon::HttpResponsePtr& response) {
		traffic_logger_.Log(BuildHttpResponseLogEntry(request, response));
	}

	void LogWebSocketInbound(const drogon::WebSocketConnectionPtr& connection,
				    std::string_view message,
				    const drogon::WebSocketMessageType& type) {
		LogInboundWebSocketMessage(&traffic_logger_, connection, message, type);
	}

	void SendWebSocketJson(const drogon::WebSocketConnectionPtr& connection,
				  const Json::Value& message,
				  int delay_ms = 0,
				  std::optional<std::int64_t> player_id = std::nullopt,
				  std::optional<WebSocketRoute> route = std::nullopt) {
		SendLoggedWebSocketJson(&traffic_logger_, connection, message, delay_ms, player_id, route);
	}

	void send_to_player(std::int64_t player_id,
			   const Json::Value& message,
			   int delay_ms = 0) override {
		socket_hub_.SendToPlayer(player_id, message, delay_ms);
	}

	void on_session_ended(std::int64_t session_id,
			      const std::array<std::int64_t, 4>& player_ids,
			      const std::array<int, 4>& final_scores,
			      int round_count) override {
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

	auto get_player_ratings(const std::array<std::int64_t, 4>& player_ids)
		-> std::vector<game::PlayerRatingSnapshot> override {
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

	[[nodiscard]] auto stats() -> stats::StatsService& {
		return stats_service_;
	}
	[[nodiscard]] auto stats() const -> const stats::StatsService& {
		return stats_service_;
	}

	[[nodiscard]] auto socket_hub() -> GameSocketHub& {
		return socket_hub_;
	}

	[[nodiscard]] auto traffic_logger() -> DebugTrafficLogger* {
		return &traffic_logger_;
	}

	[[nodiscard]] auto rating() -> ranking::RatingService& {
		return rating_service_;
	}

private:
	[[nodiscard]] auto MakeAuthService(storage::Database* database) const -> auth::AuthService {
		return auth::AuthService(database, auth_config_, const_cast<random::SeedContainer*>(&seed_container_));
	}

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

auto AuthenticateRequest(ServerState& state, const drogon::HttpRequestPtr& request)
	-> util::StatusOr<auth::AuthenticatedSession> {
	auto token = ExtractBearerToken(request);
	if (!token.ok()) {
		return token.status();
	}

	return state.Authenticate(token.value(), CurrentUnixTimeMs());
}

auto AuthenticateWebSocketRequest(ServerState& state, const drogon::HttpRequestPtr& request)
	-> util::StatusOr<auth::AuthenticatedSession> {
	auto token = ExtractWebSocketToken(request);
	if (!token.ok()) {
		return token.status();
	}

	return state.Authenticate(token.value(), CurrentUnixTimeMs());
}

auto AuthenticateOptionalWebSocketRequest(ServerState& state,
					  const drogon::HttpRequestPtr& request)
	-> util::StatusOr<std::optional<auth::AuthenticatedSession>> {
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

auto AuthenticateOptionalRequest(ServerState& state, const drogon::HttpRequestPtr& request)
	-> util::StatusOr<std::optional<auth::AuthenticatedSession>> {
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

auto BuildLobbyListPayload(const ServerState& state,
			   std::optional<std::int64_t> viewer_player_id) -> Json::Value {
	Json::Value payload(Json::objectValue);
	payload["sessions"] = SerializePendingSummaryList(state.ListVisibleJoinableSessions(viewer_player_id));
	payload["active_sessions"] = SerializeActiveSummaryList(state.ListVisibleActiveSessions(viewer_player_id));
	return payload;
}

auto BuildSessionSnapshotPayload(ServerState& state,
				std::int64_t session_id,
				std::optional<std::int64_t> viewer_player_id)
	-> util::StatusOr<Json::Value> {
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
	if (!viewer_player_id.has_value()) {
		return util::Status::NotFound("session not found");
	}
	return active_session.value()->build_snapshot_for_player_id(*viewer_player_id);
}

auto BuildCreatedSessionSnapshotPayload(ServerState& state,
					 std::int64_t session_id,
					 std::int64_t owner_player_id)
	-> util::StatusOr<Json::Value> {
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

auto BuildReplaySessionPayload(const ServerState& state,
				   std::string_view session_identifier)
	-> util::StatusOr<Json::Value> {
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
		NormalizeReplaySeedFields(round_record);
		round_records_payload.append(std::move(round_record));
	}
	payload["round_records"] = std::move(round_records_payload);
	return payload;
}

struct ReplayListQuery {
	int page{1};
	int page_size{10};
	std::string session_query;
	std::string player_query;
	bool exact_session_match{false};
	std::optional<std::int64_t> started_after_ms;
	std::optional<std::int64_t> started_before_ms;
};

auto ReadOptionalStringField(const Json::Value& object,
				 std::initializer_list<std::string_view> names,
				 std::string_view label) -> util::StatusOr<std::string> {
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

auto ReadOptionalInt64Field(const Json::Value& object,
				std::initializer_list<std::string_view> names,
				std::string_view label) -> util::StatusOr<std::optional<std::int64_t>> {
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

auto ToLowerCopy(std::string_view value) -> std::string {
	std::string lowered;
	lowered.reserve(value.size());
	for (const unsigned char character : value) {
		lowered.push_back(static_cast<char>(std::tolower(character)));
	}
	return lowered;
}

auto ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) -> bool {
	if (needle.empty()) {
		return true;
	}
	const auto lowered_haystack = ToLowerCopy(haystack);
	const auto lowered_needle = ToLowerCopy(needle);
	return lowered_haystack.find(lowered_needle) != std::string::npos;
}

auto EqualsCaseInsensitiveText(std::string_view left, std::string_view right) -> bool {
	return ToLowerCopy(left) == ToLowerCopy(right);
}

auto ParseReplayListQuery(const Json::Value& object) -> util::StatusOr<ReplayListQuery> {
	if (!object.isObject()) {
		return util::Status::InvalidArgument("replay list payload must be a JSON object");
	}

	ReplayListQuery query;

	auto page = ReadOptionalInt(object, {"page"}, "page", query.page);
	if (!page.ok()) {
		return page.status();
	}
	if (page.value() <= 0) {
		return util::Status::InvalidArgument("page must be positive");
	}
	query.page = page.value();

	auto page_size = ReadOptionalInt(object, {"page_size", "pageSize"}, "page_size", query.page_size);
	if (!page_size.ok()) {
		return page_size.status();
	}
	if (page_size.value() <= 0 || page_size.value() > 50) {
		return util::Status::InvalidArgument("page_size must be between 1 and 50");
	}
	query.page_size = page_size.value();

	auto session_query = ReadOptionalStringField(object, {"session_query", "sessionQuery"}, "session_query");
	if (!session_query.ok()) {
		return session_query.status();
	}
	query.session_query = session_query.value();

	auto player_query = ReadOptionalStringField(object, {"player_query", "playerQuery"}, "player_query");
	if (!player_query.ok()) {
		return player_query.status();
	}
	query.player_query = player_query.value();

	auto exact_session_match = ReadOptionalBool(
		object,
		{"exact_session_match", "exactSessionMatch"},
		"exact_session_match",
		query.exact_session_match);
	if (!exact_session_match.ok()) {
		return exact_session_match.status();
	}
	query.exact_session_match = exact_session_match.value();

	auto started_after_ms = ReadOptionalInt64Field(
		object,
		{"started_after_ms", "startedAfterMs"},
		"started_after_ms");
	if (!started_after_ms.ok()) {
		return started_after_ms.status();
	}
	query.started_after_ms = started_after_ms.value();

	auto started_before_ms = ReadOptionalInt64Field(
		object,
		{"started_before_ms", "startedBeforeMs"},
		"started_before_ms");
	if (!started_before_ms.ok()) {
		return started_before_ms.status();
	}
	query.started_before_ms = started_before_ms.value();

	if (query.started_after_ms.has_value() && query.started_before_ms.has_value() &&
		*query.started_after_ms > *query.started_before_ms) {
		return util::Status::InvalidArgument("started_after_ms must be less than or equal to started_before_ms");
	}

	return query;
}

auto BuildReplayListPayload(const ServerState& state,
				const ReplayListQuery& query) -> util::StatusOr<Json::Value> {
	auto replays = state.ListReplays();
	if (!replays.ok()) {
		return replays.status();
	}

	std::vector<const replay::ReplayInfo*> filtered_replays;
	filtered_replays.reserve(replays.value().size());
	std::unordered_set<std::string> unique_player_names;

	for (const auto& replay_info : replays.value()) {
		const auto timestamp_ms = static_cast<std::int64_t>(replay_info.timestamp_ns / 1000000ULL);
		if (query.started_after_ms.has_value() && timestamp_ms < *query.started_after_ms) {
			continue;
		}
		if (query.started_before_ms.has_value() && timestamp_ms > *query.started_before_ms) {
			continue;
		}

		if (!query.session_query.empty()) {
			const bool session_matches = query.exact_session_match
				? EqualsCaseInsensitiveText(replay_info.session_identifier, query.session_query)
				: ContainsCaseInsensitive(replay_info.session_identifier, query.session_query);
			if (!session_matches) {
				continue;
			}
		}

		if (!query.player_query.empty()) {
			bool matched_player = false;
			for (const auto& player_name : replay_info.player_names) {
				if (ContainsCaseInsensitive(player_name, query.player_query)) {
					matched_player = true;
					break;
				}
			}
			if (!matched_player) {
				continue;
			}
		}

		filtered_replays.push_back(&replay_info);
		for (const auto& player_name : replay_info.player_names) {
			const auto trimmed_name = TrimString(player_name);
			if (!trimmed_name.empty()) {
				unique_player_names.insert(std::string(trimmed_name));
			}
		}
	}

	const std::size_t total_count = filtered_replays.size();
	const std::size_t page_count =
		total_count == 0 ? 1 : (total_count + static_cast<std::size_t>(query.page_size) - 1) /
						   static_cast<std::size_t>(query.page_size);
	const std::size_t current_page = std::min<std::size_t>(
		static_cast<std::size_t>(query.page),
		page_count);
	const std::size_t offset = (current_page - 1) * static_cast<std::size_t>(query.page_size);
	const std::size_t end = std::min(
		offset + static_cast<std::size_t>(query.page_size),
		total_count);

	Json::Value payload(Json::objectValue);
	Json::Value replay_payload(Json::arrayValue);
	for (std::size_t index = offset; index < end; ++index) {
		replay_payload.append(SerializeReplayInfo(*filtered_replays[index]));
	}
	payload["replays"] = std::move(replay_payload);
	payload["total_count"] = Json::UInt64(total_count);
	payload["page"] = Json::UInt64(current_page);
	payload["page_size"] = query.page_size;
	payload["page_count"] = Json::UInt64(page_count);
	payload["unique_player_count"] = Json::UInt64(unique_player_names.size());
	payload["latest_timestamp_ms"] =
		total_count == 0
			? Json::Value(Json::nullValue)
			: Json::Value(Json::UInt64(filtered_replays.front()->timestamp_ns / 1000000ULL));
	payload["session_query"] = query.session_query;
	payload["player_query"] = query.player_query;
	payload["exact_session_match"] = query.exact_session_match;
	return payload;
}

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
			const bool trivial_filter = StatsFilterPayloadIsTrivial(*filter_json, filter);
			const std::string cache_key = trivial_filter ? std::string("overall") : "filter:" + JsonToCompactString(*filter_json);
			StatsWsSession result;
			if (GetReusableStatsCache(connection.get(), cache_key, result)) {
				// Cached result is still current.
			} else if (trivial_filter) {
				auto built = BuildOverallStatsResult();
				if (!built.ok()) {
					state_->SendWebSocketJson(connection,
						MakeWebSocketError("stats_query_failed",
							built.status().message(), request_id));
					return;
				}
				result = std::move(built.value());
				StoreStatsCache(connection.get(), result);
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
		result.filter.include_nonstandard = true;
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

				if (!round_entry->fan_names.empty()) {
					std::string fans_str;
					for (std::size_t j = 0; j < round_entry->fan_names.size(); ++j) {
						if (j > 0) {
							fans_str += ", ";
						}
						fans_str += round_entry->fan_names[j];
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

}  // namespace

util::StatusOr<RuntimeConfig> LoadRuntimeConfigFromEnv() {
	RuntimeConfig config;
	config.bind_address = std::string(kDefaultBindAddress);
	config.port = kDefaultPort;
	config.thread_count = kDefaultThreadCount;
	config.database_path = std::filesystem::path(kDefaultDatabasePath);
	config.debug_log_dir = std::filesystem::path(kDefaultDebugLogDir);

	if (const char* raw_bind_address = std::getenv("MMCR_BACKEND_BIND_ADDRESS");
		raw_bind_address != nullptr && raw_bind_address[0] != '\0') {
		config.bind_address = raw_bind_address;
	}

	if (const char* raw_port = std::getenv("MMCR_BACKEND_PORT");
		raw_port != nullptr && raw_port[0] != '\0') {
		auto port = ParseUnsignedInteger(raw_port, "MMCR_BACKEND_PORT");
		if (!port.ok()) {
			return port.status();
		}
		if (port.value() == 0 || port.value() > 65535) {
			return util::Status::InvalidArgument(
				"MMCR_BACKEND_PORT must be between 1 and 65535");
		}
		config.port = static_cast<std::uint16_t>(port.value());
	}

	if (const char* raw_threads = std::getenv("MMCR_BACKEND_THREADS");
		raw_threads != nullptr && raw_threads[0] != '\0') {
		auto threads = ParseUnsignedInteger(raw_threads, "MMCR_BACKEND_THREADS");
		if (!threads.ok()) {
			return threads.status();
		}
		if (threads.value() > std::numeric_limits<std::size_t>::max()) {
			return util::Status::InvalidArgument("MMCR_BACKEND_THREADS is out of range");
		}
		config.thread_count = static_cast<std::size_t>(threads.value());
	}

	if (const char* raw_database_path = std::getenv("MMCR_BACKEND_DB_PATH");
		raw_database_path != nullptr && raw_database_path[0] != '\0') {
		config.database_path = std::filesystem::path(raw_database_path);
	}

	if (const char* raw_debug_log_dir = std::getenv("MMCR_BACKEND_DEBUG_LOG_DIR");
		raw_debug_log_dir != nullptr && raw_debug_log_dir[0] != '\0') {
		config.debug_log_dir = std::filesystem::path(raw_debug_log_dir);
	}

	if (const char* raw_records_path = std::getenv("MMCR_RECORDS_DIR");
		raw_records_path != nullptr && raw_records_path[0] != '\0') {
		config.records_path = std::filesystem::path(raw_records_path);
	}

	if (const char* raw_imported_records_path = std::getenv("MMCR_IMPORTED_RECORDS_DIR");
		raw_imported_records_path != nullptr && raw_imported_records_path[0] != '\0') {
		config.imported_records_path = std::filesystem::path(raw_imported_records_path);
	}

	if (const char* raw_ssl_cert_path = std::getenv("MMCR_BACKEND_SSL_CERT_PATH");
		raw_ssl_cert_path != nullptr && raw_ssl_cert_path[0] != '\0') {
		config.ssl_cert_path = std::filesystem::path(raw_ssl_cert_path);
	}

	if (const char* raw_ssl_key_path = std::getenv("MMCR_BACKEND_SSL_KEY_PATH");
		raw_ssl_key_path != nullptr && raw_ssl_key_path[0] != '\0') {
		config.ssl_key_path = std::filesystem::path(raw_ssl_key_path);
	}

	if (TrimString(config.bind_address).empty()) {
		return util::Status::InvalidArgument("bind address must not be empty");
	}
	if (config.database_path.empty()) {
		return util::Status::InvalidArgument("database path must not be empty");
	}
	if (config.records_path.empty()) {
		return util::Status::InvalidArgument("MMCR_RECORDS_DIR must not be empty");
	}
	if (config.ssl_cert_path.empty() != config.ssl_key_path.empty()) {
		return util::Status::InvalidArgument(
			"MMCR_BACKEND_SSL_CERT_PATH and MMCR_BACKEND_SSL_KEY_PATH must be set together");
	}
	if (!config.ssl_cert_path.empty() && !std::filesystem::is_regular_file(config.ssl_cert_path)) {
		return util::Status::InvalidArgument(
			"MMCR_BACKEND_SSL_CERT_PATH must point to an existing certificate file");
	}
	if (!config.ssl_key_path.empty() && !std::filesystem::is_regular_file(config.ssl_key_path)) {
		return util::Status::InvalidArgument(
			"MMCR_BACKEND_SSL_KEY_PATH must point to an existing private key file");
	}

	return config;
}

int RunServer(const RuntimeConfig& config) {
	auto state = std::make_shared<ServerState>(config);
	const auto status = state->Initialize();
	if (!status.ok()) {
		std::cerr << status.DebugString() << '\n';
		return 1;
	}

	RegisterHttpRoutes(state);
	drogon::app().registerController(std::make_shared<GameWebSocketController>(state));
	drogon::app().registerController(std::make_shared<ReplayListWebSocketController>(state));
	drogon::app().registerController(std::make_shared<ReplayWebSocketController>(state));
	drogon::app().registerController(std::make_shared<StatsWebSocketController>(state));
	auto& app = drogon::app();
	app.setThreadNum(config.thread_count);
	const bool use_ssl = !config.ssl_cert_path.empty();
	if (use_ssl) {
		app.addListener(
			config.bind_address,
			config.port,
			true,
			config.ssl_cert_path.string(),
			config.ssl_key_path.string());
	} else {
		app.addListener(config.bind_address, config.port);
	}

	std::cout << mmcr::util::kProjectName << ' ' << mmcr::util::kBuildVersion << " listening on "
		  << (use_ssl ? "https://" : "http://") << config.bind_address << ':' << config.port << " workers="
		  << state->resolved_thread_count() << " db_pool=" << state->database_pool_size()
		  << '\n';
	app.run();
	return 0;
}

}  // namespace mmcr::server