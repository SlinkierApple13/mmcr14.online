#include "stats/stats_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "storage/migration.h"

namespace mmcr::stats {
namespace {

auto TimeRangeBegin(std::vector<const RoundEntry*>::const_iterator begin,
                    std::vector<const RoundEntry*>::const_iterator end,
                    std::int64_t time_end) {
    return std::partition_point(begin, end, [time_end](const RoundEntry* entry) {
        return entry->timestamp_ms > time_end;
    });
}

auto TimeRangeEnd(std::vector<const RoundEntry*>::const_iterator begin,
                  std::vector<const RoundEntry*>::const_iterator end,
                  std::int64_t time_start) {
    return std::partition_point(begin, end, [time_start](const RoundEntry* entry) {
        return entry->timestamp_ms >= time_start;
    });
}

void SortStatsRounds(std::vector<const RoundEntry*>& rounds,
                     std::string_view sort_field,
                     std::string_view sort_order) {
    const bool descending = sort_order != "asc";
    if (sort_field != "fan") {
        if (!descending) {
            std::reverse(rounds.begin(), rounds.end());
        }
        return;
    }

    struct FanBucket {
        double fan{0.0};
        std::vector<const RoundEntry*> rounds_time_desc;
    };

    std::unordered_map<double, std::size_t> bucket_indices;
    bucket_indices.reserve(rounds.size());
    std::vector<FanBucket> buckets;
    buckets.reserve(rounds.size());
    for (const auto* round : rounds) {
        const double fan = round == nullptr ? 0.0 : round->fan;
        auto [index_it, inserted] = bucket_indices.emplace(fan, buckets.size());
        if (inserted) {
            buckets.push_back(FanBucket{.fan = fan});
        }
        buckets[index_it->second].rounds_time_desc.push_back(round);
    }

    std::sort(buckets.begin(), buckets.end(), [descending](const FanBucket& left, const FanBucket& right) {
        return descending ? (left.fan > right.fan) : (left.fan < right.fan);
    });

    rounds.clear();
    for (auto& bucket : buckets) {
        if (descending) {
            rounds.insert(rounds.end(), bucket.rounds_time_desc.begin(), bucket.rounds_time_desc.end());
        } else {
            rounds.insert(rounds.end(), bucket.rounds_time_desc.rbegin(), bucket.rounds_time_desc.rend());
        }
    }
}

}  // namespace

StatsService::StatsService(storage::Database* database)
    : database_(database) {}

util::Status StatsService::InitializeSchema(const std::filesystem::path& migrations_dir) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    storage::MigrationRunner runner(database_);
    return runner.ApplyDirectory(migrations_dir);
}

util::Status StatsService::LoadFromDatabase() {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto statement = database_->Prepare(
        "SELECT session_identifier, round_number, drawn_game, winner_seat, from_seat, win_type_bits, "
        "win_tile, turn, time_ms, fan_text, meld_count_json, players_json, fan_results_json, "
        "winning_hand_json FROM stats_round_entries;");
    if (!statement.ok()) {
        return statement.status();
    }

    rounds_.clear();
    players_.clear();

    for (;;) {
        auto step = statement.value().Step();
        if (!step.ok()) {
            return step.status();
        }
        if (step.value() == storage::Statement::StepResult::kDone) {
            break;
        }

        RoundEntry entry;
        entry.round_key.session_identifier = statement.value().ColumnText(0);
        entry.round_key.round_number = static_cast<std::uint64_t>(statement.value().ColumnInt64(1));
        entry.drawn_game = statement.value().ColumnInt64(2) != 0;
        entry.winner_seat = static_cast<int>(statement.value().ColumnInt64(3));
        entry.from_seat = static_cast<int>(statement.value().ColumnInt64(4));
        entry.win_type = mahjong::win_type(static_cast<mahjong::win_t>(statement.value().ColumnInt64(5)));
        const auto stored_win_tile = statement.value().ColumnInt64(6);
        entry.win_tile = stored_win_tile < 0 ? mahjong::tile::invalid : static_cast<mahjong::tile_t>(stored_win_tile);
        entry.turn = statement.value().ColumnInt64(7);
        entry.timestamp_ms = statement.value().ColumnInt64(8);

        auto fan = ParseFanText(statement.value().ColumnText(9));
        if (!fan.ok()) {
            return fan.status();
        }
        entry.fan = fan.value();

        auto meld_count = ParseMeldCountJson(statement.value().ColumnText(10));
        if (!meld_count.ok()) {
            return meld_count.status();
        }
        entry.meld_count = meld_count.value();

        auto players = ParsePlayersJson(statement.value().ColumnText(11));
        if (!players.ok()) {
            return players.status();
        }
        entry.players = players.value();

        auto fan_results = ParseFanResultsJson(statement.value().ColumnText(12));
        if (!fan_results.ok()) {
            return fan_results.status();
        }
        entry.fan_results = fan_results.value();
        entry.fan_ids = FanIdsFromResults(entry.fan_results);

        auto winning_hand = ParseWinningHandJson(statement.value().ColumnText(13));
        if (!winning_hand.ok()) {
            return winning_hand.status();
        }
        entry.winning_hand = std::move(winning_hand.value());

        rounds_[entry.round_key] = std::move(entry);
    }

    rebuild_indexes_locked();
    ++version_;
    return util::Status::Ok();
}

util::Status StatsService::UpsertRoundRecord(const Json::Value& record) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    auto round_result = ReadRequiredObject(record, "round_result");
    if (!round_result.ok()) {
        return round_result.status();
    }
    if (!(*round_result.value())["completed"].isBool()) {
        return util::Status::InvalidArgument("round_result.completed must be a bool");
    }
    if (!(*round_result.value())["completed"].asBool()) {
        return util::Status::Ok();
    }

    auto projected = ProjectRoundRecord(record);
    if (!projected.ok()) {
        return projected.status();
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto status = persist_round_locked(projected.value());
    if (!status.ok()) {
        return status;
    }

    rounds_[projected.value().round_key] = projected.value();
    rebuild_indexes_locked();
    ++version_;
    return util::Status::Ok();
}

std::vector<const RoundEntry*> StatsService::ListAllRounds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rounds_by_time_desc_;
}

util::Status StatsService::ImportRecordsFromDirectory(const std::filesystem::path& records_root) {
    if (!std::filesystem::exists(records_root)) {
        return util::Status::NotFound("records root does not exist: " + records_root.string());
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(records_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        std::ifstream stream(entry.path());
        if (!stream.is_open()) {
            return util::Status::Internal("failed to open stats record: " + entry.path().string());
        }

        Json::CharReaderBuilder builder;
        Json::Value parsed;
        std::string errors;
        if (!Json::parseFromStream(builder, stream, &parsed, &errors)) {
            return util::Status::InvalidArgument("failed to parse stats record: " + errors);
        }

        auto status = UpsertRoundRecord(parsed);
        if (!status.ok()) {
            return status;
        }
    }

    return util::Status::Ok();
}

util::StatusOr<RoundCollection> StatsService::Query(const StatsFilter& filter) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    RoundCollection collection(filter.player_id);

    const auto begin = TimeRangeBegin(rounds_by_time_desc_.begin(), rounds_by_time_desc_.end(), filter.time_end);
    const auto end = TimeRangeEnd(begin, rounds_by_time_desc_.end(), filter.time_start);
    for (auto it = begin; it != end; ++it) {
        const auto* entry = *it;
        if (filter.matches(*entry)) {
            collection.add_round(entry);
        }
    }

    return collection;
}

util::StatusOr<RoundPage> StatsService::ListRounds(const StatsFilter& filter,
                              std::string_view sort_field,
                              std::string_view sort_order,
                              std::size_t offset,
                              std::size_t limit) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<const RoundEntry*> rounds;
    const auto range_begin = TimeRangeBegin(rounds_by_time_desc_.begin(), rounds_by_time_desc_.end(), filter.time_end);
    const auto range_end = TimeRangeEnd(range_begin, rounds_by_time_desc_.end(), filter.time_start);
    rounds.reserve(static_cast<std::size_t>(std::distance(range_begin, range_end)));
    for (auto it = range_begin; it != range_end; ++it) {
        const auto* entry = *it;
        if (filter.matches(*entry)) {
            rounds.push_back(entry);
        }
    }

    SortStatsRounds(rounds, sort_field, sort_order);

    RoundPage page;
    page.total_count = rounds.size();
    if (offset >= rounds.size()) {
        return page;
    }
    const auto page_end = std::min(rounds.size(), offset + limit);
    page.rounds.assign(rounds.begin() + static_cast<std::ptrdiff_t>(offset),
                       rounds.begin() + static_cast<std::ptrdiff_t>(page_end));
    return page;
}

std::vector<RoundPlayer> StatsService::ListPlayers() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<RoundPlayer> players;
    players.reserve(players_.size());
    for (const auto& [_, player] : players_) {
        players.push_back(player);
    }
    std::sort(players.begin(), players.end(), [](const RoundPlayer& left, const RoundPlayer& right) {
        if (left.username != right.username) {
            return left.username < right.username;
        }
        return left.player_id < right.player_id;
    });
    return players;
}

std::size_t StatsService::round_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rounds_.size();
}

std::uint64_t StatsService::version() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return version_;
}

void StatsService::rebuild_indexes_locked() {
    rebuild_player_index_locked();
    rebuild_time_index_locked();
}

void StatsService::rebuild_player_index_locked() {
    players_.clear();
    for (const auto& [_, entry] : rounds_) {
        for (const auto& player : entry.players) {
            if (player.player_id == 0) {
                continue;
            }
            players_[player.player_id] = player;
        }
    }
}

void StatsService::rebuild_time_index_locked() {
    rounds_by_time_desc_.clear();
    rounds_by_time_desc_.reserve(rounds_.size());
    for (const auto& [_, entry] : rounds_) {
        rounds_by_time_desc_.push_back(&entry);
    }
    std::sort(rounds_by_time_desc_.begin(), rounds_by_time_desc_.end(), [](const RoundEntry* left, const RoundEntry* right) {
        if (left->timestamp_ms != right->timestamp_ms) {
            return left->timestamp_ms > right->timestamp_ms;
        }
        if (left->fan != right->fan) {
            return left->fan > right->fan;
        }
        if (left->round_key.session_identifier != right->round_key.session_identifier) {
            return left->round_key.session_identifier > right->round_key.session_identifier;
        }
        return left->round_key.round_number > right->round_key.round_number;
    });
}

util::Status StatsService::persist_round_locked(const RoundEntry& entry) {
    auto statement = database_->Prepare(
        "INSERT OR REPLACE INTO stats_round_entries("
        "session_identifier, round_number, drawn_game, winner_seat, from_seat, win_type_bits, "
        "win_tile, turn, time_ms, fan_text, meld_count_json, players_json, fan_results_json, "
        "winning_hand_json) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14);");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, entry.round_key.session_identifier);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(2, static_cast<std::int64_t>(entry.round_key.round_number));
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, entry.drawn_game ? 1 : 0);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(4, entry.winner_seat);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(5, entry.from_seat);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(6, static_cast<std::int64_t>(static_cast<mahjong::win_t>(entry.win_type)));
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(7, entry.win_tile == mahjong::tile::invalid ? -1 : entry.win_tile);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(8, entry.turn);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(9, entry.timestamp_ms);
    if (!status.ok()) {
        return status;
    }
    std::ostringstream fan_stream;
    fan_stream << entry.fan;
    status = stmt.BindText(10, fan_stream.str());
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(11, EncodeMeldCountJson(entry.meld_count));
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(12, EncodePlayersJson(entry.players));
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(13, EncodeFanResultsJson(entry.fan_results));
    if (!status.ok()) {
        return status;
    }
    const auto winning_hand_json = EncodeWinningHandJson(entry.winning_hand);
    if (winning_hand_json.empty()) {
        status = stmt.BindNull(14);
    } else {
        status = stmt.BindText(14, winning_hand_json);
    }
    if (!status.ok()) {
        return status;
    }
    return StepDone(stmt);
}

}  // namespace mmcr::stats
