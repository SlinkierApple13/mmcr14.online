#include <array>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "external/qingque/basic/mahjong.h"
#include "external/qingque/rules/qingque.h"
#include "game/config.h"
#include "game/engine/session_internal.h"
#include "game/mode/mode_controller.h"
#include "game/mode/mode_registry.h"
#include "random/seed.h"

namespace mmcr::game {
namespace {

std::uint64_t CountBitsForTest(std::uint64_t bits) {
    std::uint64_t count = 0;
    while (bits != 0) {
        bits &= bits - 1;
        ++count;
    }
    return count;
}

// ── Diagnostic: RawFanIndices on a 三色同顺 winning hand ──────────
TEST(RawFanIndicesDiagnostic, SanshokuDoujunIsDetected) {
    using namespace mahjong;
    using namespace mahjong::tile_literals;

    // Melded version: 123m + 123p + 123s exposed, 456m + 99m pair, win 9m.
    // NOTE: qingque encodes a sequence meld by its CENTER tile (123 -> 2).
    std::vector<mahjong::meld> melds = {
        meld(2_m, mahjong::meld_type::sequence, false, true),
        meld(2_p, mahjong::meld_type::sequence, false, true),
        meld(2_s, mahjong::meld_type::sequence, false, true),
    };
    std::vector<mahjong::tile_t> tiles = {4_m, 5_m, 6_m, 9_m};
    mahjong::tile_t win_tile = 9_m;

    mahjong::hand h(tiles, melds, win_tile);
    EXPECT_TRUE(qingque::is_winning_hand(h)) << "hand should be a winning hand";
    std::cerr << "DIAG melded decompose size: " << h.decompose().size() << "\n";
    std::vector<int> indices = RawFanIndices(h);
    std::cerr << "DIAG melded RawFanIndices:";
    for (int idx : indices) std::cerr << " [" << idx << "]" << qingque::fans[idx].name;
    std::cerr << "\n";
    bool found = false;
    for (int idx : indices) {
        if (qingque::fans[idx].name == "三色同顺") found = true;
    }
    EXPECT_TRUE(found) << "三色同顺 missing from RawFanIndices (melded)";

    // Concealed version: closed hand 123m 123p 123s 456m 99m, win 9m.
    std::vector<mahjong::tile_t> closed_tiles = {
        1_m, 2_m, 3_m, 1_p, 2_p, 3_p, 1_s, 2_s, 3_s, 4_m, 5_m, 6_m, 9_m, 9_m,
    };
    mahjong::hand h2(closed_tiles, {}, 9_m, 0, true);
    EXPECT_TRUE(qingque::is_winning_hand(h2)) << "closed hand should be a winning hand";
    std::cerr << "DIAG closed decompose size: " << h2.decompose().size() << "\n";
    std::vector<int> indices2 = RawFanIndices(h2);
    std::cerr << "DIAG closed RawFanIndices:";
    for (int idx : indices2) std::cerr << " [" << idx << "]" << qingque::fans[idx].name;
    std::cerr << "\n";
    bool found2 = false;
    for (int idx : indices2) {
        if (qingque::fans[idx].name == "三色同顺") found2 = true;
    }
    EXPECT_TRUE(found2) << "三色同顺 missing from RawFanIndices (closed)";
}

TEST(ModeRegistryTest, BuiltinPresetsAreAvailable) {
    const auto& presets = ModePresets();
    ASSERT_EQ(presets.size(), 2U);

    EXPECT_EQ(presets[0].id, "standard");
    EXPECT_EQ(presets[0].mode, GameMode::kStandard);

    EXPECT_EQ(presets[1].id, "pass_five_gates");
    EXPECT_EQ(presets[1].mode, GameMode::kPassFiveGates);
    EXPECT_FALSE(presets[1].name.empty());
}

TEST(ModeRegistryTest, ParseAndFormatRoundTrip) {
    const auto standard = ParseGameMode("standard");
    ASSERT_TRUE(standard.has_value());
    EXPECT_EQ(*standard, GameMode::kStandard);
    EXPECT_EQ(GameModeName(*standard), "standard");

    const auto five_gates = ParseGameMode("pass_five_gates");
    ASSERT_TRUE(five_gates.has_value());
    EXPECT_EQ(*five_gates, GameMode::kPassFiveGates);
    EXPECT_EQ(GameModeName(*five_gates), "pass_five_gates");

    EXPECT_FALSE(ParseGameMode("no_such_mode").has_value());
    EXPECT_FALSE(ParseGameMode("").has_value());
}

TEST(ModeRegistryTest, DisplayNameIsNeverEmpty) {
    EXPECT_FALSE(ModeDisplayName(GameMode::kStandard).empty());
    EXPECT_FALSE(ModeDisplayName(GameMode::kPassFiveGates).empty());
}

TEST(ModeControllerFactoryTest, CreatesMatchingController) {
    GameConfig standard_config;
    standard_config.mode = GameMode::kStandard;
    auto standard = CreateModeController(standard_config);
    ASSERT_NE(standard, nullptr);
    EXPECT_EQ(standard->mode_name(), "standard");

    GameConfig five_gates_config;
    five_gates_config.mode = GameMode::kPassFiveGates;
    auto five_gates = CreateModeController(five_gates_config);
    ASSERT_NE(five_gates, nullptr);
    EXPECT_EQ(five_gates->mode_name(), "pass_five_gates");
}

TEST(ModeControllerTest, StandardNextTransitionFollowsRoundCount) {
    GameConfig config;
    config.mode = GameMode::kStandard;
    auto mode = CreateModeController(config);
    ASSERT_NE(mode, nullptr);

    // 0 = unlimited: never ends by round count.
    EXPECT_EQ(mode->NextTransition(100, 0), EventKind::kStart);
    // Finite: ends once round_counter reaches round_count.
    EXPECT_EQ(mode->NextTransition(15, 16), EventKind::kStart);
    EXPECT_EQ(mode->NextTransition(16, 16), EventKind::kEnd);
    EXPECT_EQ(mode->NextTransition(17, 16), EventKind::kEnd);
}

TEST(ModeControllerTest, DefaultSettlementContinues) {
    GameConfig config;
    config.mode = GameMode::kStandard;
    auto mode = CreateModeController(config);
    ASSERT_NE(mode, nullptr);

    const std::array<int, 4> scores{0, 0, 0, 0};
    EXPECT_EQ(mode->OnRoundSettled(0, {}, scores), TransitionDecision::kContinue);
}

// ---------------------------------------------------------------------------
// 过五关 (Pass Five Gates) tests
// ---------------------------------------------------------------------------

namespace {

constexpr std::array<int, 4> kZeroScores{0, 0, 0, 0};

// Builds a pass-five-gates controller with a team assignment:
// players 1,2 -> team 0 (虎); players 3,4 -> team 1 (龙).
std::unique_ptr<ModeController> MakeFiveGates(
    PassFiveGatesConfig cfg = PassFiveGatesConfig{}) {
    GameConfig config;
    config.mode = GameMode::kPassFiveGates;
    config.round_count = 0;  // unlimited by default
    if (cfg.team.empty()) {
        cfg.team = {{1, 0}, {2, 0}, {3, 1}, {4, 1}};
    }
    config.pass_five_gates = std::move(cfg);
    return CreateModeController(config);
}

// Runs OnSessionStart with a fixed seed container, then returns mode_state.
Json::Value StartAndState(ModeController& mode, std::uint64_t seed_value) {
    random::SeedContainer seeds;
    seeds.RecordTraffic(seed_value);
    mode.OnSessionStart(&seeds);
    Json::Value mode_state(Json::objectValue);
    mode.SerializeState(mode_state);
    return mode_state;
}

// Looks up the qingque fan-table index for a name. Used to synthesize raw
// fan-index lists that complete specific targets.
int FanIndex(std::string_view name) {
    for (std::size_t i = 0; i < qingque::fans.size(); ++i) {
        if (qingque::fans[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

TEST(PassFiveGatesTest, DrawsFiveTargetsFromAllThreePools) {
    auto mode = MakeFiveGates();
    const Json::Value state = StartAndState(*mode, 0xABCDEF);
    const Json::Value& targets = state["targets"];
    ASSERT_TRUE(targets.isArray());
    ASSERT_EQ(targets.size(), 5U);

    std::set<std::string> names;
    for (const auto& target : targets) {
        ASSERT_TRUE(target.isString());
        names.insert(target.asString());
    }
    EXPECT_EQ(names.size(), 5U);  // no duplicates
    // All targets must be known fan names (or the composite marker).
    for (const auto& name : names) {
        if (name == "四大偶然番") {
            continue;
        }
        EXPECT_GE(FanIndex(name), 0) << "unknown target name: " << name;
    }
}

TEST(PassFiveGatesTest, DrawIsReproducibleWithSameSeed) {
    auto first = MakeFiveGates();
    const Json::Value state1 = StartAndState(*first, 42);
    auto second = MakeFiveGates();
    const Json::Value state2 = StartAndState(*second, 42);

    EXPECT_EQ(state1["targets"].toStyledString(), state2["targets"].toStyledString());
}

TEST(PassFiveGatesTest, HierarchyRulesAreRespected) {
    // Scan many seeds; assert no drawn set violates any hierarchy chain.
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        auto mode = MakeFiveGates();
        const Json::Value state = StartAndState(*mode, seed);
        std::set<std::string> drawn;
        for (const auto& target : state["targets"]) {
            drawn.insert(target.asString());
        }
        // superior may not coexist with subordinate
        EXPECT_FALSE(drawn.contains("七对") && drawn.contains("叠对"))
            << "seed " << seed << ": 七对 and 叠对 drawn together";
        EXPECT_FALSE(drawn.contains("混一色") && drawn.contains("清一色"))
            << "seed " << seed << ": 混一色 and 清一色 drawn together";
        EXPECT_FALSE(drawn.contains("番牌二副") &&
                     (drawn.contains("番牌二刻") || drawn.contains("番牌三副")))
            << "seed " << seed << ": 番牌二副 conflicts";
        EXPECT_FALSE(drawn.contains("混带幺") && drawn.contains("清带幺"))
            << "seed " << seed << ": 混带幺 and 清带幺 drawn together";
        EXPECT_FALSE(drawn.contains("四聚") && drawn.contains("三聚"))
            << "seed " << seed << ": 四聚 and 三聚 drawn together";
    }
}

TEST(PassFiveGatesTest, CompositeOccasionalTargetCompletesOnAnyOccasionalFan) {
    // Scan for a seed whose drawn set includes the composite 四大偶然番, then
    // complete it with a single occasional fan (岭上开花).
    auto mode = MakeFiveGates();
    Json::Value state;
    std::uint64_t found_seed = 0;
    for (std::uint64_t seed = 1; seed <= 100 && found_seed == 0; ++seed) {
        mode = MakeFiveGates();
        state = StartAndState(*mode, seed);
        for (const auto& target : state["targets"]) {
            if (target.asString() == "四大偶然番") {
                found_seed = seed;
                break;
            }
        }
    }
    ASSERT_NE(found_seed, 0U) << "no seed drew the composite target";
    mode->SetSeatPlayers({1, 2, 3, 4});
    const std::array<int, 4> scores{100, 0, 0, 0};

    // Seat 0 (player 1, team 0) completes 岭上开花 -> composite target done.
    const int ling_shang = FanIndex("岭上开花");
    ASSERT_GE(ling_shang, 0);
    const auto decision = mode->OnRoundSettled(0, {ling_shang}, scores);
    EXPECT_EQ(decision, TransitionDecision::kContinue);  // only 1 of 5 done

    Json::Value mode_state(Json::objectValue);
    mode->SerializeState(mode_state);
    const std::uint64_t team0_completed = mode_state["completed"][0].asUInt64();
    EXPECT_EQ(CountBitsForTest(team0_completed), 1U);
}

TEST(PassFiveGatesTest, OneHandCanCompleteMultipleTargets) {
    auto mode = MakeFiveGates();
    const Json::Value state = StartAndState(*mode, 99);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // Find the targets for team 0 and synthesize raw fan indices for two of them.
    std::vector<std::string> team0_targets;
    for (const auto& target : state["targets"]) {
        team0_targets.push_back(target.asString());
    }
    // Pick two distinct non-composite targets and complete both in one hand.
    std::vector<int> raw;
    int completed_count = 0;
    for (const auto& name : team0_targets) {
        if (name == "四大偶然番") {
            continue;
        }
        const int index = FanIndex(name);
        if (index < 0) {
            continue;
        }
        raw.push_back(index);
        if (++completed_count >= 2) {
            break;
        }
    }
    ASSERT_GE(completed_count, 2) << "expected at least two simple targets";

    const std::array<int, 4> scores{100, 0, 0, 0};
    const auto decision = mode->OnRoundSettled(0, raw, scores);
    EXPECT_EQ(decision, TransitionDecision::kContinue);

    Json::Value mode_state(Json::objectValue);
    mode->SerializeState(mode_state);
    const std::uint64_t team0_completed = mode_state["completed"][0].asUInt64();
    EXPECT_EQ(CountBitsForTest(team0_completed), 2U);
}

TEST(PassFiveGatesTest, CompletingAllFiveEndsWithWinnerTeam) {
    auto mode = MakeFiveGates();
    const Json::Value state = StartAndState(*mode, 123);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // Collect every simple target index (excluding composite) for the session.
    std::vector<int> all_simple;
    for (const auto& target : state["targets"]) {
        if (target.asString() == "四大偶然番") {
            continue;
        }
        const int index = FanIndex(target.asString());
        if (index >= 0) {
            all_simple.push_back(index);
        }
    }
    // If the composite is present, complete it with an occasional fan too.
    std::vector<int> raw = all_simple;
    bool has_composite = false;
    for (const auto& target : state["targets"]) {
        if (target.asString() == "四大偶然番") {
            has_composite = true;
            raw.push_back(FanIndex("抢杠"));
        }
    }
    if (!has_composite) {
        // All 5 were simple targets; raw already covers them.
    }

    const std::array<int, 4> scores{100, 0, 0, 0};
    const auto decision = mode->OnRoundSettled(0, raw, scores);
    EXPECT_EQ(decision, TransitionDecision::kEnd);

    Json::Value mode_state(Json::objectValue);
    mode->SerializeState(mode_state);
    EXPECT_EQ(mode_state["winner_team"].asInt(), 0);

    // Session end result attaches winner team + completed bitmaps.
    const std::array<int, 4> final_scores{100, 0, -50, -50};
    const Json::Value result = mode->OnSessionEnd(final_scores);
    EXPECT_EQ(result["winner_team"].asInt(), 0);
    EXPECT_FALSE(result["draw"].asBool());
    EXPECT_EQ(result["targets"].size(), 5U);
}

TEST(PassFiveGatesTest, KnockoutDecidesWinner) {
    PassFiveGatesConfig cfg;
    cfg.knockout_score = 100;
    auto mode = MakeFiveGates(std::move(cfg));
    StartAndState(*mode, 5);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // Team 1 (players 3,4) both knocked out (score < -100); team 0 wins.
    const std::array<int, 4> scores{0, 0, -150, -120};
    const auto decision = mode->OnRoundSettled(1, {}, scores);
    EXPECT_EQ(decision, TransitionDecision::kEnd);

    Json::Value mode_state(Json::objectValue);
    mode->SerializeState(mode_state);
    EXPECT_EQ(mode_state["winner_team"].asInt(), 0);
}

TEST(PassFiveGatesTest, EqualKnockoutCountIsDraw) {
    PassFiveGatesConfig cfg;
    cfg.knockout_score = 100;
    auto mode = MakeFiveGates(std::move(cfg));
    StartAndState(*mode, 6);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // One knocked-out player on each team -> draw.
    const std::array<int, 4> scores{-150, 0, -160, 0};
    const auto decision = mode->OnRoundSettled(1, {}, scores);
    EXPECT_EQ(decision, TransitionDecision::kEnd);

    Json::Value mode_state(Json::objectValue);
    mode->SerializeState(mode_state);
    EXPECT_EQ(mode_state["winner_team"].asInt(), -2);  // kDraw
}

TEST(PassFiveGatesTest, RoundLimitTieBreakByCompletedThenScore) {
    auto mode = MakeFiveGates();
    StartAndState(*mode, 8);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // Team 0 completes one target, team 1 completes none; no winner yet.
    const int fan = FanIndex("七对");
    ASSERT_GE(fan, 0);
    const std::array<int, 4> scores{50, 0, 0, 0};
    EXPECT_EQ(mode->OnRoundSettled(0, {fan}, scores), TransitionDecision::kContinue);

    // Session ends by round limit (round_count>0): team 0 has more completed.
    const std::array<int, 4> final_scores{50, 0, 0, 0};
    const Json::Value result = mode->OnSessionEnd(final_scores);
    EXPECT_EQ(result["winner_team"].asInt(), 0);
}

TEST(PassFiveGatesTest, RoundLimitTieBreaksByScoreWhenCompletedEqual) {
    auto mode = MakeFiveGates();
    StartAndState(*mode, 9);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // No target completed by either team; team 0 has higher total score.
    const std::array<int, 4> final_scores{100, 0, -40, -60};
    const Json::Value result = mode->OnSessionEnd(final_scores);
    EXPECT_EQ(result["winner_team"].asInt(), 0);
}

TEST(PassFiveGatesTest, ModeUpdateIsRepeatedlySerializableForBroadcast) {
    auto mode = MakeFiveGates();
    const Json::Value state = StartAndState(*mode, 77);
    mode->SetSeatPlayers({1, 2, 3, 4});

    // Complete one simple target for team 0.
    std::vector<int> raw;
    bool done = false;
    for (const auto& target : state["targets"]) {
        if (target.asString() == "四大偶然番") {
            continue;
        }
        const int index = FanIndex(target.asString());
        if (index >= 0) {
            raw.push_back(index);
            done = true;
            break;
        }
    }
    ASSERT_TRUE(done) << "expected at least one simple target";

    const std::array<int, 4> scores{50, 0, 0, 0};
    EXPECT_EQ(mode->OnRoundSettled(0, raw, scores), TransitionDecision::kContinue);

    // The mode update must be reproducible for every broadcast recipient
    // (previously it was consumed by the first SerializeEvent call).
    Json::Value first(Json::objectValue);
    mode->SerializeEvent(first);
    ASSERT_TRUE(first.isMember("mode_update")) << "expected a mode_update";
    ASSERT_TRUE(first["mode_update"]["completed_now"].isArray());
    ASSERT_GE(first["mode_update"]["completed_now"].size(), 1U);

    Json::Value second(Json::objectValue);
    mode->SerializeEvent(second);
    ASSERT_TRUE(second.isMember("mode_update")) << "second SerializeEvent must still report the update";
    EXPECT_EQ(first["mode_update"].toStyledString(), second["mode_update"].toStyledString());

    // A new round clears the pending update.
    mode->OnRoundStart(2);
    Json::Value after_round(Json::objectValue);
    mode->SerializeEvent(after_round);
    EXPECT_FALSE(after_round.isMember("mode_update"));
}

}  // namespace
}  // namespace mmcr::game
