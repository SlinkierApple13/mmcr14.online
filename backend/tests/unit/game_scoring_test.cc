#include "external/qingque/rules/qingque.h"
#include "external/qingque/rules/w_data.h"

#include <gtest/gtest.h>

namespace mmcr::game {
namespace {

TEST(GameScoringTest, LoadsFanCacheWithoutCrowOrLegacyAssets) {
    const auto wd = qingque_wd::get_wd();

    EXPECT_FALSE(wd.fan_cache.empty());
    EXPECT_EQ(wd.fan_cache.size(), qingque_wd::fan_cache(wd).size());
}

TEST(GameScoringTest, MissingFanCacheEntriesReturnZero) {
    qingque::w_data wd;
    qingque::fan_code code;
    code.set(0);

    EXPECT_DOUBLE_EQ(0.0, qingque::get_fan(wd, code));
}

}  // namespace
}  // namespace mmcr::game