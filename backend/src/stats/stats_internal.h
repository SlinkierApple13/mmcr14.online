#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <jsoncpp/json/json.h>

#include "external/qingque/rules/qingque.h"
#include "game/engine/hand.h"
#include "stats/service.h"
#include "storage/database.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::stats {

// ---------------------------------------------------------------------------
// JSON helpers shared between the round-entry projection, the database
// persistence layer, and the round collection filtering code.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string JsonToCompactString(const Json::Value& value);
[[nodiscard]] util::StatusOr<Json::Value> ParseJsonString(std::string_view raw);

[[nodiscard]] util::StatusOr<const Json::Value*> ReadRequiredObject(
    const Json::Value& object, std::string_view field_name);
[[nodiscard]] util::StatusOr<const Json::Value*> ReadRequiredArray(
    const Json::Value& object, std::string_view field_name);
[[nodiscard]] util::StatusOr<std::string> ReadRequiredString(
    const Json::Value& object, std::string_view field_name);
[[nodiscard]] util::StatusOr<std::int64_t> ReadRequiredInt64(
    const Json::Value& object, std::string_view field_name);
[[nodiscard]] util::StatusOr<std::uint64_t> ReadRequiredUInt64(
    const Json::Value& object, std::string_view field_name);
[[nodiscard]] util::StatusOr<double> ReadRequiredDouble(
    const Json::Value& object, std::string_view field_name);

[[nodiscard]] util::StatusOr<std::array<RoundPlayer, 4>> ParsePlayers(
    const Json::Value& initial_seats);
[[nodiscard]] std::string EncodePlayersJson(const std::array<RoundPlayer, 4>& players);
[[nodiscard]] util::StatusOr<std::array<RoundPlayer, 4>> ParsePlayersJson(std::string_view raw);

[[nodiscard]] std::string EncodeMeldCountJson(const std::array<int, 4>& meld_count);
[[nodiscard]] util::StatusOr<std::array<int, 4>> ParseMeldCountJson(std::string_view raw);

[[nodiscard]] std::string EncodeFanResultsJson(
    const std::vector<qingque::fan_code>& fan_results);
[[nodiscard]] util::StatusOr<std::vector<qingque::fan_code>> ParseFanResultsJson(
    std::string_view raw);
[[nodiscard]] std::vector<int> FanIdsFromResults(
    const std::vector<qingque::fan_code>& fan_results);

[[nodiscard]] std::string EncodeWinningHandJson(
    const std::optional<game::HandWrapper>& winning_hand);
[[nodiscard]] util::StatusOr<std::optional<game::HandWrapper>> ParseWinningHandJson(
    std::string_view raw);

[[nodiscard]] util::Status StepDone(storage::Statement& statement);
[[nodiscard]] util::StatusOr<double> ParseFanText(std::string_view raw);

[[nodiscard]] bool IsNonstandardSession(std::string_view session_identifier);

}  // namespace mmcr::stats
