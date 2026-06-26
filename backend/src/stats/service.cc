#include "stats/service.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "storage/migration.h"

namespace mmcr::stats {
namespace {

auto JsonToCompactString(const Json::Value& value) -> std::string {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    return Json::writeString(builder, value);
}

auto ParseJsonString(std::string_view raw) -> util::StatusOr<Json::Value> {
    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    std::istringstream stream{std::string(raw)};
    if (!Json::parseFromStream(builder, stream, &parsed, &errors)) {
        return util::Status::InvalidArgument("failed to parse stats JSON: " + errors);
    }
    return parsed;
}

auto ReadRequiredObject(const Json::Value& object,
                        std::string_view field_name) -> util::StatusOr<const Json::Value*> {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isObject()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be an object");
    }
    return &field;
}

auto ReadRequiredArray(const Json::Value& object,
                       std::string_view field_name) -> util::StatusOr<const Json::Value*> {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isArray()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be an array");
    }
    return &field;
}

auto ReadRequiredString(const Json::Value& object,
                        std::string_view field_name) -> util::StatusOr<std::string> {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isString()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be a string");
    }
    return field.asString();
}

auto ReadRequiredInt64(const Json::Value& object,
                       std::string_view field_name) -> util::StatusOr<std::int64_t> {
    const Json::Value& field = object[std::string(field_name)];
    if (field.isInt64()) {
        return field.asInt64();
    }
    if (field.isInt()) {
        return static_cast<std::int64_t>(field.asInt());
    }
    if (field.isUInt64()) {
        return static_cast<std::int64_t>(field.asUInt64());
    }
    if (field.isUInt()) {
        return static_cast<std::int64_t>(field.asUInt());
    }
    return util::Status::InvalidArgument(std::string(field_name) + " must be an integer");
}

auto ReadRequiredUInt64(const Json::Value& object,
                        std::string_view field_name) -> util::StatusOr<std::uint64_t> {
    auto value = ReadRequiredInt64(object, field_name);
    if (!value.ok()) {
        return value.status();
    }
    if (value.value() < 0) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be non-negative");
    }
    return static_cast<std::uint64_t>(value.value());
}

auto ReadRequiredDouble(const Json::Value& object,
                        std::string_view field_name) -> util::StatusOr<double> {
    const Json::Value& field = object[std::string(field_name)];
    if (!field.isDouble() && !field.isInt() && !field.isUInt() && !field.isInt64() && !field.isUInt64()) {
        return util::Status::InvalidArgument(std::string(field_name) + " must be numeric");
    }
    return field.asDouble();
}

auto ParsePlayers(const Json::Value& initial_seats) -> util::StatusOr<std::array<RoundPlayer, 4>> {
    if (!initial_seats.isArray() || initial_seats.size() != 4) {
        return util::Status::InvalidArgument("initial_seats must contain exactly four seats");
    }

    std::array<RoundPlayer, 4> players;
    for (Json::ArrayIndex index = 0; index < initial_seats.size(); ++index) {
        const auto& seat = initial_seats[index];
        if (!seat.isObject()) {
            return util::Status::InvalidArgument("initial_seats entries must be objects");
        }

        players[index].seat_index = static_cast<int>(index);
        const Json::Value& player_id = seat["player_id"];
        if (player_id.isInt64()) {
            players[index].player_id = player_id.asInt64();
        } else if (player_id.isInt()) {
            players[index].player_id = player_id.asInt();
        } else if (player_id.isUInt64()) {
            players[index].player_id = static_cast<std::int64_t>(player_id.asUInt64());
        } else {
            players[index].player_id = 0;
        }

        const Json::Value& player_name = seat["player_name"];
        players[index].username = player_name.isString() ? player_name.asString() : std::string();
    }

    return players;
}

auto EncodePlayersJson(const std::array<RoundPlayer, 4>& players) -> std::string {
    Json::Value payload(Json::arrayValue);
    for (const auto& player : players) {
        Json::Value entry(Json::objectValue);
        entry["seat_index"] = player.seat_index;
        entry["player_id"] = Json::Int64(player.player_id);
        entry["username"] = player.username;
        payload.append(std::move(entry));
    }
    return JsonToCompactString(payload);
}

auto ParsePlayersJson(std::string_view raw) -> util::StatusOr<std::array<RoundPlayer, 4>> {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray() || parsed.value().size() != 4) {
        return util::Status::InvalidArgument("players_json must contain four entries");
    }

    std::array<RoundPlayer, 4> players;
    for (Json::ArrayIndex index = 0; index < parsed.value().size(); ++index) {
        const auto& entry = parsed.value()[index];
        players[index].seat_index = static_cast<int>(index);
        players[index].player_id = entry["player_id"].isInt64() ? entry["player_id"].asInt64() : 0;
        players[index].username = entry["username"].isString() ? entry["username"].asString() : std::string();
    }
    return players;
}

auto EncodeMeldCountJson(const std::array<int, 4>& meld_count) -> std::string {
    Json::Value payload(Json::arrayValue);
    for (const int count : meld_count) {
        payload.append(count);
    }
    return JsonToCompactString(payload);
}

auto ParseMeldCountJson(std::string_view raw) -> util::StatusOr<std::array<int, 4>> {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray() || parsed.value().size() != 4) {
        return util::Status::InvalidArgument("meld_count_json must contain four integers");
    }

    std::array<int, 4> meld_count{};
    for (Json::ArrayIndex index = 0; index < parsed.value().size(); ++index) {
        meld_count[index] = parsed.value()[index].isInt() ? parsed.value()[index].asInt() : 0;
    }
    return meld_count;
}

auto EncodeFanResultsJson(const std::vector<qingque::fan_code>& fan_results) -> std::string {
    Json::Value payload(Json::arrayValue);
    for (const auto& fan_result : fan_results) {
        payload.append(fan_result.to_string());
    }
    return JsonToCompactString(payload);
}

auto ParseFanResultsJson(std::string_view raw) -> util::StatusOr<std::vector<qingque::fan_code>> {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray()) {
        return util::Status::InvalidArgument("fan_results_json must be an array");
    }

    std::vector<qingque::fan_code> fan_results;
    fan_results.reserve(parsed.value().size());
    for (const auto& entry : parsed.value()) {
        if (!entry.isString()) {
            return util::Status::InvalidArgument("fan_results_json must contain strings");
        }
        fan_results.emplace_back(entry.asString());
    }
    return fan_results;
}

auto EncodeFanNamesJson(const std::vector<std::string>& fan_names) -> std::string {
    Json::Value payload(Json::arrayValue);
    for (const auto& fan_name : fan_names) {
        payload.append(fan_name);
    }
    return JsonToCompactString(payload);
}

auto ParseFanNamesJson(std::string_view raw) -> util::StatusOr<std::vector<std::string>> {
    auto parsed = ParseJsonString(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (!parsed.value().isArray()) {
        return util::Status::InvalidArgument("fan_names_json must be an array");
    }

    std::vector<std::string> fan_names;
    fan_names.reserve(parsed.value().size());
    for (const auto& entry : parsed.value()) {
        if (entry.isString()) {
            fan_names.push_back(entry.asString());
        }
    }
    return fan_names;
}

auto StepDone(storage::Statement& statement) -> util::Status {
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() != storage::Statement::StepResult::kDone) {
        return util::Status::Internal("statement unexpectedly returned a row");
    }
    return util::Status::Ok();
}

auto ParseFanText(std::string_view raw) -> util::StatusOr<double> {
    try {
        return std::stod(std::string(raw));
    } catch (...) {
        return util::Status::InvalidArgument("invalid stored fan value");
    }
}

auto IsNonstandardSession(std::string_view session_identifier) -> bool {
    const auto underscore = session_identifier.find('_');
    const auto prefix = underscore == std::string_view::npos
        ? session_identifier
        : session_identifier.substr(0, underscore);
    try {
        return std::stoll(std::string(prefix)) >= 1'000'000;
    } catch (...) {
        return false;
    }
}

}  // namespace

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

auto RoundKeyHash::operator()(const RoundKey& key) const noexcept -> std::size_t {
    const auto first = std::hash<std::string>{}(key.session_identifier);
    const auto second = std::hash<std::uint64_t>{}(key.round_number);
    return first ^ (second << 1);
}

auto FanCodeHash::operator()(const qingque::fan_code& code) const noexcept -> std::size_t {
    return std::hash<std::string>{}(code.to_string());
}

auto RoundEntry::has_player(std::int64_t player_id) const -> bool {
    return std::any_of(players.begin(), players.end(), [player_id](const RoundPlayer& player) {
        return player.player_id == player_id;
    });
}

auto RoundEntry::winner_player_id() const -> std::int64_t {
    if (winner_seat < 0 || winner_seat >= static_cast<int>(players.size())) {
        return 0;
    }
    return players[static_cast<std::size_t>(winner_seat)].player_id;
}

auto RoundEntry::from_player_id() const -> std::int64_t {
    if (from_seat < 0 || from_seat >= static_cast<int>(players.size())) {
        return 0;
    }
    return players[static_cast<std::size_t>(from_seat)].player_id;
}

auto RoundEntry::winner_username() const -> std::string_view {
    if (winner_seat < 0 || winner_seat >= static_cast<int>(players.size())) {
        return {};
    }
    return players[static_cast<std::size_t>(winner_seat)].username;
}

auto RoundEntry::from_username() const -> std::string_view {
    if (from_seat < 0 || from_seat >= static_cast<int>(players.size())) {
        return {};
    }
    return players[static_cast<std::size_t>(from_seat)].username;
}

auto RoundEntry::self_drawn() const -> bool {
    return win_type(mahjong::win_type::self_drawn);
}

auto RoundEntry::has_win_type(mahjong::win_t flag) const -> bool {
    return win_type(flag);
}

auto RoundEntry::pt_gain(std::int64_t player_id) const -> int {
    if (drawn_game || player_id == 0) {
        return 0;
    }
    const int points = static_cast<int>(3 * std::round(fan * fan));
    if (winner_player_id() == player_id) {
        return points;
    }
    if (!self_drawn() && from_player_id() == player_id) {
        return -points;
    }
    return self_drawn() ? -static_cast<int>(std::round(fan * fan)) : 0;
}

auto RoundEntry::player_meld_count(std::int64_t player_id) const -> int {
    for (const auto& player : players) {
        if (player.player_id == player_id) {
            return meld_count[static_cast<std::size_t>(player.seat_index)];
        }
    }
    return 0;
}

auto FanStatEntry::fan() const -> double {
    return occurrences.empty() ? 0.0 : occurrences.front()->fan;
}

RoundCollection::RoundCollection(std::optional<std::int64_t> player_id_value)
    : player_id(player_id_value) {}

void RoundCollection::add_round(const RoundEntry* entry) {
    auto match_player = [&](std::int64_t candidate_player_id) {
        return !player_id.has_value() || (candidate_player_id != 0 && *player_id == candidate_player_id);
    };

    const bool player_won = !entry->drawn_game && match_player(entry->winner_player_id());
    const bool player_self_won = player_won && entry->self_drawn();
    const bool player_ron_won = player_won && !entry->self_drawn();
    const bool player_shot = player_id.has_value() && !entry->drawn_game && !entry->self_drawn() &&
        entry->from_player_id() == *player_id;
    const bool player_was_selfdrawn = player_id.has_value() && !entry->drawn_game && entry->self_drawn() &&
        entry->winner_player_id() != *player_id && entry->has_player(*player_id);

    if (player_won) {
        tot_win_pt += static_cast<std::uint64_t>(3 * std::round(entry->fan * entry->fan));
        tot_win_turn += static_cast<std::uint64_t>(entry->turn);
        ++tot_wins;
    }
    if (player_self_won) {
        tot_hwin_pt += static_cast<std::uint64_t>(3 * std::round(entry->fan * entry->fan));
        tot_hwin_turn += static_cast<std::uint64_t>(entry->turn);
        ++tot_hwins;
    }
    if (player_ron_won) {
        tot_rkwin_pt += static_cast<std::uint64_t>(3 * std::round(entry->fan * entry->fan));
        tot_rkwin_turn += static_cast<std::uint64_t>(entry->turn);
    }
    if (player_shot) {
        tot_shoot_pt += static_cast<std::uint64_t>(3 * std::round(entry->fan * entry->fan));
        tot_shoot_turn += static_cast<std::uint64_t>(entry->turn);
        ++tot_shoots;
    }
    if (player_was_selfdrawn) {
        tot_selfdrawned_pt += static_cast<std::uint64_t>(3 * std::round(entry->fan * entry->fan));
        tot_selfdrawned_turn += static_cast<std::uint64_t>(entry->turn);
        ++tot_selfdrawneds;
    }

    tot_turn += static_cast<std::uint64_t>(entry->turn);
    if (player_id.has_value()) {
        tot_round_pt += entry->pt_gain(*player_id);
        tot_meld_count += static_cast<std::uint64_t>(entry->player_meld_count(*player_id));
        if (entry->player_meld_count(*player_id) > 0) {
            ++tot_meld_games;
        }
    } else {
        tot_meld_count += static_cast<std::uint64_t>(
            entry->meld_count[0] + entry->meld_count[1] + entry->meld_count[2] + entry->meld_count[3]);
        tot_meld_games += static_cast<std::uint64_t>((entry->meld_count[0] > 0) + (entry->meld_count[1] > 0) +
            (entry->meld_count[2] > 0) + (entry->meld_count[3] > 0));
    }
    if (entry->drawn_game) {
        ++tot_drawn_games;
    }

    std::unordered_set<qingque::fan_code, FanCodeHash> processed_results;
    auto place_entry = [&](const qingque::fan_code& fan_result) {
        std::queue<std::pair<qingque::fan_code, int>> to_process;
        to_process.push({fan_result, 0});
        while (!to_process.empty()) {
            auto [current_result, index] = to_process.front();
            to_process.pop();
            if (index >= static_cast<int>(qingque::fans.size())) {
                if (current_result.any()) {
                    processed_results.insert(current_result);
                }
                continue;
            }
            if (!current_result.test(index)) {
                to_process.push({current_result, index + 1});
                continue;
            }
            qingque::fan_code dropped = current_result;
            dropped.set(index, false);
            to_process.push({dropped, index + 1});
            to_process.push({current_result, index + 1});
        }
    };

    if ((player_id.has_value() && player_won) || !player_id.has_value()) {
        for (const auto& fan_result : entry->fan_results) {
            place_entry(fan_result);
        }
        for (const auto& result : processed_results) {
            fan_stats[result].fan_result = result;
            fan_stats[result].occurrences.push_back(entry);
        }

        std::unordered_set<qingque::fan_code, FanCodeHash> precise_results;
        for (const auto& result : entry->fan_results) {
            if (!result.any()) {
                continue;
            }
            precise_results.insert(result);
        }
        for (const auto& result : precise_results) {
            fan_stats_precise[result].fan_result = result;
            fan_stats_precise[result].occurrences.push_back(entry);
        }

        const auto deduplicated = qingque::derepellenise2(entry->fan_results);
        std::unordered_set<qingque::fan_code, FanCodeHash> no_superior_results;
        for (const auto& fan_result : deduplicated) {
            std::queue<std::pair<qingque::fan_code, int>> to_process;
            to_process.push({fan_result, 0});
            while (!to_process.empty()) {
                auto [current_result, index] = to_process.front();
                to_process.pop();
                if (index >= static_cast<int>(qingque::fans.size())) {
                    if (current_result.any()) {
                        no_superior_results.insert(current_result);
                    }
                    continue;
                }
                if (!current_result.test(index)) {
                    to_process.push({current_result, index + 1});
                    continue;
                }
                qingque::fan_code dropped = current_result;
                dropped.set(index, false);
                to_process.push({dropped, index + 1});
                to_process.push({current_result, index + 1});
            }
        }
        for (const auto& result : no_superior_results) {
            fan_stats_no_superior[result].fan_result = result;
            fan_stats_no_superior[result].occurrences.push_back(entry);
        }
    }

    rounds.push_back(entry);
}

auto RoundCollection::avg_win_pt() const -> double {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_win_pt) / tot_wins;
}

auto RoundCollection::avg_hwin_pt() const -> double {
    return tot_hwins == 0 ? 0.0 : static_cast<double>(tot_hwin_pt) / tot_hwins;
}

auto RoundCollection::avg_rkwin_pt() const -> double {
    return (tot_wins - tot_hwins) == 0 ? 0.0 : static_cast<double>(tot_rkwin_pt) / (tot_wins - tot_hwins);
}

auto RoundCollection::avg_shoot_pt() const -> double {
    return tot_shoots == 0 ? 0.0 : static_cast<double>(tot_shoot_pt) / tot_shoots;
}

auto RoundCollection::avg_selfdrawned_pt() const -> double {
    return tot_selfdrawneds == 0 ? 0.0 : static_cast<double>(tot_selfdrawned_pt) / tot_selfdrawneds;
}

auto RoundCollection::avg_win_turn() const -> double {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_win_turn) / tot_wins;
}

auto RoundCollection::avg_hwin_turn() const -> double {
    return tot_hwins == 0 ? 0.0 : static_cast<double>(tot_hwin_turn) / tot_hwins;
}

auto RoundCollection::avg_rkwin_turn() const -> double {
    return (tot_wins - tot_hwins) == 0 ? 0.0 : static_cast<double>(tot_rkwin_turn) / (tot_wins - tot_hwins);
}

auto RoundCollection::avg_shoot_turn() const -> double {
    return tot_shoots == 0 ? 0.0 : static_cast<double>(tot_shoot_turn) / tot_shoots;
}

auto RoundCollection::avg_selfdrawned_turn() const -> double {
    return tot_selfdrawneds == 0 ? 0.0 : static_cast<double>(tot_selfdrawned_turn) / tot_selfdrawneds;
}

auto RoundCollection::avg_turn() const -> double {
    return rounds.empty() ? 0.0 : static_cast<double>(tot_turn) / rounds.size();
}

auto RoundCollection::avg_round_pt() const -> double {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_round_pt) / rounds.size();
}

auto RoundCollection::avg_meld_count() const -> double {
    if (rounds.empty()) {
        return 0.0;
    }
    return static_cast<double>(tot_meld_count) / (rounds.size() * (player_id.has_value() ? 1.0 : 4.0));
}

auto RoundCollection::win_rate() const -> double {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_wins) / rounds.size();
}

auto RoundCollection::hwin_rate() const -> double {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_hwins) / tot_wins;
}

auto RoundCollection::shoot_rate() const -> double {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_shoots) / rounds.size();
}

auto RoundCollection::selfdrawned_rate() const -> double {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_selfdrawneds) / rounds.size();
}

auto RoundCollection::drawn_game_rate() const -> double {
    return rounds.empty() ? 0.0 : static_cast<double>(tot_drawn_games) / rounds.size();
}

auto RoundCollection::meld_rate() const -> double {
    if (rounds.empty()) {
        return 0.0;
    }
    return static_cast<double>(tot_meld_games) / (rounds.size() * (player_id.has_value() ? 1.0 : 4.0));
}

auto RoundCollection::single_fan_stats(bool exclude_superior_fans) const -> std::vector<SingleFanStat> {
    std::vector<SingleFanStat> stats;
    const auto& source = exclude_superior_fans ? fan_stats_no_superior : fan_stats;
    for (std::size_t index = 0; index < qingque::fans.size(); ++index) {
        qingque::fan_code code;
        code.set(index, true);
        const auto it = source.find(code);
        if (it == source.end()) {
            continue;
        }
        stats.push_back(SingleFanStat{
            .fan_id = static_cast<int>(index),
            .fan_name = qingque::fans[index].name,
            .occurrence_count = it->second.occurrences.size(),
            .occurrence_rate = tot_wins == 0 ? 0.0 : it->second.occurrences.size() / static_cast<double>(tot_wins),
        });
    }
    return stats;
}

auto RoundCollection::fan_composition_stats(bool exclude_superior_fans) const
    -> std::vector<FanCompositionStat> {
    std::vector<FanCompositionStat> stats;
    const auto& source = exclude_superior_fans ? fan_stats_no_superior : fan_stats;
    for (const auto& [fan_code, precise] : fan_stats_precise) {
        const auto readable = qingque::derepellenise(fan_code);
        std::string fan_names;
        bool first = true;
        for (std::size_t index = 1; index < qingque::fans.size(); ++index) {
            if (!readable.test(index)) {
                continue;
            }
            if (!first) {
                fan_names += ", ";
            }
            fan_names += qingque::fans[index].name;
            first = false;
        }
        if (fan_names.empty()) {
            continue;
        }

        const double fan_value = precise.fan();
        const std::uint64_t fan_pt = static_cast<std::uint64_t>(std::round(fan_value * fan_value)) * 3;
        qingque::fan_code inclusive_code = exclude_superior_fans ? qingque::derepellenise2(fan_code) : fan_code;
        const auto inclusive_it = source.find(inclusive_code);

        stats.push_back(FanCompositionStat{
            .label = fan_names + " (" + std::to_string(fan_pt) + "')",
            .fan_names = fan_names,
            .fan_value = fan_value,
            .fan_pt = fan_pt,
            .exact_count = precise.occurrences.size(),
            .inclusive_count = inclusive_it == source.end() ? 0 : inclusive_it->second.occurrences.size(),
        });
    }

    std::sort(stats.begin(), stats.end(), [](const FanCompositionStat& left, const FanCompositionStat& right) {
        if (left.fan_value != right.fan_value) {
            return left.fan_value > right.fan_value;
        }
        return left.exact_count > right.exact_count;
    });
    return stats;
}

auto RoundCollection::ToJson(bool exclude_superior_fans) const -> Json::Value {
    Json::Value resp(Json::objectValue);
    resp["avg_win_pt"] = avg_win_pt();
    resp["avg_hwin_pt"] = avg_hwin_pt();
    resp["avg_rkwin_pt"] = avg_rkwin_pt();
    resp["avg_win_turn"] = avg_win_turn();
    resp["avg_hwin_turn"] = avg_hwin_turn();
    resp["avg_rkwin_turn"] = avg_rkwin_turn();
    resp["avg_turn"] = avg_turn();
    resp["avg_meld_count"] = avg_meld_count();
    resp["win_rate"] = win_rate();
    resp["hwin_rate"] = hwin_rate();
    resp["drawn_game_rate"] = drawn_game_rate();
    resp["meld_rate"] = meld_rate();
    resp["tot_rounds"] = static_cast<Json::UInt64>(rounds.size());
    resp["tot_wins"] = static_cast<Json::UInt64>(tot_wins);

    const auto& fan_stats_to_use = exclude_superior_fans ? fan_stats_no_superior : fan_stats;
    Json::Value fan_stats_array(Json::arrayValue);
    for (std::size_t i = 0; i < qingque::fans.size(); ++i) {
        qingque::fan_code fan_result;
        fan_result.set(i, true);
        const auto it = fan_stats_to_use.find(fan_result);
        if (it == fan_stats_to_use.end()) {
            continue;
        }
        Json::Value fan_json(Json::objectValue);
        fan_json["fan_name"] = qingque::fans[i].name;
        fan_json["occurance_count"] = static_cast<Json::UInt64>(it->second.occurrences.size());
        fan_json["occurance_rate"] = tot_wins == 0 ? 0.0
            : static_cast<double>(it->second.occurrences.size()) / static_cast<double>(tot_wins);
        fan_stats_array.append(std::move(fan_json));
    }
    resp["fan_stats"] = std::move(fan_stats_array);

    if (!player_id.has_value()) {
        return resp;
    }

    resp["avg_shoot_pt"] = avg_shoot_pt();
    resp["avg_selfdrawned_pt"] = avg_selfdrawned_pt();
    resp["shoot_rate"] = shoot_rate();
    resp["selfdrawned_rate"] = selfdrawned_rate();
    resp["avg_round_pt"] = avg_round_pt();
    resp["avg_selfdrawned_turn"] = avg_selfdrawned_turn();
    resp["avg_shoot_turn"] = avg_shoot_turn();
    resp["player_id"] = Json::Int64(*player_id);
    return resp;
}

auto RoundCollection::FanCompositionStatsJson(bool exclude_superior_fans) const -> Json::Value {
    const auto& fan_stats_to_use = exclude_superior_fans ? fan_stats_no_superior : fan_stats;

    Json::Value arr(Json::arrayValue);
    for (const auto& [fan_code, precise] : fan_stats_precise) {
        const auto readable = qingque::derepellenise(fan_code);
        std::string fan_names;
        bool first = true;
        for (std::size_t i = 1; i < qingque::fans.size(); ++i) {
            if (readable.test(i)) {
                if (!first) {
                    fan_names += ", ";
                }
                fan_names += qingque::fans[i].name;
                first = false;
            }
        }
        if (fan_names.empty()) {
            continue;
        }

        const double fan_value = precise.fan();
        const std::uint64_t fan_pt = static_cast<std::uint64_t>(std::round(fan_value * fan_value)) * 3;

        Json::Value comp_json(Json::objectValue);
        comp_json["label"] = fan_names + " (" + std::to_string(fan_pt) + "')";
        comp_json["fan_names"] = fan_names;
        comp_json["fan_value"] = fan_value;
        comp_json["fan_pt"] = Json::UInt64(fan_pt);
        comp_json["exact_count"] = static_cast<Json::UInt64>(precise.occurrences.size());

        std::size_t inclusive_count = 0;
        const auto fan_code_to_use = exclude_superior_fans ? qingque::derepellenise2(fan_code) : fan_code;
        const auto inclusive_it = fan_stats_to_use.find(fan_code_to_use);
        if (inclusive_it != fan_stats_to_use.end()) {
            inclusive_count = inclusive_it->second.occurrences.size();
        }
        comp_json["inclusive_count"] = static_cast<Json::UInt64>(inclusive_count);
        arr.append(std::move(comp_json));
    }

    return arr;
}

auto StatsFilter::matches(const RoundEntry& entry) const -> bool {
    if (player_id.has_value() && !entry.has_player(*player_id)) {
        return false;
    }
    const bool is_nonstandard = IsNonstandardSession(entry.round_key.session_identifier);
    if (nonstandard_only != is_nonstandard) {
        return false;
    }
    if (entry.timestamp_ms < time_start || entry.timestamp_ms > time_end) {
        return false;
    }
    if (entry.fan < min_fan || entry.fan > max_fan) {
        return false;
    }

    const auto fan_results_to_check = exclude_superior_fans
        ? qingque::derepellenise2(entry.fan_results)
        : entry.fan_results;

    if (!fan_filter_positive.empty()) {
        bool found_match = false;
        for (const auto& fan_result : fan_results_to_check) {
            bool has_all = true;
            for (const int fan_id : fan_filter_positive) {
                if (!fan_result.test(static_cast<std::size_t>(fan_id))) {
                    has_all = false;
                    break;
                }
            }
            if (has_all) {
                found_match = true;
                break;
            }
        }
        if (!found_match) {
            return false;
        }
    }

    for (const int fan_id : fan_filter_negative) {
        for (const auto& fan_result : fan_results_to_check) {
            if (fan_result.test(static_cast<std::size_t>(fan_id))) {
                return false;
            }
        }
    }

    for (const auto target_player_id : player_filter_positive) {
        if (!entry.has_player(target_player_id)) {
            return false;
        }
    }
    for (const auto forbidden_player_id : player_filter_negative) {
        if (entry.has_player(forbidden_player_id)) {
            return false;
        }
    }
    if (win_player_id.has_value() && entry.winner_player_id() != *win_player_id) {
        return false;
    }
    for (const auto forbidden_winner_id : win_player_filter_negative) {
        if (entry.winner_player_id() == forbidden_winner_id) {
            return false;
        }
    }
    if (from_player_id.has_value() && entry.from_player_id() != *from_player_id) {
        return false;
    }
    for (const auto forbidden_from_id : from_player_filter_negative) {
        if (entry.from_player_id() == forbidden_from_id) {
            return false;
        }
    }
    for (const auto flag : win_type_filter_positive) {
        if (!entry.has_win_type(flag)) {
            return false;
        }
    }
    for (const auto flag : win_type_filter_negative) {
        if (entry.has_win_type(flag)) {
            return false;
        }
    }
    if (self_drawn.has_value() && entry.self_drawn() != *self_drawn) {
        return false;
    }
    return true;
}

StatsService::StatsService(storage::Database* database)
    : database_(database) {}

auto StatsService::InitializeSchema(const std::filesystem::path& migrations_dir) -> util::Status {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    storage::MigrationRunner runner(database_);
    return runner.ApplyDirectory(migrations_dir);
}

auto StatsService::LoadFromDatabase() -> util::Status {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto statement = database_->Prepare(
        "SELECT session_identifier, round_number, drawn_game, winner_seat, from_seat, win_type_bits, "
        "win_tile, turn, time_ms, fan_text, meld_count_json, players_json, fan_results_json, fan_names_json "
        "FROM stats_round_entries;");
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

        auto fan_names = ParseFanNamesJson(statement.value().ColumnText(13));
        if (!fan_names.ok()) {
            return fan_names.status();
        }
        entry.fan_names = fan_names.value();

        rounds_[entry.round_key] = std::move(entry);
    }

    rebuild_indexes_locked();
    ++version_;
    return util::Status::Ok();
}

auto StatsService::UpsertRoundRecord(const Json::Value& record) -> util::Status {
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

auto StatsService::ListAllRounds() const -> std::vector<const RoundEntry*> {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rounds_by_time_desc_;
}

auto StatsService::ImportRecordsFromDirectory(const std::filesystem::path& records_root) -> util::Status {
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

auto StatsService::Query(const StatsFilter& filter) const -> util::StatusOr<RoundCollection> {
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

auto StatsService::ListRounds(const StatsFilter& filter,
                              std::string_view sort_field,
                              std::string_view sort_order,
                              std::size_t offset,
                              std::size_t limit) const -> util::StatusOr<RoundPage> {
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

auto StatsService::ListPlayers() const -> std::vector<RoundPlayer> {
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

auto StatsService::round_count() const -> std::size_t {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rounds_.size();
}

auto StatsService::version() const -> std::uint64_t {
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

auto StatsService::persist_round_locked(const RoundEntry& entry) -> util::Status {
    auto statement = database_->Prepare(
        "INSERT OR REPLACE INTO stats_round_entries("
        "session_identifier, round_number, drawn_game, winner_seat, from_seat, win_type_bits, "
        "win_tile, turn, time_ms, fan_text, meld_count_json, players_json, fan_results_json, fan_names_json) "
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
    status = stmt.BindText(14, EncodeFanNamesJson(entry.fan_names));
    if (!status.ok()) {
        return status;
    }
    return StepDone(stmt);
}

auto ProjectRoundRecord(const Json::Value& record) -> util::StatusOr<RoundEntry> {
    if (!record.isObject()) {
        return util::Status::InvalidArgument("round record must be a JSON object");
    }

    auto header = ReadRequiredObject(record, "header");
    if (!header.ok()) {
        return header.status();
    }

    auto session_identifier = ReadRequiredString(*header.value(), "session_identifier");
    if (!session_identifier.ok()) {
        return session_identifier.status();
    }
    auto round_number = ReadRequiredUInt64(*header.value(), "round_number");
    if (!round_number.ok()) {
        return round_number.status();
    }

    auto initial_seats = ReadRequiredArray(record, "initial_seats");
    if (!initial_seats.ok()) {
        return initial_seats.status();
    }
    auto players = ParsePlayers(*initial_seats.value());
    if (!players.ok()) {
        return players.status();
    }

    auto round_result = ReadRequiredObject(record, "round_result");
    if (!round_result.ok()) {
        return round_result.status();
    }
    if (!(*round_result.value())["completed"].isBool()) {
        return util::Status::InvalidArgument("round_result.completed must be a bool");
    }
    if (!(*round_result.value())["completed"].asBool()) {
        return util::Status::InvalidArgument("round record is incomplete");
    }

    auto turn = ReadRequiredInt64(*round_result.value(), "turn");
    if (!turn.ok()) {
        return turn.status();
    }
    auto time_ms = ReadRequiredInt64(*round_result.value(), "time_ms");
    if (!time_ms.ok()) {
        return time_ms.status();
    }
    auto meld_count_json = ReadRequiredArray(*round_result.value(), "meld_count");
    if (!meld_count_json.ok()) {
        return meld_count_json.status();
    }
    if (meld_count_json.value()->size() != 4) {
        return util::Status::InvalidArgument("round_result.meld_count must contain four entries");
    }

    RoundEntry round_entry;
    round_entry.round_key = RoundKey{session_identifier.value(), round_number.value()};
    round_entry.players = players.value();
    round_entry.turn = turn.value();
    round_entry.timestamp_ms = time_ms.value();
    for (Json::ArrayIndex index = 0; index < meld_count_json.value()->size(); ++index) {
        const auto& count = (*meld_count_json.value())[index];
        if (!count.isInt()) {
            return util::Status::InvalidArgument("round_result.meld_count entries must be integers");
        }
        round_entry.meld_count[index] = count.asInt();
    }

    if (!(*round_result.value())["drawn_game"].isBool()) {
        return util::Status::InvalidArgument("round_result.drawn_game must be a bool");
    }
    round_entry.drawn_game = (*round_result.value())["drawn_game"].asBool();
    if (round_entry.drawn_game) {
        return round_entry;
    }

    auto winner_seat = ReadRequiredInt64(*round_result.value(), "winner_seat");
    if (!winner_seat.ok()) {
        return winner_seat.status();
    }
    auto from_seat = ReadRequiredInt64(*round_result.value(), "from_seat");
    if (!from_seat.ok()) {
        return from_seat.status();
    }
    auto win_type_bits = ReadRequiredUInt64(*round_result.value(), "win_type_bits");
    if (!win_type_bits.ok()) {
        return win_type_bits.status();
    }
    const Json::Value& win_tile = (*round_result.value())["win_tile"];
    if (!win_tile.isInt() && !win_tile.isUInt() && !win_tile.isInt64() && !win_tile.isUInt64()) {
        return util::Status::InvalidArgument("round_result.win_tile must be an integer");
    }

    round_entry.winner_seat = static_cast<int>(winner_seat.value());
    round_entry.from_seat = static_cast<int>(from_seat.value());
    round_entry.win_type = mahjong::win_type(static_cast<mahjong::win_t>(win_type_bits.value()));
    round_entry.win_tile = static_cast<mahjong::tile_t>(win_tile.asUInt());

    auto fan = ReadRequiredDouble(*round_result.value(), "fan");
    if (!fan.ok()) {
        return fan.status();
    }
    round_entry.fan = fan.value();

    const Json::Value& fan_results_json = (*round_result.value())["fan_results"];
    if (!fan_results_json.isArray()) {
        return util::Status::InvalidArgument("round_result.fan_results must be an array");
    }
    std::vector<qingque::fan_code> fan_results;
    fan_results.reserve(fan_results_json.size());
    for (const auto& entry : fan_results_json) {
        if (!entry.isString()) {
            return util::Status::InvalidArgument("round_result.fan_results must contain strings");
        }
        fan_results.emplace_back(entry.asString());
    }
    round_entry.fan_results = std::move(fan_results);

    const Json::Value& fan_names_json = (*round_result.value())["fan_names"];
    if (!fan_names_json.isArray()) {
        return util::Status::InvalidArgument("round_result.fan_names must be an array");
    }
    round_entry.fan_names.reserve(fan_names_json.size());
    for (const auto& entry : fan_names_json) {
        if (!entry.isString()) {
            return util::Status::InvalidArgument("round_result.fan_names must contain strings");
        }
        round_entry.fan_names.push_back(entry.asString());
    }

    return round_entry;
}

}  // namespace mmcr::stats