#include "game/mode/mode_registry.h"

#include <utility>

namespace mmcr::game {

namespace {

const std::vector<ModePreset>& BuildPresets() {
    static const std::vector<ModePreset> presets = [] {
        std::vector<ModePreset> result;
        result.push_back(ModePreset{
            .id = "standard",
            .name = "标准",
            .mode = GameMode::kStandard,
            .default_pass_five_gates = std::nullopt,
        });
        result.push_back(ModePreset{
            .id = "pass_five_gates",
            .name = "过五关",
            .mode = GameMode::kPassFiveGates,
            .default_pass_five_gates = PassFiveGatesConfig{},
        });
        return result;
    }();
    return presets;
}

}  // namespace

const std::vector<ModePreset>& ModePresets() {
    return BuildPresets();
}

const ModePreset* FindModePreset(std::string_view id) {
    for (const auto& preset : ModePresets()) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

const ModePreset* FindModePreset(GameMode mode) {
    for (const auto& preset : ModePresets()) {
        if (preset.mode == mode) {
            return &preset;
        }
    }
    return nullptr;
}

std::string_view GameModeName(GameMode mode) {
    if (const ModePreset* preset = FindModePreset(mode); preset != nullptr) {
        return preset->id;
    }
    return "standard";
}

std::string_view ModeDisplayName(GameMode mode) {
    if (const ModePreset* preset = FindModePreset(mode); preset != nullptr) {
        return preset->name;
    }
    return "标准";
}

std::optional<GameMode> ParseGameMode(std::string_view id) {
    if (const ModePreset* preset = FindModePreset(id); preset != nullptr) {
        return preset->mode;
    }
    return std::nullopt;
}

}  // namespace mmcr::game
