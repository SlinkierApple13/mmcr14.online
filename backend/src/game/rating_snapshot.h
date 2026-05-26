#pragma once

#include <cstdint>
#include <vector>

#include <jsoncpp/json/json.h>

namespace mmcr::game {

struct PlayerRatingSnapshot {
    std::int64_t player_id{0};
    std::string username;
    double mu{0.0};
    double tau{15.0};
    double sigma{300.0};
    double points{0.0};
    int level{0};
    std::int64_t total_games{0};

    [[nodiscard]] auto ToJson() const -> Json::Value;
    [[nodiscard]] static auto FromJson(const Json::Value& json) -> PlayerRatingSnapshot;
};

}  // namespace mmcr::game
