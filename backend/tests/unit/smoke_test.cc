#include <array>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "random/seed.h"
#include "random/shuffle.h"
#include "random/uniform_int_distribution.h"
#include "util/status.h"

TEST(StatusTest, FormatsNonOkStatus) {
    const auto status = mmcr::util::Status::InvalidArgument("bad tile index");

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), mmcr::util::StatusCode::kInvalidArgument);
    EXPECT_EQ(status.DebugString(), "invalid_argument: bad tile index");
}

TEST(RandomTest, SeedContainerUsesCappedQueueAndFallback) {
    // Mirrors FastMix in src/random/seed.cc: recorded traffic values are
    // mixed before storage, and the fallback mixes from_value with the
    // previously extracted value.
    constexpr auto fast_mix = [](std::uint64_t seed) -> std::uint64_t {
        seed ^= seed >> 33;
        seed *= 0xff51afd7ed558ccdULL;
        seed ^= seed >> 33;
        seed *= 0xc4ceb9fe1a85ec53ULL;
        seed ^= seed >> 33;
        return seed;
    };

    mmcr::random::SeedContainer container(2);

    container.RecordTraffic(100ULL);
    container.RecordTraffic(200ULL);
    container.RecordTraffic(300ULL);

    const auto first = fast_mix(100ULL);
    const auto second = fast_mix(200ULL);
    const auto third = fast_mix(300ULL);

    EXPECT_EQ(container.size(), 2U);
    EXPECT_EQ(container.Extract(400ULL), second);
    EXPECT_EQ(container.Extract(500ULL), third);

    EXPECT_EQ(container.Extract(600ULL), fast_mix(600ULL ^ third));
    EXPECT_NE(first, second);
}

TEST(RandomTest, DrawHexConsumesValuesFromSeedContainer) {
    mmcr::random::SeedContainer container(1);
    container.RecordTraffic(1234ULL);

    const auto hex = mmcr::random::DrawHex(container, 8);
    ASSERT_TRUE(hex.ok()) << hex.status().DebugString();
    EXPECT_EQ(hex.value().size(), 16U);
    EXPECT_EQ(container.size(), 0U);
}

// The following golden values pin the libstdc++ behaviour that wall rebuilds
// rely on.  Changing them means old records no longer replay as recorded.

TEST(RandomTest, UniformIntDistributionMatchesRecordedDice) {
    std::mt19937_64 engine(42);
    mmcr::random::uniform_int_distribution<int> distribution(1, 6);

    const std::array<int, 12> expected{5, 4, 5, 1, 6, 1, 4, 3, 2, 3, 1, 4};
    for (const int value : expected) {
        EXPECT_EQ(value, distribution(engine));
    }
}

TEST(RandomTest, ShuffleMatchesRecordedPermutation) {
    std::vector<int> values(16);
    std::iota(values.begin(), values.end(), 0);

    std::mt19937_64 engine(0xabcdef1234567890ULL);
    mmcr::random::shuffle(values.begin(), values.end(), engine);

    const std::vector<int> expected{4, 8, 0, 13, 7, 12, 15, 6,
                                    1, 10, 2, 3, 11, 5, 9, 14};
    EXPECT_EQ(values, expected);
}

TEST(RandomTest, SeatShuffleMatchesRecordedPermutation) {
    std::array<int, 4> seats{0, 1, 2, 3};
    std::mt19937_64 engine(1);
    mmcr::random::shuffle(seats.begin(), seats.end(), engine);

    EXPECT_EQ(seats, (std::array<int, 4>{2, 3, 1, 0}));
}

TEST(RandomTest, WallRebuildSequenceIsStable) {
    // Mirrors Wall::prepare: one shuffle with the first seed, then one
    // shuffle over the 136 tile slots per seed, then the dice roll.
    std::vector<std::uint64_t> seeds;
    for (int i = 0; i < 16; ++i) {
        seeds.push_back(0x123456789abcdef0ULL +
                        static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL);
    }

    std::vector<int> tiles(136);
    std::iota(tiles.begin(), tiles.end(), 0);
    std::mt19937_64 aux_rng(seeds[0]);
    mmcr::random::shuffle(tiles.begin(), tiles.end(), aux_rng);

    std::array<int, 136> wall{};
    for (std::size_t i = 0; i < wall.size(); ++i) {
        wall[i] = tiles[i % tiles.size()];
    }
    for (const auto seed : seeds) {
        std::mt19937_64 rng(seed);
        mmcr::random::shuffle(wall.begin(), wall.end(), rng);
    }

    const std::array<int, 8> expected_front{45, 2, 13, 115, 50, 87, 62, 31};
    for (std::size_t i = 0; i < expected_front.size(); ++i) {
        EXPECT_EQ(expected_front[i], wall[i]);
    }
    EXPECT_EQ(wall[135], 91);

    std::array<int, 4> dices{};
    mmcr::random::uniform_int_distribution<int> distribution(1, 6);
    for (auto& dice : dices) {
        dice = distribution(aux_rng);
    }
    EXPECT_EQ(dices, (std::array<int, 4>{6, 4, 2, 2}));
    EXPECT_EQ(((dices[0] + dices[1]) * 52 + (dices[2] + dices[3])) % 68, 48);
}