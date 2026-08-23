#include "stats/stats_internal.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_set>
#include <utility>

namespace mmcr::stats {

bool IsNonstandardSession(std::string_view session_identifier) {
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

        const auto deduplicated = qingque::dedupe2(entry->fan_results);
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

double RoundCollection::avg_win_pt() const {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_win_pt) / tot_wins;
}

double RoundCollection::avg_hwin_pt() const {
    return tot_hwins == 0 ? 0.0 : static_cast<double>(tot_hwin_pt) / tot_hwins;
}

double RoundCollection::avg_rkwin_pt() const {
    return (tot_wins - tot_hwins) == 0 ? 0.0 : static_cast<double>(tot_rkwin_pt) / (tot_wins - tot_hwins);
}

double RoundCollection::avg_shoot_pt() const {
    return tot_shoots == 0 ? 0.0 : static_cast<double>(tot_shoot_pt) / tot_shoots;
}

double RoundCollection::avg_selfdrawned_pt() const {
    return tot_selfdrawneds == 0 ? 0.0 : static_cast<double>(tot_selfdrawned_pt) / tot_selfdrawneds;
}

double RoundCollection::avg_win_turn() const {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_win_turn) / tot_wins;
}

double RoundCollection::avg_hwin_turn() const {
    return tot_hwins == 0 ? 0.0 : static_cast<double>(tot_hwin_turn) / tot_hwins;
}

double RoundCollection::avg_rkwin_turn() const {
    return (tot_wins - tot_hwins) == 0 ? 0.0 : static_cast<double>(tot_rkwin_turn) / (tot_wins - tot_hwins);
}

double RoundCollection::avg_shoot_turn() const {
    return tot_shoots == 0 ? 0.0 : static_cast<double>(tot_shoot_turn) / tot_shoots;
}

double RoundCollection::avg_selfdrawned_turn() const {
    return tot_selfdrawneds == 0 ? 0.0 : static_cast<double>(tot_selfdrawned_turn) / tot_selfdrawneds;
}

double RoundCollection::avg_turn() const {
    return rounds.empty() ? 0.0 : static_cast<double>(tot_turn) / rounds.size();
}

double RoundCollection::avg_round_pt() const {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_round_pt) / rounds.size();
}

double RoundCollection::avg_meld_count() const {
    if (rounds.empty()) {
        return 0.0;
    }
    return static_cast<double>(tot_meld_count) / (rounds.size() * (player_id.has_value() ? 1.0 : 4.0));
}

double RoundCollection::win_rate() const {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_wins) / rounds.size();
}

double RoundCollection::hwin_rate() const {
    return tot_wins == 0 ? 0.0 : static_cast<double>(tot_hwins) / tot_wins;
}

double RoundCollection::shoot_rate() const {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_shoots) / rounds.size();
}

double RoundCollection::selfdrawned_rate() const {
    return (!player_id.has_value() || rounds.empty()) ? 0.0 : static_cast<double>(tot_selfdrawneds) / rounds.size();
}

double RoundCollection::drawn_game_rate() const {
    return rounds.empty() ? 0.0 : static_cast<double>(tot_drawn_games) / rounds.size();
}

double RoundCollection::meld_rate() const {
    if (rounds.empty()) {
        return 0.0;
    }
    return static_cast<double>(tot_meld_games) / (rounds.size() * (player_id.has_value() ? 1.0 : 4.0));
}

std::vector<SingleFanStat> RoundCollection::single_fan_stats(bool exclude_superior_fans) const {
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

std::vector<FanCompositionStat> RoundCollection::fan_composition_stats(bool exclude_superior_fans) const {
    std::vector<FanCompositionStat> stats;
    const auto& source = exclude_superior_fans ? fan_stats_no_superior : fan_stats;
    for (const auto& [fan_code, precise] : fan_stats_precise) {
        const auto readable = qingque::dedupe(fan_code);
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
        qingque::fan_code inclusive_code = exclude_superior_fans ? qingque::dedupe2(fan_code) : fan_code;
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

Json::Value RoundCollection::ToJson(bool exclude_superior_fans) const {
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

Json::Value RoundCollection::FanCompositionStatsJson(bool exclude_superior_fans) const {
    const auto& fan_stats_to_use = exclude_superior_fans ? fan_stats_no_superior : fan_stats;

    Json::Value arr(Json::arrayValue);
    for (const auto& [fan_code, precise] : fan_stats_precise) {
        const auto readable = qingque::dedupe(fan_code);
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
        const auto fan_code_to_use = exclude_superior_fans ? qingque::dedupe2(fan_code) : fan_code;
        const auto inclusive_it = fan_stats_to_use.find(fan_code_to_use);
        if (inclusive_it != fan_stats_to_use.end()) {
            inclusive_count = inclusive_it->second.occurrences.size();
        }
        comp_json["inclusive_count"] = static_cast<Json::UInt64>(inclusive_count);
        arr.append(std::move(comp_json));
    }

    return arr;
}

bool StatsFilter::matches(const RoundEntry& entry) const {
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
        ? qingque::dedupe2(entry.fan_results)
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

}  // namespace mmcr::stats
