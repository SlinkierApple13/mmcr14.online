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

TEST(RandomTest, SplitMix64ProducesDeterministicSequence) {
    mmcr::random::SplitMix64 first(123456789ULL);
    mmcr::random::SplitMix64 second(123456789ULL);

    EXPECT_EQ(first.Next(), second.Next());
    EXPECT_EQ(first.Next(), second.Next());
    EXPECT_EQ(first.Next(), second.Next());
}

TEST(RandomTest, DerivedMatchSeedDependsOnMatchIndex) {
    const auto first = mmcr::random::DeriveMatchSeed(42ULL, 0ULL);
    const auto second = mmcr::random::DeriveMatchSeed(42ULL, 1ULL);
    const auto repeat = mmcr::random::DeriveMatchSeed(42ULL, 0ULL);

    EXPECT_EQ(first, repeat);
    EXPECT_NE(first.value, second.value);
}

TEST(RandomTest, SeedContainerUsesCappedQueueAndFallback) {
    mmcr::random::SeedContainer container(2);

    container.RecordTraffic(100ULL);
    container.RecordTraffic(200ULL);
    container.RecordTraffic(300ULL);

    std::mt19937_64 first_generator(100ULL);
    std::mt19937_64 second_generator(200ULL);
    std::mt19937_64 third_generator(300ULL);
    const auto first = first_generator();
    const auto second = second_generator();
    const auto third = third_generator();

    EXPECT_EQ(container.size(), 2U);
    EXPECT_EQ(container.Extract(400ULL), second);
    EXPECT_EQ(container.Extract(500ULL), third);

    std::mt19937_64 fallback_generator(600ULL ^ third);
    EXPECT_EQ(container.Extract(600ULL), fallback_generator());
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