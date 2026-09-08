#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jsoncpp/json/json.h>

#include "game/config.h"

namespace mmcr::game {

// Built-in play-mode preset. The registry is the single source of truth for
// both request validation (ParseGameConfig) and the public modes list
// (GET /api/v1/game/modes). Adding a village rule = one entry here + a
// ModeController implementation.
struct ModePreset {
    std::string id;            // wire id, e.g. "pass_five_gates"
    std::string name;          // display name, e.g. "过五关"
    GameMode mode;             // enum value
    // Default client-settable parameters, shown by the lobby when creating a
    // room. nullopt for modes without configurable parameters.
    std::optional<PassFiveGatesConfig> default_pass_five_gates;
};

// Ordered built-in presets (index 0 is the default "standard").
[[nodiscard]] const std::vector<ModePreset>& ModePresets();

[[nodiscard]] const ModePreset* FindModePreset(std::string_view id);
[[nodiscard]] const ModePreset* FindModePreset(GameMode mode);

// Wire id (e.g. "pass_five_gates") / display name (e.g. "过五关").
[[nodiscard]] std::string_view GameModeName(GameMode mode);
[[nodiscard]] std::string_view ModeDisplayName(GameMode mode);
[[nodiscard]] std::optional<GameMode> ParseGameMode(std::string_view id);

}  // namespace mmcr::game
