#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mmcr::game {

// Play mode selector. The fan library (qingque) is untouched; modes only
// change session-level flow and victory conditions.
enum class GameMode : std::uint8_t {
    kStandard,
    kPassFiveGates,
};

[[nodiscard]] std::string_view GameModeName(GameMode mode);
[[nodiscard]] std::optional<GameMode> ParseGameMode(std::string_view id);

// Strongly-typed per-mode configuration for 过五关 (pass five gates).
//
// This replaces the loose JSON blob the mode previously read from
// GameConfig::mode_config: mode logic now consumes typed fields only, and
// JSON <-> struct conversion happens at the server boundary (request parsing)
// and the record layer (replay serialization), never inside the game engine.
struct PassFiveGatesConfig {
    // A team is knocked out once its score falls below -knockout_score;
    // 0 (the default) disables knockout.
    int knockout_score{0};

    // Server-injected team assignment keyed by player id (0 = 虎, 1 = 龙).
    // Populated by the hub from the pending room's seat picks; clients can
    // never write this key.
    std::unordered_map<std::int64_t, int> team{};

    [[nodiscard]] auto operator==(const PassFiveGatesConfig&) const -> bool = default;
};

struct GameConfig {
    static constexpr int afk_timeout_times{3};
    static constexpr int afk_tolerance_ms{5000};
    static constexpr int dead_time{5000};
    static constexpr int epsilon_ms{10};
    static constexpr int pass_margin_ms{500};
    static constexpr int primary_timer_min_ms{3000};
    static constexpr int primary_timer_max_ms{15000};
    static constexpr int secondary_timer_min_ms{3000};
    static constexpr int auxiliary_timer_min_ms{0};
    static constexpr int auxiliary_timer_max_ms{45000};
    static constexpr int round_count_min{1};
    static constexpr int round_count_max{32};
    static constexpr int minimal_transition_ms{400};
    static constexpr int network_delay_ms{100};
    static constexpr int meld_offset_ms{1450};
    static constexpr int meld_pause_ms{770};
    static constexpr int random_pause_range{1450};
    static constexpr int round_interval_ms{7500};
    static constexpr double random_pause_prob{0.11};

    int primary_timer_ms{7000};
    int secondary_timer_ms{4000};
    int auxiliary_timer_ms{12000};
    int round_count{16};
    int seat_shuffle_period{4};
    bool recorded{true};
    bool debug_mode{false};
    bool unranked{false};
    bool public_session{true};
    // 击飞 (forced end floor): when set, a round whose settlement leaves any
    // seat below the floor ends the session instead of starting the next
    // round. nullopt keeps normal behavior. Allowed values: null, -1500, -2000.
    std::optional<int> forced_end_floor{std::nullopt};
    GameMode mode{GameMode::kStandard};
    // Engaged when mode == kPassFiveGates; nullopt for modes without config.
    std::optional<PassFiveGatesConfig> pass_five_gates;

    static constexpr int with_margin(int base) {
        return base + network_delay_ms;
    }
};

}  // namespace mmcr::game
