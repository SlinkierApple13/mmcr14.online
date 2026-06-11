#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <jsoncpp/json/json.h>

#include "auth/service.h"
#include "external/qingque/basic/mahjong.h"
#include "game/config.h"
#include "game/engine/timer.h"
#include "game/engine/wall.h"
#include "game/rating_snapshot.h"
#include "util/my_queue.h"
#include "util/status.h"
#include "external/qingque/rules/qingque.h"

namespace mmcr::random {
class SeedContainer;
}

namespace mmcr::storage {
class GameRecordManager;
}

namespace mmcr::game {

class GameHub;

enum class EventKind : std::uint8_t {
    kNone,
    kStart,
    kPredraw,
    kDrawTile,
    kDiscardTile,
    kChow,
    kPung,
    kMeldedKong,
    kAddedKong,
    kConcealedKong,
    kDiscardWin,
    kRobAddedKongWin,
    kSelfDrawnWin,
    kPass,
    kFinalPass,
    kDrawnGame,
    kEnd,
    kPlayerLeft,
    kPlayerResumed,
};

struct RoundStartSnapshot {
    std::optional<uint64_t> seat_shuffle_seed; // empty if no shuffle
    std::vector<uint64_t> wall_seeds;
    std::array<int64_t, 4> player_ids; // in seat order
};

struct WinData {
    mahjong::hand h;
    double win_fan{0};
    int win_base_point{0};
    std::vector<qingque::fan_code> win_fan_codes;
    std::vector<std::string> win_fans;
};

struct Event {
    EventKind kind{EventKind::kStart};
    int actor_seat{-1};
    std::optional<mahjong::tile_t> tile;
    std::optional<bool> use_drawn_tile;
    std::optional<bool> draw_from_back;
    std::optional<bool> forced;
    std::vector<mahjong::tile_t> drawn_tiles{};
    std::optional<std::uint64_t> ui64_value; // auxiliary field
    std::optional<RoundStartSnapshot> round_start_snapshot;
    std::optional<WinData> win_data;
    std::optional<std::uint64_t> win_type_bits;
    std::optional<int> result_source_actor;
    std::vector<mahjong::tile_t> revealed_hand_tiles{};
    std::vector<int> final_scores{};
    std::optional<std::int64_t> round_turn;
    std::optional<std::vector<PlayerRatingSnapshot>> ratings;
    std::int64_t timestamp_ms{0};
    std::uint64_t stage_counter{0};
};

// bit 0: <1>23
// bit 1: 1<2>3
// bit 2: 12<3>
// bit 3: pung
// bit 4: melded_kong
// bit 5: discard_win
// bit 6: rob_added_kong_win
using MeldOptions = uint8_t;

namespace MeldOpFilter {
    constexpr MeldOptions kChow = 0b00000111;
    constexpr MeldOptions kPung = 0b00001000;
    constexpr MeldOptions kMeldedKong = 0b00010000;
    constexpr MeldOptions kDiscardWin = 0b00100000;
    constexpr MeldOptions kRobAddedKongWin = 0b01000000;
    constexpr std::array<MeldOptions, 4> kChows = {0b00000000, 0b00000001, 0b00000010, 0b00000100};
}

struct SelfMeldOptions {
    bool self_drawn_win{false};
    std::vector<mahjong::tile_t> ckong_from_hand;
    std::vector<mahjong::tile_t> ckong_from_draw;
    std::vector<mahjong::tile_t> akong_from_hand;
    std::vector<mahjong::tile_t> akong_from_draw;
};

struct State {
    mutable std::recursive_mutex mutex;
    std::uint64_t round_counter{0};
    std::uint64_t stage_counter{0};
    int this_priority{-1};
    std::optional<Event> next_transition;
    int current_player{-1};
};

struct MeldWrapper {
    mahjong::meld meld_value{mahjong::meld::invalid};
    int chow_mode{0}; // 0: none, 1: <1>23, 2: 1<2>3, 3: 12<3>
    int meld_from_rel{0}; // 0: none, 1: right, 2: opposite, 3: left
};

enum PendingStatus {
    kPendingNone,
    kPendingPrimary,
    kPendingSecondary,
    kPendingSlept,
};

struct FanData {
    std::pair<double, qingque::fan_code> w_discard{};
    std::pair<double, qingque::fan_code> w_drawn{};
};

struct WaitOptions : public std::unordered_map<mahjong::tile_t, FanData> {
    using std::unordered_map<mahjong::tile_t, FanData>::unordered_map;
    
    bool can_win_discard(mahjong::tile_t ti) const {
        auto it = find(ti);
        if (it == end()) {
            return false;
        }
        return it->second.w_discard.first > 1e-3;
    }

    bool can_win_drawn(mahjong::tile_t ti) const {
        auto it = find(ti);
        if (it == end()) {
            return false;
        }
        return it->second.w_drawn.first > 1e-3;
    }
};

struct Seat {
    [[nodiscard]] auto has_drawn_tile() const -> bool {
        return drawn_tile != mahjong::tile::invalid;
    }

    void reset() {
        discard_pile.clear();
        melds.clear();
        hand_tiles.clear();
        drawn_tile = mahjong::tile::invalid;
        score = 0;
        avail_melds_other = 0;
        avail_melds_self = {};
        disconnected = false;
    }

    auto remove_hand_tile(mahjong::tile_t tile) -> bool {
        const auto tile_it = std::find(hand_tiles.begin(), hand_tiles.end(), tile);
        if (tile_it == hand_tiles.end()) {
            return false;
        }

        hand_tiles.erase(tile_it);
        return true;
    }

    auto pop_discard() -> std::optional<mahjong::tile_t> {
        if (discard_pile.empty()) {
            return std::nullopt;
        }

        auto tile = discard_pile.back();
        discard_pile.pop_back();
        return tile;
    }

    auto is_afk() const -> bool {
        return afk_counter > GameConfig::afk_timeout_times || !player.valid();
    }

    auto resume() {
        afk_counter = 0;
        disconnected = false;
    }

    auto leave() {
        afk_counter = std::max(afk_counter, std::max(0, GameConfig::afk_timeout_times));
        disconnected = true;
    }

    void increment_afk_counter() {
        if (!is_afk()) {
            ++afk_counter;
        }
    }

    std::vector<mahjong::tile_t> discard_pile;
    std::vector<MeldWrapper> melds;
    std::vector<mahjong::tile_t> hand_tiles;
    mahjong::tile_t drawn_tile{mahjong::tile::invalid};
    auth::PlayerProfilePtr player{};
    MeldOptions avail_melds_other{0};
    WaitOptions wait_options{};
    SelfMeldOptions avail_melds_self{};
    PendingStatus pending{PendingStatus::kPendingNone};
    int afk_counter{0};
    int score{0};
    int auxiliary_ms{0};
    std::int64_t pending_from_ms{0};
    bool disconnected{false};
};

class ActiveSession {
public:
    ActiveSession(random::SeedContainer* seed_container,
                  GameHub* hub,
                  std::int64_t session_id,
                  std::array<auth::PlayerProfilePtr, 4> players,
                  GameConfig config,
                  storage::GameRecordManager* record_manager = nullptr);

    using SessionEndCallback = std::function<void(
        std::int64_t session_id,
        const std::array<std::int64_t, 4>& player_ids,
        const std::array<int, 4>& final_scores,
        int round_count)>;

    void set_session_end_callback(SessionEndCallback callback) {
        session_end_callback_ = std::move(callback);
    }

    [[nodiscard]] auto session_id() const noexcept -> std::int64_t {
        return session_id_;
    }

    [[nodiscard]] auto seats() -> std::array<Seat, 4>& {
        return seats_;
    }

    [[nodiscard]] auto seats() const -> const std::array<Seat, 4>& {
        return seats_;
    }

    [[nodiscard]] auto state() -> State& {
        return state_;
    }

    [[nodiscard]] auto state() const -> const State& {
        return state_;
    }

    [[nodiscard]] auto config() const -> const GameConfig& {
        return config_;
    }

    [[nodiscard]] auto public_session() const noexcept -> bool {
        return config_.public_session;
    }

    [[nodiscard]] auto ended() const noexcept -> bool {
        return ended_;
    }

    [[nodiscard]] auto ended_at_ms() const noexcept -> std::int64_t {
        return ended_at_ms_;
    }

    [[nodiscard]] auto rounds_played() const noexcept -> std::uint64_t {
        return state_.round_counter;
    }

    [[nodiscard]] auto handle_message(std::int64_t player_id, const Json::Value& message)
        -> util::Status;
    [[nodiscard]] auto player_leaves(std::int64_t player_id) -> util::Status;
    [[nodiscard]] auto player_resumes(std::int64_t player_id) -> util::Status;
    [[nodiscard]] auto build_snapshot_for_player_id(std::int64_t player_id) const
        -> util::StatusOr<Json::Value>;
    void end_session(std::int64_t timestamp_ms = 0, bool enqueue_record = true);

    [[nodiscard]] auto has_player(std::int64_t player_id) const -> bool {
        return find_seat_index(player_id).has_value();
    }

private:
    [[nodiscard]] auto find_seat_index(std::int64_t player_id) const -> std::optional<int>;
    [[nodiscard]] auto recording_enabled() const -> bool;
    void capture_round_record_state(const Event& transition);
    void enqueue_current_round_record();

    GameConfig config_;
    Wall wall_;
    std::array<Seat, 4> seats_{};
    std::vector<Event> transition_queue_;
    std::vector<Event> event_queue_;
    State state_;
    Timer transition_timer_;
    std::array<Timer, 4> pending_start_timers_{};
    std::array<std::optional<PendingStatus>, 4> scheduled_pending_{};
    std::array<bool, 4> interval_delayed_seats_{};
    std::int64_t last_claim_delivery_ms_{0};
    std::int64_t next_transition_not_before_ms_{0};
    random::SeedContainer* seed_container_{nullptr};
    GameHub* hub_{nullptr};
    storage::GameRecordManager* record_manager_{nullptr};
    SessionEndCallback session_end_callback_;
    std::int64_t session_id_{-1};
    std::uint64_t session_init_timestamp_ns_{0};
    std::string session_identifier_;
    bool ended_{false};
    std::int64_t ended_at_ms_{0};
    std::array<int, 4> final_scores_{};
    std::mt19937_64 random_pause_rng_;
    std::optional<RoundStartSnapshot> current_round_start_snapshot_;
    Json::Value current_round_initial_seats_{Json::arrayValue};
    std::size_t current_round_transition_start_index_{0};
    std::size_t current_round_event_start_index_{0};
    std::uint64_t current_round_number_{0};
    std::int64_t current_round_turn_{0};
    int current_round_turn_actor_{0};
    std::array<int, 4> current_round_meld_count_{};
    bool current_round_saved_{true};
    std::vector<PlayerRatingSnapshot> current_round_ratings_;
	std::vector<PlayerRatingSnapshot> final_round_ratings_;

    [[nodiscard]] auto handle_event(const Event& event) -> util::Status;

    void init();
    void execute_transition();
    void broadcast_claim(const Event& event);
    void process_transition(const Event& transition);
    [[nodiscard]] auto send_message(int target_seat, const Json::Value& message, int delay_ms = 0)
        -> int;
    void schedule_pending_start(int seat,
                                PendingStatus pending,
                                int delay_ms,
                                std::uint64_t stage_counter);
    [[nodiscard]] auto build_snapshot_for_player(
        int seat,
        const Event* context_event = nullptr) const -> Json::Value;
    [[nodiscard]] auto build_event_message_for_player(
        int seat,
        const Event& event,
        std::string_view category) const -> Json::Value;

    void set_timer(int delay_ms, uint64_t stage_counter);
    void set_timer_extend(int delay_ms, uint64_t stage_counter);
    void set_timer_shrink(int delay_ms, uint64_t stage_counter);

    void start_primary_wait(int seat, std::int64_t now);
    void start_secondary_wait(int seat, std::int64_t now);
    void end_wait(int seat, std::int64_t now);

    void flush_for_player(int seat);

    int get_random_pause();

    PendingStatus update_pending_status(int seat, std::int64_t now);
};

}  // namespace mmcr::game