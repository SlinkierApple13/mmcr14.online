#pragma once

#include <jsoncpp/json/json.h>

#include "game/config.h"
#include "util/status_or.h"

namespace mmcr::game {

// Serialization helpers for per-mode configuration. These exist so that the
// JSON representation only ever crosses the server boundary (request parsing)
// and the record layer (replay headers): game engine code consumes the typed
// PassFiveGatesConfig directly and never touches JSON.

// Serializes config.pass_five_gates (when engaged) into the replay-header
// shape used for mode_config; returns JSON null when no mode config applies.
[[nodiscard]] Json::Value SerializeModeConfigJson(const GameConfig& config);

// Parses + validates a client-supplied mode_config JSON object into a typed
// PassFiveGatesConfig. Rejects unknown keys and out-of-range values.
[[nodiscard]] util::StatusOr<PassFiveGatesConfig> ParsePassFiveGatesConfig(
    const Json::Value& raw_mode_config);

}  // namespace mmcr::game
