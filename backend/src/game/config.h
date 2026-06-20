#pragma once

namespace mmcr::game {

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

    static constexpr int with_margin(int base) {
        return base + network_delay_ms;
    }
};

}  // namespace mmcr::game