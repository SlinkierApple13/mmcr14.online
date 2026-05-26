#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <jsoncpp/json/json.h>

#include "util/status_or.h"

namespace mmcr::replay {

struct ReplayInfo {
	std::string session_identifier;
	std::uint64_t timestamp_ns{0};
	std::uint64_t round_count{0};
	std::vector<std::string> player_names;
};

class ReplayManager {
public:
	ReplayManager(std::filesystem::path records_root,
			  std::filesystem::path imported_records_root = {});

	[[nodiscard]] auto Initialize() -> util::Status;
	[[nodiscard]] auto ListSessions() const -> util::StatusOr<std::vector<ReplayInfo>>;
	[[nodiscard]] auto LoadSessionRecords(std::string_view session_identifier) const
		-> util::StatusOr<std::vector<Json::Value>>;
	[[nodiscard]] auto LoadRoundRecord(std::string_view session_identifier,
						  std::uint64_t round_number) const -> util::StatusOr<Json::Value>;

	[[nodiscard]] auto records_root() const noexcept -> const std::filesystem::path& {
		return records_root_;
	}

private:
	[[nodiscard]] auto validate_session_identifier(std::string_view session_identifier) const -> util::Status;
	[[nodiscard]] auto import_external_records() -> util::Status;

	std::filesystem::path records_root_;
	std::filesystem::path imported_records_root_;
};

}  // namespace mmcr::replay