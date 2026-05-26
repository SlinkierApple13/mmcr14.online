#include "replay/manager.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <system_error>

#include "util/status.h"

namespace mmcr::replay {
namespace {

auto ParseJsonFile(const std::filesystem::path& path) -> util::StatusOr<Json::Value> {
	std::ifstream stream(path);
	if (!stream.is_open()) {
		return util::Status::NotFound("failed to open replay record: " + path.string());
	}

	Json::CharReaderBuilder builder;
	Json::Value payload;
	std::string errors;
	if (!Json::parseFromStream(builder, stream, &payload, &errors)) {
		return util::Status::InvalidArgument("failed to parse replay record: " + errors);
	}
	return payload;
}

auto CopyDirectoryRecursively(const std::filesystem::path& from,
					  const std::filesystem::path& to) -> util::Status {
	std::error_code error;
	std::filesystem::create_directories(to, error);
	if (error) {
		return util::Status::Internal("failed to create replay import target: " + error.message());
	}

	for (const auto& entry : std::filesystem::recursive_directory_iterator(from)) {
		const auto relative = std::filesystem::relative(entry.path(), from, error);
		if (error) {
			return util::Status::Internal("failed to derive replay import path: " + error.message());
		}
		const auto target = to / relative;
		if (entry.is_directory()) {
			std::filesystem::create_directories(target, error);
			if (error) {
				return util::Status::Internal("failed to create replay import directory: " + error.message());
			}
			continue;
		}
		std::filesystem::copy_file(
			entry.path(),
			target,
			std::filesystem::copy_options::overwrite_existing,
			error);
		if (error) {
			return util::Status::Internal("failed to copy replay import file: " + error.message());
		}
	}

	return util::Status::Ok();
}

auto TryParseTimestampNs(std::string_view session_identifier) -> std::uint64_t {
	const auto underscore = session_identifier.find_last_of('_');
	if (underscore == std::string_view::npos || underscore + 1 >= session_identifier.size()) {
		return 0;
	}
	std::uint64_t timestamp_ns = 0;
	const auto timestamp = session_identifier.substr(underscore + 1);
	const auto begin = timestamp.data();
	const auto end = begin + timestamp.size();
	auto result = std::from_chars(begin, end, timestamp_ns);
	if (result.ec != std::errc() || result.ptr != end) {
		return 0;
	}
	return timestamp_ns;
}

auto ReadPlayerNames(const Json::Value& round_record) -> std::vector<std::string> {
	std::vector<std::string> names;
	const Json::Value& seats = round_record["initial_seats"];
	if (!seats.isArray()) {
		return names;
	}
	for (const auto& seat : seats) {
		const Json::Value& name = seat["player_name"];
		if (name.isString()) {
			names.push_back(name.asString());
		}
	}
	return names;
}

}  // namespace

ReplayManager::ReplayManager(std::filesystem::path records_root,
				     std::filesystem::path imported_records_root)
	: records_root_(std::move(records_root)),
	  imported_records_root_(std::move(imported_records_root)) {}

auto ReplayManager::Initialize() -> util::Status {
	if (records_root_.empty()) {
		return util::Status::InvalidArgument("records root must not be empty");
	}
	if (!std::filesystem::exists(records_root_)) {
		return util::Status::InvalidArgument("records root must exist");
	}
	if (!std::filesystem::is_directory(records_root_)) {
		return util::Status::InvalidArgument("records root must be a directory");
	}
	return import_external_records();
}

auto ReplayManager::ListSessions() const -> util::StatusOr<std::vector<ReplayInfo>> {
	std::vector<ReplayInfo> sessions;
	for (const auto& entry : std::filesystem::directory_iterator(records_root_)) {
		if (!entry.is_directory()) {
			continue;
		}

		const auto session_identifier = entry.path().filename().string();
		std::uint64_t round_count = 0;
		for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
			if (!file.is_regular_file() || file.path().extension() != ".json") {
				continue;
			}
			++round_count;
		}
		if (round_count == 0) {
			continue;
		}

		auto first_round = LoadRoundRecord(session_identifier, 1);
		if (!first_round.ok()) {
			continue;
		}

		sessions.push_back(ReplayInfo{
			.session_identifier = session_identifier,
			.timestamp_ns = TryParseTimestampNs(session_identifier),
			.round_count = round_count,
			.player_names = ReadPlayerNames(first_round.value()),
		});
	}

	std::sort(sessions.begin(), sessions.end(), [](const ReplayInfo& left, const ReplayInfo& right) {
		return left.timestamp_ns > right.timestamp_ns;
	});
	return sessions;
}

auto ReplayManager::LoadSessionRecords(std::string_view session_identifier) const
	-> util::StatusOr<std::vector<Json::Value>> {
	auto valid = validate_session_identifier(session_identifier);
	if (!valid.ok()) {
		return valid;
	}

	const auto session_path = records_root_ / std::string(session_identifier);
	if (!std::filesystem::exists(session_path)) {
		return util::Status::NotFound("replay session not found");
	}
	if (!std::filesystem::is_directory(session_path)) {
		return util::Status::InvalidArgument("replay session path must be a directory");
	}

	std::vector<std::pair<std::uint64_t, std::filesystem::path>> round_files;
	for (const auto& entry : std::filesystem::directory_iterator(session_path)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".json") {
			continue;
		}

		const auto stem = entry.path().stem().string();
		std::uint64_t round_number = 0;
		auto parse_result = std::from_chars(stem.data(), stem.data() + stem.size(), round_number);
		if (parse_result.ec != std::errc() || parse_result.ptr != stem.data() + stem.size()) {
			continue;
		}
		if (round_number == 0) {
			continue;
		}
		round_files.emplace_back(round_number, entry.path());
	}

	if (round_files.empty()) {
		return util::Status::NotFound("replay session has no round records");
	}

	std::sort(round_files.begin(), round_files.end(), [](const auto& left, const auto& right) {
		return left.first < right.first;
	});

	std::vector<Json::Value> round_records;
	round_records.reserve(round_files.size());
	for (const auto& [round_number, path] : round_files) {
		(void)round_number;
		auto parsed = ParseJsonFile(path);
		if (!parsed.ok()) {
			return parsed.status();
		}
		round_records.push_back(std::move(parsed.value()));
	}

	return round_records;
}

auto ReplayManager::LoadRoundRecord(std::string_view session_identifier,
					     std::uint64_t round_number) const -> util::StatusOr<Json::Value> {
	auto valid = validate_session_identifier(session_identifier);
	if (!valid.ok()) {
		return valid;
	}
	if (round_number == 0) {
		return util::Status::InvalidArgument("round number must be positive");
	}

	const auto record_path = records_root_ / std::string(session_identifier) /
		(std::to_string(round_number) + ".json");
	return ParseJsonFile(record_path);
}

auto ReplayManager::validate_session_identifier(std::string_view session_identifier) const -> util::Status {
	if (session_identifier.empty()) {
		return util::Status::InvalidArgument("session identifier must not be empty");
	}
	const std::filesystem::path path(session_identifier);
	if (path.has_parent_path() || path.filename() != path) {
		return util::Status::InvalidArgument("session identifier must be a single path segment");
	}
	return util::Status::Ok();
}

auto ReplayManager::import_external_records() -> util::Status {
	if (imported_records_root_.empty()) {
		return util::Status::Ok();
	}
	if (!std::filesystem::exists(imported_records_root_)) {
		return util::Status::InvalidArgument("imported records root must exist when configured");
	}
	if (!std::filesystem::is_directory(imported_records_root_)) {
		return util::Status::InvalidArgument("imported records root must be a directory");
	}

	for (const auto& entry : std::filesystem::directory_iterator(imported_records_root_)) {
		if (!entry.is_directory()) {
			continue;
		}

		const auto destination = records_root_ / entry.path().filename();
		if (std::filesystem::exists(destination)) {
			return util::Status::Internal(
				"cannot import replay folder because destination already exists: " +
				destination.string());
		}

		std::error_code error;
		std::filesystem::rename(entry.path(), destination, error);
		if (!error) {
			continue;
		}

		auto copy_status = CopyDirectoryRecursively(entry.path(), destination);
		if (!copy_status.ok()) {
			return copy_status;
		}
		std::filesystem::remove_all(entry.path(), error);
		if (error) {
			return util::Status::Internal("failed to remove imported replay source: " + error.message());
		}
	}

	return util::Status::Ok();
}

}  // namespace mmcr::replay