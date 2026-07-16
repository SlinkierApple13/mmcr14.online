#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <jsoncpp/json/json.h>

#include "external/qingque/basic/mahjong.h"
#include "external/qingque/rules/qingque.h"
#include "game/engine/hand.h"
#include "storage/database.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::stats {

struct RoundKey {
    std::string session_identifier;
    std::uint64_t round_number{0};

    auto operator==(const RoundKey&) const -> bool = default;
};

struct RoundKeyHash {
    [[nodiscard]] auto operator()(const RoundKey& key) const noexcept -> std::size_t;
};

struct FanCodeHash {
    [[nodiscard]] auto operator()(const qingque::fan_code& code) const noexcept -> std::size_t;
};

struct RoundPlayer {
    int seat_index{0};
    std::int64_t player_id{0};
    std::string username;
};

struct RoundEntry {
    bool drawn_game{false};
    std::array<RoundPlayer, 4> players{};
    int winner_seat{-1};
    int from_seat{-1};
    mahjong::win_type win_type{0};
    mahjong::tile_t win_tile{mahjong::tile::invalid};
    std::array<int, 4> meld_count{};
    RoundKey round_key;
    std::int64_t turn{0};
    std::int64_t timestamp_ms{0};
    double fan{0.0};
    std::vector<qingque::fan_code> fan_results;
    std::vector<int> fan_ids;
    std::optional<game::HandWrapper> winning_hand;

    [[nodiscard]] auto has_player(std::int64_t player_id) const -> bool;
    [[nodiscard]] auto winner_player_id() const -> std::int64_t;
    [[nodiscard]] auto from_player_id() const -> std::int64_t;
    [[nodiscard]] auto winner_username() const -> std::string_view;
    [[nodiscard]] auto from_username() const -> std::string_view;
    [[nodiscard]] auto self_drawn() const -> bool;
    [[nodiscard]] auto has_win_type(mahjong::win_t flag) const -> bool;
    [[nodiscard]] auto pt_gain(std::int64_t player_id) const -> int;
    [[nodiscard]] auto player_meld_count(std::int64_t player_id) const -> int;
};

struct FanStatEntry {
    qingque::fan_code fan_result{};
    std::vector<const RoundEntry*> occurrences;

    [[nodiscard]] auto fan() const -> double;
};

struct FanCompositionStat {
    std::string label;
    std::string fan_names;
    double fan_value{0.0};
    std::uint64_t fan_pt{0};
    std::size_t exact_count{0};
    std::size_t inclusive_count{0};
};

struct SingleFanStat {
    int fan_id{0};
    std::string fan_name;
    std::size_t occurrence_count{0};
    double occurrence_rate{0.0};
};

struct RoundCollection {
    std::optional<std::int64_t> player_id;

    std::uint64_t tot_win_pt{0};
    std::uint64_t tot_hwin_pt{0};
    std::uint64_t tot_rkwin_pt{0};
    std::uint64_t tot_shoot_pt{0};
    std::uint64_t tot_selfdrawned_pt{0};
    std::uint64_t tot_win_turn{0};
    std::uint64_t tot_hwin_turn{0};
    std::uint64_t tot_rkwin_turn{0};
    std::uint64_t tot_shoot_turn{0};
    std::uint64_t tot_selfdrawned_turn{0};
    std::uint64_t tot_turn{0};
    std::int64_t tot_round_pt{0};
    std::uint64_t tot_meld_count{0};
    std::uint64_t tot_wins{0};
    std::uint64_t tot_hwins{0};
    std::uint64_t tot_shoots{0};
    std::uint64_t tot_selfdrawneds{0};
    std::uint64_t tot_drawn_games{0};
    std::uint64_t tot_meld_games{0};

    std::vector<const RoundEntry*> rounds;
    std::unordered_map<qingque::fan_code, FanStatEntry, FanCodeHash> fan_stats;
    std::unordered_map<qingque::fan_code, FanStatEntry, FanCodeHash> fan_stats_no_superior;
    std::unordered_map<qingque::fan_code, FanStatEntry, FanCodeHash> fan_stats_precise;

    RoundCollection() = default;
    explicit RoundCollection(std::optional<std::int64_t> player_id_value);

    void add_round(const RoundEntry* entry);

    [[nodiscard]] auto avg_win_pt() const -> double;
    [[nodiscard]] auto avg_hwin_pt() const -> double;
    [[nodiscard]] auto avg_rkwin_pt() const -> double;
    [[nodiscard]] auto avg_shoot_pt() const -> double;
    [[nodiscard]] auto avg_selfdrawned_pt() const -> double;
    [[nodiscard]] auto avg_win_turn() const -> double;
    [[nodiscard]] auto avg_hwin_turn() const -> double;
    [[nodiscard]] auto avg_rkwin_turn() const -> double;
    [[nodiscard]] auto avg_shoot_turn() const -> double;
    [[nodiscard]] auto avg_selfdrawned_turn() const -> double;
    [[nodiscard]] auto avg_turn() const -> double;
    [[nodiscard]] auto avg_round_pt() const -> double;
    [[nodiscard]] auto avg_meld_count() const -> double;
    [[nodiscard]] auto win_rate() const -> double;
    [[nodiscard]] auto hwin_rate() const -> double;
    [[nodiscard]] auto shoot_rate() const -> double;
    [[nodiscard]] auto selfdrawned_rate() const -> double;
    [[nodiscard]] auto drawn_game_rate() const -> double;
    [[nodiscard]] auto meld_rate() const -> double;
    [[nodiscard]] auto single_fan_stats(bool exclude_superior_fans = false) const
        -> std::vector<SingleFanStat>;
    [[nodiscard]] auto fan_composition_stats(bool exclude_superior_fans = false) const
        -> std::vector<FanCompositionStat>;

    [[nodiscard]] auto ToJson(bool exclude_superior_fans = false) const -> Json::Value;
    [[nodiscard]] auto FanCompositionStatsJson(bool exclude_superior_fans = false) const -> Json::Value;
};

struct StatsFilter {
    std::vector<int> fan_filter_positive;
    std::vector<int> fan_filter_negative;
    std::vector<std::int64_t> player_filter_positive;
    std::vector<std::int64_t> player_filter_negative;
    std::optional<std::int64_t> player_id;
    std::optional<std::int64_t> win_player_id;
    std::vector<std::int64_t> win_player_filter_negative;
    std::optional<std::int64_t> from_player_id;
    std::vector<std::int64_t> from_player_filter_negative;
    std::vector<mahjong::win_t> win_type_filter_positive;
    std::vector<mahjong::win_t> win_type_filter_negative;
    std::optional<bool> self_drawn;
    bool exclude_superior_fans{true};
    bool nonstandard_only{false};
    std::int64_t time_start{0};
    std::int64_t time_end{std::numeric_limits<std::int64_t>::max()};
    double min_fan{0.0};
    double max_fan{std::numeric_limits<double>::max()};

    [[nodiscard]] auto matches(const RoundEntry& entry) const -> bool;
};

struct RoundPage {
    std::vector<const RoundEntry*> rounds;
    std::size_t total_count{0};
};

class StatsService {
public:
    explicit StatsService(storage::Database* database);

    [[nodiscard]] auto InitializeSchema(const std::filesystem::path& migrations_dir) -> util::Status;
    [[nodiscard]] auto LoadFromDatabase() -> util::Status;
    [[nodiscard]] auto UpsertRoundRecord(const Json::Value& record) -> util::Status;
    [[nodiscard]] auto ImportRecordsFromDirectory(const std::filesystem::path& records_root)
        -> util::Status;
    [[nodiscard]] auto Query(const StatsFilter& filter) const -> util::StatusOr<RoundCollection>;
    [[nodiscard]] auto ListRounds(const StatsFilter& filter,
                                  std::string_view sort_field = "time",
                                  std::string_view sort_order = "desc",
                                  std::size_t offset = 0,
                                  std::size_t limit = 50) const -> util::StatusOr<RoundPage>;
    [[nodiscard]] auto ListAllRounds() const -> std::vector<const RoundEntry*>;
    [[nodiscard]] auto ListPlayers() const -> std::vector<RoundPlayer>;
    [[nodiscard]] auto round_count() const -> std::size_t;
    [[nodiscard]] auto version() const -> std::uint64_t;

private:
    void rebuild_indexes_locked();
    void rebuild_player_index_locked();
    void rebuild_time_index_locked();
    [[nodiscard]] auto persist_round_locked(const RoundEntry& entry) -> util::Status;

    storage::Database* database_;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<RoundKey, RoundEntry, RoundKeyHash> rounds_;
    std::vector<const RoundEntry*> rounds_by_time_desc_;
    std::unordered_map<std::int64_t, RoundPlayer> players_;
    std::uint64_t version_{0};
};

[[nodiscard]] auto ProjectRoundRecord(const Json::Value& record) -> util::StatusOr<RoundEntry>;

}  // namespace mmcr::stats