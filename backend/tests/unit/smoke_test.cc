#include <cstdint>
#include <random>

#include <gtest/gtest.h>

#include "random/seed.h"
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