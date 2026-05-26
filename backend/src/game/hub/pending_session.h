#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <shared_mutex>

#include <jsoncpp/json/json.h>

#include "auth/service.h"
#include "game/config.h"
#include "game/engine/timer.h"
#include "util/status.h"

namespace mmcr::game {

class GameHub;

struct QueueConfig {
    int empty_timeout_ms{10000};
    bool public_session{true};
    bool singleplayer{false};
};

struct PendingSeat {
    int seat_index{-1};
    auth::PlayerProfilePtr player;
    bool ready{false};
};

struct PendingSessionSummary {
    std::int64_t session_id{0};
    int occupied_seat_count{0};
    int ready_seat_count{0};
    int primary_timer_ms{7000};
    int secondary_timer_ms{4000};
    int auxiliary_timer_ms{12000};
    int round_count{16};
    bool recorded{true};
    bool debug_mode{false};
    bool public_session{true};
    bool can_join{true};
    bool can_start{false};
    std::vector<std::string> names;
};

struct PendingSessionSnapshot {
    PendingSessionSummary summary;
    std::array<PendingSeat, 4> seats{};
};

class PendingSession {
public:
    PendingSession(GameHub* hub, std::int64_t session_id, GameConfig game_config, QueueConfig queue_config);

    [[nodiscard]] auto session_id() const noexcept -> std::int64_t {
        return session_id_;
    }

    [[nodiscard]] auto is_empty() const -> bool;
    [[nodiscard]] auto is_full() const -> bool;
    [[nodiscard]] auto all_ready() const -> bool;
    [[nodiscard]] auto game_config() const -> const GameConfig& {
        return game_config_;
    }
    [[nodiscard]] auto queue_config() const -> const QueueConfig& {
        return queue_config_;
    }
    [[nodiscard]] auto seats() const -> const std::array<PendingSeat, 4>& {
        return seats_;
    }

    [[nodiscard]] auto collect_invalid_players() -> std::vector<std::int64_t>;
    [[nodiscard]] auto empty_timeout_elapsed() const -> bool;
    [[nodiscard]] auto join_player(auth::PlayerProfilePtr player) -> util::Status;
    [[nodiscard]] auto player_leaves(std::int64_t player_id) -> util::Status;
    [[nodiscard]] auto player_ready(std::int64_t player_id, bool ready) -> util::Status;
    void ensure_empty_timer();
    void reset_empty_timer();

private:
    [[nodiscard]] auto is_empty_locked() const -> bool;
    void send_message(std::int64_t player_id, const Json::Value& message);

    QueueConfig queue_config_;
    GameHub* hub_{nullptr};
    std::int64_t session_id_{0};
    mutable std::shared_mutex mutex_;
    std::array<PendingSeat, 4> seats_{};
    GameConfig game_config_;
    bool empty_timeout_elapsed_{false};
    Timer empty_session_timer_;
};

}  // namespace mmcr::game