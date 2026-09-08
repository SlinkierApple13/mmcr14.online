#include "game/mode/mode_config.h"

#include <limits>
#include <string>

#include "util/status.h"

namespace mmcr::game {

Json::Value SerializeModeConfigJson(const GameConfig& config) {
    if (config.mode != GameMode::kPassFiveGates || !config.pass_five_gates.has_value()) {
        return Json::Value(Json::nullValue);
    }
    const PassFiveGatesConfig& cfg = *config.pass_five_gates;
    Json::Value payload(Json::objectValue);
    payload["knockout_score"] = cfg.knockout_score;
    Json::Value team(Json::objectValue);
    for (const auto& [player_id, seat_team] : cfg.team) {
        team[std::to_string(player_id)] = seat_team;
    }
    payload["team"] = std::move(team);
    return payload;
}

util::StatusOr<PassFiveGatesConfig> ParsePassFiveGatesConfig(
    const Json::Value& raw_mode_config) {
    if (raw_mode_config.isNull()) {
        return PassFiveGatesConfig{};
    }
    if (!raw_mode_config.isObject()) {
        return util::Status::InvalidArgument("mode_config must be a JSON object");
    }
    // Only "knockout_score" is a client-settable parameter. The "team" key is
    // injected by the server at session start and must be rejected from client
    // input (a client could otherwise forge membership).
    for (const auto& member : raw_mode_config.getMemberNames()) {
        if (member != "knockout_score") {
            return util::Status::InvalidArgument("mode_config has unsupported key: " + member);
        }
    }
    PassFiveGatesConfig cfg;
    const Json::Value& knockout_value = raw_mode_config["knockout_score"];
    if (!knockout_value.isNull()) {
        if (!knockout_value.isInt() && !knockout_value.isInt64() && !knockout_value.isUInt() &&
            !knockout_value.isUInt64()) {
            return util::Status::InvalidArgument("knockout_score must be an integer");
        }
        if ((knockout_value.isInt64() || knockout_value.isInt()) &&
            knockout_value.asInt64() < 0) {
            return util::Status::InvalidArgument("knockout_score must be >= 0");
        }
        if (knockout_value.isUInt64() &&
            knockout_value.asUInt64() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return util::Status::InvalidArgument("knockout_score is out of range");
        }
        cfg.knockout_score = knockout_value.asInt();
    }
    return cfg;
}

}  // namespace mmcr::game
