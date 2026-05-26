#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>

#include "storage/database.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::ranking {

constexpr double kDefaultMu = 0.0;
constexpr double kDefaultTau = 15.0;
constexpr double kDefaultSigma = 300.0;
constexpr double kDefaultPoints = 0.0;
constexpr int kDefaultLevel = 0;

constexpr double kTimeDeviationGrowthRate = 5e-7;  // δ² in s⁻¹
constexpr double kVolatilityLearningRate = 1e9;     // η
constexpr double kPointAlpha = 0.85;
constexpr double kPointBeta = 5.0;
constexpr double kPointGamma = 1.0;

struct RatingEntry {
    std::int64_t player_id{0};
    double mu{kDefaultMu};
    double tau{kDefaultTau};
    double sigma{kDefaultSigma};
    double points{kDefaultPoints};
    int level{kDefaultLevel};
    std::int64_t last_update_ts{0};
    std::int64_t total_games{0};
};

struct SessionScore {
    std::int64_t player_id;
    double score;  // y_i: net score in the session
};

struct SessionUpdateResult {
    std::array<double, 4> delta_mu{};
    std::array<double, 4> delta_points{};
};

class RatingService {
public:
    explicit RatingService(storage::Database* database);

    [[nodiscard]] auto InitializeSchema(const std::filesystem::path& migrations_dir) -> util::Status;

    [[nodiscard]] auto GetRating(std::int64_t player_id, std::int64_t now_ts)
        -> util::StatusOr<RatingEntry>;

    [[nodiscard]] auto UpdateAfterSession(const std::array<SessionScore, 4>& scores,
                                           int round_count,
                                           std::int64_t now_ts)
        -> util::StatusOr<SessionUpdateResult>;

    [[nodiscard]] auto ComputeRankLevel(double points, int previous_level) const -> int;

private:
    [[nodiscard]] auto load_rating_locked(std::int64_t player_id, std::int64_t now_ts)
        -> util::StatusOr<RatingEntry>;
    [[nodiscard]] auto save_rating_locked(const RatingEntry& entry) -> util::Status;
    void ensure_player_initialized_locked(std::int64_t player_id, std::int64_t now_ts);

    storage::Database* database_;
    mutable std::recursive_mutex mutex_;
};

}  // namespace mmcr::ranking
