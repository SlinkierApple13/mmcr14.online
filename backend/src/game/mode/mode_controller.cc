#include "game/mode/mode_controller.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "external/qingque/rules/qingque.h"
#include "random/seed.h"

namespace mmcr::game {

namespace {

// ---------------------------------------------------------------------------
// Standard mode: every hook keeps the historical behaviour. This class exists
// so session.cc never branches on the mode; zero regression for default games.
// ---------------------------------------------------------------------------
class StandardMode final : public ModeController {
public:
    [[nodiscard]] std::string_view mode_name() const override {
        return "standard";
    }
};

// ---------------------------------------------------------------------------
// 过五关 (Pass Five Gates) mode
// ---------------------------------------------------------------------------
//
// Each session draws 5 public targets (1 easy + 3 medium + 1 hard). The 4
// players split into two teams (虎/龙). A team wins by completing all 5
// targets, by knocking the opposing team out (score < -knockout_score), or
// by the round-limit tie-break. The qingque fan library is untouched: target
// completion is judged against the union of fan indices across every winning
// decomposition (RawFanIndices).
// ---------------------------------------------------------------------------

// A drawable target: either a single qingque fan or the composite
// "四大偶然番" (any one of the four occasional fans completes it).
struct TargetSpec {
    std::string name;                 // display name
    std::vector<qingque::indices> fan_indices;  // completing fans (union)
    bool composite{false};
};

// The four occasional fans behind the composite target 四大偶然番.
constexpr std::array<qingque::indices, 4> kOccasionalFans = {
    qingque::indices::out_with_replacement_tile,
    qingque::indices::last_tile_draw,
    qingque::indices::last_tile_claim,
    qingque::indices::robbing_the_kong,
};

// Pools of drawable fan indices (no duplicates within a pool).
const std::vector<qingque::indices>& EasyPool() {
    static const std::vector<qingque::indices> pool = {
        qingque::indices::all_triplets,
        qingque::indices::fan_tile_2p,
        qingque::indices::half_flush,
        qingque::indices::all_types,
        qingque::indices::mixed_outside_hand,
        qingque::indices::pure_straight,
        qingque::indices::mixed_triple_sequence,
        qingque::indices::mixed_straight,
        qingque::indices::two_short_straights,
    };
    return pool;
}

const std::vector<qingque::indices>& MediumPool() {
    static const std::vector<qingque::indices> pool = {
        qingque::indices::seven_pairs,
        qingque::indices::concealed_kong,
        qingque::indices::two_kongs,
        qingque::indices::fan_tile_2t,
        qingque::indices::four_consecutive_numbers,
        qingque::indices::connected_numbers,
        qingque::indices::reflected_hand,
        qingque::indices::three_chained_sequences,
        qingque::indices::mirrored_hand,
    };
    return pool;
}

const std::vector<qingque::indices>& HardPool() {
    static const std::vector<qingque::indices> pool = {
        qingque::indices::three_concealed_triplets,
        qingque::indices::double_pair,
        qingque::indices::fan_tile_3p,
        qingque::indices::pure_outside_hand,
        qingque::indices::full_flush,
        qingque::indices::three_consecutive_numbers,
        qingque::indices::gapped_numbers,
        qingque::indices::reflected_hand_2,
        qingque::indices::common_number,
        qingque::indices::two_double_sequences,
        qingque::indices::three_shifted_sequences,
    };
    return pool;
}

// Superior/subordinate chains: a superior target must not coexist with any
// of its subordinates in the drawn set.
struct HierarchyRule {
    qingque::indices superior;
    std::vector<qingque::indices> subordinates;
};

const std::vector<HierarchyRule>& HierarchyRules() {
    static const std::vector<HierarchyRule> rules = {
        {qingque::indices::seven_pairs, {qingque::indices::double_pair}},
        {qingque::indices::half_flush, {qingque::indices::full_flush}},
        {qingque::indices::fan_tile_2p,
         {qingque::indices::fan_tile_2t, qingque::indices::fan_tile_3p}},
        {qingque::indices::mixed_outside_hand, {qingque::indices::pure_outside_hand}},
        {qingque::indices::four_consecutive_numbers,
         {qingque::indices::three_consecutive_numbers}},
    };
    return rules;
}

// Display name of a fan index (single source of truth: the qingque table).
std::string_view FanName(qingque::indices index) {
    const std::size_t table_index = static_cast<std::size_t>(index);
    return table_index < qingque::fans.size() ? qingque::fans[table_index].name
                                              : std::string_view();
}

// Builds the target spec for one fan index.
TargetSpec MakeSingleTarget(qingque::indices index) {
    TargetSpec spec;
    spec.name = std::string(FanName(index));
    spec.fan_indices.push_back(index);
    return spec;
}

// The composite 四大偶然番 target: any of the four occasional fans completes it.
TargetSpec MakeOccasionalComposite() {
    TargetSpec spec;
    spec.name = "四大偶然番";
    spec.composite = true;
    spec.fan_indices.assign(kOccasionalFans.begin(), kOccasionalFans.end());
    return spec;
}

// True when the given single fan index is one of a rule's subordinates of
// `superior`, or is itself the superior of `subordinate`.
bool ConflictsWithFan(qingque::indices fan, qingque::indices other) {
    for (const auto& rule : HierarchyRules()) {
        const bool fan_is_superior = fan == rule.superior;
        const bool fan_is_subordinate =
            std::find(rule.subordinates.begin(), rule.subordinates.end(), fan) !=
            rule.subordinates.end();
        const bool other_is_superior = other == rule.superior;
        const bool other_is_subordinate =
            std::find(rule.subordinates.begin(), rule.subordinates.end(), other) !=
            rule.subordinates.end();
        if (fan_is_superior && other_is_subordinate) {
            return true;
        }
        if (fan_is_subordinate && other_is_superior) {
            return true;
        }
    }
    return false;
}

class PassFiveGatesMode final : public ModeController {
public:
    explicit PassFiveGatesMode(const GameConfig& config) : config_(config) {
        if (config_.pass_five_gates.has_value()) {
            const PassFiveGatesConfig& cfg = *config_.pass_five_gates;
            knockout_score_ = cfg.knockout_score;
            // Server-injected team assignment (player_id -> 0|1). Copied from
            // the typed config; clients can never write this.
            team_by_player_ = cfg.team;
        }
    }

    [[nodiscard]] std::string_view mode_name() const override {
        return "pass_five_gates";
    }

    void OnSessionStart(random::SeedContainer* seed_container) override {
        // Exactly one seed, consumed before the first kStart's wall seeds, so
        // targets are reproducible in replays.
        const std::uint64_t seed = seed_container->Extract();
        std::mt19937_64 rng(seed);
        targets_ = DrawTargets(rng);
    }

    void SetSeatPlayers(const std::array<std::int64_t, 4>& player_ids) override {
        seat_player_.clear();
        player_seat_.clear();
        for (std::size_t seat = 0; seat < player_ids.size(); ++seat) {
            if (player_ids[seat] <= 0) {
                continue;
            }
            seat_player_[static_cast<int>(seat)] = player_ids[seat];
            player_seat_[player_ids[seat]] = static_cast<int>(seat);
        }
    }

    void OnRoundStart(std::uint64_t) override {
        // A new round begins: clear any leftover mode update from the
        // previous settlement so it is not re-broadcast.
        pending_mode_update_.reset();
    }

    auto OnRoundSettled(int seat, const std::vector<int>& raw_fans,
                        const std::array<int, 4>& scores) -> TransitionDecision override {
        if (targets_.empty()) {
            return TransitionDecision::kContinue;
        }
        const auto player_it = seat_player_.find(seat);
        if (player_it == seat_player_.end()) {
            return TransitionDecision::kContinue;
        }
        const auto team_it = team_by_player_.find(player_it->second);
        if (team_it == team_by_player_.end()) {
            return TransitionDecision::kContinue;
        }
        const int team = team_it->second;

        const std::set<int> raw_set(raw_fans.begin(), raw_fans.end());
        std::vector<std::string> completed_now;
        for (std::size_t target_index = 0; target_index < targets_.size(); ++target_index) {
            if ((completed_[team] & (1ULL << target_index)) != 0) {
                continue;
            }
            const TargetSpec& target = targets_[target_index];
            const bool hit = std::any_of(
                target.fan_indices.begin(), target.fan_indices.end(),
                [&raw_set](qingque::indices fan) {
                    return raw_set.contains(static_cast<int>(fan));
                });
            if (hit) {
                completed_[team] |= (1ULL << target_index);
                completed_now.push_back(target.name);
            }
        }
        if (!completed_now.empty()) {
            pending_mode_update_ = std::make_pair(team, std::move(completed_now));
        }

        // Victory priority:
        // 1. all five targets completed by the winner's team (draw if a
        //    member of that team is knocked out).
        // 2. knockout (score < -knockout_score): more knocked out loses;
        //    equal non-zero -> draw.
        // 3. otherwise continue; round-limit tie-break happens at session end.
        constexpr std::uint64_t kAllTargets = 0x1F;  // bits 0..4
        if ((completed_[team] & kAllTargets) == kAllTargets) {
            winner_team_ = (knockout_score_ > 0 && CountKnockedOut(team, scores) > 0)
                ? kDraw
                : team;
            return TransitionDecision::kEnd;
        }

        if (knockout_score_ > 0) {
            const int t0_knocked = CountKnockedOut(0, scores);
            const int t1_knocked = CountKnockedOut(1, scores);
            if (t0_knocked != t1_knocked) {
                winner_team_ = t0_knocked < t1_knocked ? 0 : 1;
                return TransitionDecision::kEnd;
            }
            if (t0_knocked > 0) {
                winner_team_ = kDraw;
                return TransitionDecision::kEnd;
            }
        }

        return TransitionDecision::kContinue;
    }

    auto OnSessionEnd(const std::array<int, 4>& final_scores) -> Json::Value override {
        if (winner_team_ == kUndecided && config_.round_count > 0) {
            // Round-limit tie-break: more completed targets wins; then team
            // total score; then draw.
            const std::uint64_t t0_count = CountBits(completed_[0]);
            const std::uint64_t t1_count = CountBits(completed_[1]);
            if (t0_count != t1_count) {
                winner_team_ = t0_count > t1_count ? 0 : 1;
            } else {
                const std::int64_t t0_score = TeamTotalScore(0, final_scores);
                const std::int64_t t1_score = TeamTotalScore(1, final_scores);
                if (t0_score != t1_score) {
                    winner_team_ = t0_score > t1_score ? 0 : 1;
                } else {
                    winner_team_ = kDraw;
                }
            }
        }
        return SerializeModeResult();
    }

    void SerializeState(Json::Value& payload) const override {
        // payload is the (empty) mode_state object created by the session;
        // fill its fields directly.
        payload["mode"] = "pass_five_gates";
        Json::Value targets(Json::arrayValue);
        for (const auto& target : targets_) {
            targets.append(target.name);
        }
        payload["targets"] = std::move(targets);
        Json::Value completed(Json::arrayValue);
        completed.append(Json::UInt64(completed_[0]));
        completed.append(Json::UInt64(completed_[1]));
        payload["completed"] = std::move(completed);
        Json::Value teams(Json::arrayValue);
        for (const auto& [player_id, team] : team_by_player_) {
            Json::Value entry(Json::objectValue);
            entry["player_id"] = Json::Int64(player_id);
            entry["team"] = team;
            teams.append(std::move(entry));
        }
        payload["teams"] = std::move(teams);
        payload["winner_team"] = winner_team_ == kUndecided ? Json::Value(Json::nullValue)
                                                            : Json::Value(winner_team_);
        payload["knockout_score"] = knockout_score_;
    }

    void SerializeEvent(Json::Value& event_payload) const override {
        if (!pending_mode_update_.has_value()) {
            return;
        }
        Json::Value update(Json::objectValue);
        update["team"] = pending_mode_update_->first;
        Json::Value completed_now(Json::arrayValue);
        for (const auto& name : pending_mode_update_->second) {
            completed_now.append(name);
        }
        update["completed_now"] = std::move(completed_now);
        Json::Value completed(Json::arrayValue);
        completed.append(Json::UInt64(completed_[0]));
        completed.append(Json::UInt64(completed_[1]));
        update["completed"] = std::move(completed);
        update["winner_team"] = winner_team_ == kUndecided ? Json::Value(Json::nullValue)
                                                           : Json::Value(winner_team_);
        event_payload["mode_update"] = std::move(update);
    }

private:
    static constexpr int kDraw = -2;
    static constexpr int kUndecided = -1;

    [[nodiscard]] std::vector<TargetSpec> DrawTargets(std::mt19937_64& rng) const {
        std::vector<TargetSpec> targets;
        // Fan indices already drawn (composite targets contribute all their
        // fans so hierarchy conflicts are checked per fan).
        std::set<qingque::indices> drawn_fans;

        const auto add_target = [&](TargetSpec spec) {
            for (const auto fan : spec.fan_indices) {
                drawn_fans.insert(fan);
            }
            targets.push_back(std::move(spec));
        };

        // 1. easy: draw 1 of 9.
        {
            std::uniform_int_distribution<std::size_t> dist(0, EasyPool().size() - 1);
            add_target(MakeSingleTarget(EasyPool()[dist(rng)]));
        }

        // 2. medium: draw 3 without replacement, avoiding hierarchy
        //    conflicts with everything already drawn. The composite
        //    四大偶然番 is one of the medium candidates.
        {
            std::vector<TargetSpec> pool;
            pool.reserve(MediumPool().size() + 1);
            for (const auto fan : MediumPool()) {
                pool.push_back(MakeSingleTarget(fan));
            }
            pool.push_back(MakeOccasionalComposite());
            std::shuffle(pool.begin(), pool.end(), rng);
            std::size_t added = 0;
            for (auto& spec : pool) {
                if (added >= 3) {
                    break;
                }
                if (ConflictsWithDrawn(spec, drawn_fans)) {
                    continue;
                }
                add_target(std::move(spec));
                ++added;
            }
        }

        // 3. hard: exclude hierarchy-conflicting fans, then draw 1.
        {
            std::vector<qingque::indices> available;
            for (const auto fan : HardPool()) {
                if (!ConflictsWithDrawn(MakeSingleTarget(fan), drawn_fans)) {
                    available.push_back(fan);
                }
            }
            std::uniform_int_distribution<std::size_t> dist(0, available.size() - 1);
            add_target(MakeSingleTarget(available[dist(rng)]));
        }

        return targets;
    }

    // Returns true when `candidate` cannot be added to the drawn set without
    // violating a superiority chain (superior must not coexist with any
    // subordinate). Composite targets carry all their fans so they are
    // checked per fan (occasional fans never appear in hierarchy rules).
    [[nodiscard]] static bool ConflictsWithDrawn(
        const TargetSpec& candidate, const std::set<qingque::indices>& drawn_fans) {
        if (drawn_fans.empty()) {
            return false;
        }
        for (const auto& rule : HierarchyRules()) {
            const bool candidate_has_superior =
                std::find(candidate.fan_indices.begin(), candidate.fan_indices.end(),
                          rule.superior) != candidate.fan_indices.end();
            const bool candidate_has_subordinate = std::any_of(
                candidate.fan_indices.begin(), candidate.fan_indices.end(),
                [&rule](qingque::indices fan) {
                    return std::find(rule.subordinates.begin(), rule.subordinates.end(), fan) !=
                           rule.subordinates.end();
                });
            if (candidate_has_superior) {
                for (const auto subordinate : rule.subordinates) {
                    if (drawn_fans.contains(subordinate)) {
                        return true;
                    }
                }
            }
            if (candidate_has_subordinate && drawn_fans.contains(rule.superior)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int CountKnockedOut(int team, const std::array<int, 4>& scores) const {
        int count = 0;
        for (const auto& [player_id, player_team] : team_by_player_) {
            if (player_team != team) {
                continue;
            }
            const auto seat_it = player_seat_.find(player_id);
            if (seat_it != player_seat_.end() && scores[seat_it->second] < -knockout_score_) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::int64_t TeamTotalScore(int team,
                                              const std::array<int, 4>& scores) const {
        std::int64_t total = 0;
        for (const auto& [player_id, player_team] : team_by_player_) {
            if (player_team != team) {
                continue;
            }
            const auto seat_it = player_seat_.find(player_id);
            if (seat_it != player_seat_.end()) {
                total += scores[seat_it->second];
            }
        }
        return total;
    }

    [[nodiscard]] static std::uint64_t CountBits(std::uint64_t bits) {
        std::uint64_t count = 0;
        while (bits != 0) {
            bits &= bits - 1;
            ++count;
        }
        return count;
    }

    [[nodiscard]] Json::Value SerializeModeResult() const {
        Json::Value result(Json::objectValue);
        result["winner_team"] = winner_team_ == kUndecided ? Json::Value(Json::nullValue)
                                                           : Json::Value(winner_team_);
        result["draw"] = winner_team_ == kDraw;
        Json::Value completed(Json::arrayValue);
        completed.append(Json::UInt64(completed_[0]));
        completed.append(Json::UInt64(completed_[1]));
        result["completed"] = std::move(completed);
        Json::Value targets(Json::arrayValue);
        for (const auto& target : targets_) {
            targets.append(target.name);
        }
        result["targets"] = std::move(targets);
        Json::Value team_names(Json::arrayValue);
        team_names.append("虎队");
        team_names.append("龙队");
        result["team_names"] = std::move(team_names);
        return result;
    }

    GameConfig config_;
    std::vector<TargetSpec> targets_;
    // player_id -> team (0 = 虎, 1 = 龙); injected by the hub at start.
    std::unordered_map<std::int64_t, int> team_by_player_;
    // seat index -> player id; refreshed each round.
    std::unordered_map<int, std::int64_t> seat_player_;
    // player id -> seat index; refreshed each round.
    std::unordered_map<std::int64_t, int> player_seat_;
    std::array<std::uint64_t, 2> completed_{0, 0};
    int knockout_score_{0};
    int winner_team_{kUndecided};
    // Team -> target names completed in the most recent settlement.
    mutable std::optional<std::pair<int, std::vector<std::string>>> pending_mode_update_;
};

}  // namespace

std::unique_ptr<ModeController> CreateModeController(const GameConfig& config) {
    switch (config.mode) {
        case GameMode::kStandard:
            return std::make_unique<StandardMode>();
        case GameMode::kPassFiveGates:
            return std::make_unique<PassFiveGatesMode>(config);
    }
    return std::make_unique<StandardMode>();
}

}  // namespace mmcr::game
