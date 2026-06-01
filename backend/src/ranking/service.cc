#include "ranking/service.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>

#include "storage/migration.h"
#include "util/status.h"

namespace mmcr::ranking {
namespace {

auto DoubleToText(double value) -> std::string {
    std::ostringstream stream;
    stream.precision(15);
    stream << std::fixed << value;
    return stream.str();
}

auto TextToDouble(std::string_view text) -> util::StatusOr<double> {
    try {
        std::size_t pos = 0;
        const double result = std::stod(std::string(text), &pos);
        if (pos != text.size()) {
            return util::Status::InvalidArgument("invalid double text");
        }
        return result;
    } catch (...) {
        return util::Status::InvalidArgument("failed to parse double text");
    }
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

}  // namespace

RatingService::RatingService(storage::Database* database)
    : database_(database) {}

auto RatingService::InitializeSchema(const std::filesystem::path& migrations_dir) -> util::Status {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    storage::MigrationRunner runner(database_);
    return runner.ApplyDirectory(migrations_dir);
}

auto RatingService::GetRating(std::int64_t player_id, std::int64_t now_ts)
    -> util::StatusOr<RatingEntry> {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ensure_player_initialized_locked(player_id, now_ts);
    return load_rating_locked(player_id, now_ts);
}

auto RatingService::UpdateAfterSession(const std::array<SessionScore, 4>& scores,
                                        int round_count,
                                        std::int64_t now_ts)
    -> util::StatusOr<SessionUpdateResult> {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }
    if (round_count <= 0) {
        return util::Status::InvalidArgument("round_count must be positive");
    }

    const double m = static_cast<double>(round_count);
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::array<RatingEntry, 4> ratings;
    for (int i = 0; i < 4; ++i) {
        ensure_player_initialized_locked(scores[i].player_id, now_ts);
        auto entry = load_rating_locked(scores[i].player_id, now_ts);
        if (!entry.ok()) return entry.status();
        ratings[i] = entry.value();
    }

    const double mu_avg = (ratings[0].mu + ratings[1].mu + ratings[2].mu + ratings[3].mu) / 4.0;

    std::array<double, 4> s;
    for (int i = 0; i < 4; ++i) {
        s[i] = scores[i].score - m * (ratings[i].mu - mu_avg);
    }

    double sum_inv_sigma2 = 0.0;
    double s_p_bar_num = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double inv = 1.0 / (ratings[i].sigma * ratings[i].sigma);
        sum_inv_sigma2 += inv;
        s_p_bar_num += s[i] * inv;
    }
    const double s_p_bar = sum_inv_sigma2 > 0.0 ? s_p_bar_num / sum_inv_sigma2 : 0.0;

    double sum_denom = 0.0;
    double s_q_bar_num = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double denom = m * ratings[i].tau * ratings[i].tau
            + ratings[i].sigma * ratings[i].sigma;
        const double inv = 1.0 / denom;
        sum_denom += inv;
        s_q_bar_num += s[i] * inv;
    }
    const double s_q_bar = sum_denom > 0.0 ? s_q_bar_num / sum_denom : 0.0;

    SessionUpdateResult result{};
    for (int i = 0; i < 4; ++i) {
        const double tau2 = ratings[i].tau * ratings[i].tau;
        const double sigma2 = ratings[i].sigma * ratings[i].sigma;
        const double denom_i = m * tau2 + sigma2;

        double correction = 0.0;
        for (int k = 0; k < 4; ++k) {
            const double tau_k2 = ratings[k].tau * ratings[k].tau;
            const double denom_k = m * tau_k2 + ratings[k].sigma * ratings[k].sigma;
            correction += tau_k2 * (s[k] - s_p_bar)
                / (ratings[k].sigma * ratings[k].sigma * denom_k);
        }
        const double mu_new = ratings[i].mu
            + (tau2 / denom_i) * (s[i] - s_p_bar + (m / sum_denom) * correction);

        result.delta_mu[i] = mu_new - ratings[i].mu;

        const double tau2_new = (tau2 * sigma2) / denom_i
            + (m / sum_denom) * (tau2 * tau2) / (denom_i * denom_i);

        const double diff = s[i] - s_q_bar;
        // old volatility update is deprecated because SGD on sigma2 is instable
        // const double sigma2_new = sigma2
        //     + (kVolatilityLearningRate / (denom_i * denom_i))
        //         * ((diff * diff / m) - denom_i + (1.0 / sum_denom));
        // use SGD on log sigma instead
        const double sigma_new = ratings[i].sigma * std::exp(
            kLogVolatilityLearningRate * 2 * sigma2 / (denom_i * denom_i) 
                * ((diff * diff / m) - denom_i + (1.0 / sum_denom)));

        ratings[i].mu = mu_new;
        ratings[i].tau = std::sqrt(std::max(tau2_new, 0.0));
        ratings[i].sigma = sigma_new;
        ratings[i].last_update_ts = now_ts;

        ratings[i].total_games += round_count;

        const double g = static_cast<double>(std::max<std::int64_t>(1, ratings[i].total_games));
        const double sqrt_g = std::sqrt(g);
        const double inner = std::log(std::pow(g, 1.0 / kPointBeta) + std::exp(mu_new / kPointBeta));
        const double scale = (kPointBeta * kPointGamma) / sqrt_g;
        const double points_new = kPointAlpha * ratings[i].points
            + ((1.0 - kPointAlpha) / kPointGamma) * sqrt_g * std::tanh(scale * inner);
        result.delta_points[i] = points_new - ratings[i].points;
        ratings[i].points = points_new;

        ratings[i].level = ComputeRankLevel(ratings[i].points, ratings[i].level);

        auto status = save_rating_locked(ratings[i]);
        if (!status.ok()) return status;
    }

    return result;
}

auto RatingService::ComputeRankLevel(double points, int previous_level) const -> int {
    auto threshold = [](int l) -> double {
        switch (l) {
            case 1: return 4.0;
            case 2: return 8.0;
            case 3: return 11.0;
            case 4: return 14.0;
            default: return 2.0 * static_cast<double>(l) + 6.0;
        }
    };
    // Find smallest l' >= 1 such that P < T_{l'}
    int lp = 1;
    while (points >= threshold(lp)) ++lp;

    const int prev = previous_level;
    if (lp >= prev + 2) {
        return lp - 1;  // promotion
    }
    if (lp <= prev - 1) {
        return lp;  // demotion
    }
    return prev;  // unchanged
}

void RatingService::ensure_player_initialized_locked(std::int64_t player_id, std::int64_t now_ts) {
    auto check = database_->Prepare(
        "SELECT rating_mu FROM players WHERE id = ?1 LIMIT 1;");
    if (!check.ok()) return;
    auto& stmt = check.value();
    (void)stmt.BindInt64(1, player_id);
    auto step = stmt.Step();
    if (!step.ok()) return;
    if (step.value() == storage::Statement::StepResult::kRow) {
        const std::string text = stmt.ColumnText(0);
        if (!text.empty() && text != "NULL") return;
    }

    auto insert = database_->Prepare(
        "UPDATE players SET rating_mu = ?1, rating_tau = ?2, rating_sigma = ?3, "
        "rating_points = ?4, rating_level = 0, rating_last_update_ts = ?5, "
        "rating_total_games = 0 "
        "WHERE id = ?6;");
    if (!insert.ok()) return;
    auto& ins = insert.value();
    (void)ins.BindText(1, DoubleToText(kDefaultMu));
    (void)ins.BindText(2, DoubleToText(kDefaultTau));
    (void)ins.BindText(3, DoubleToText(kDefaultSigma));
    (void)ins.BindText(4, DoubleToText(kDefaultPoints));
    (void)ins.BindInt64(5, now_ts);
    (void)ins.BindInt64(6, player_id);
    (void)StepDone(ins);
}

auto RatingService::load_rating_locked(std::int64_t player_id, std::int64_t now_ts)
    -> util::StatusOr<RatingEntry> {
    auto statement = database_->Prepare(
        "SELECT rating_mu, rating_tau, rating_sigma, rating_points, rating_level, "
        "rating_last_update_ts, rating_total_games FROM players WHERE id = ?1 LIMIT 1;");
    if (!statement.ok()) return statement.status();
    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) return status;
    auto step = stmt.Step();
    if (!step.ok()) return step.status();
    if (step.value() == storage::Statement::StepResult::kDone) {
        return util::Status::NotFound("player not found");
    }

    RatingEntry entry;
    entry.player_id = player_id;
    auto mu = TextToDouble(stmt.ColumnText(0));
    auto tau = TextToDouble(stmt.ColumnText(1));
    auto sigma = TextToDouble(stmt.ColumnText(2));
    auto points = TextToDouble(stmt.ColumnText(3));
    if (!mu.ok() || !tau.ok() || !sigma.ok() || !points.ok()) {
        return util::Status::Internal("failed to parse rating doubles");
    }
    entry.mu = mu.value();
    entry.tau = tau.value();
    entry.sigma = sigma.value();
    entry.points = points.value();
    entry.level = static_cast<int>(stmt.ColumnInt64(4));
    entry.last_update_ts = stmt.ColumnInt64(5);
    entry.total_games = stmt.ColumnInt64(6);

    if (now_ts > entry.last_update_ts && entry.last_update_ts > 0) {
        const double dt = static_cast<double>(now_ts - entry.last_update_ts);
        const double tau2_drifted = entry.tau * entry.tau + kTimeDeviationGrowthRate * dt;
        const double tau_max2 = kDefaultTau * kDefaultTau;
        entry.tau = std::sqrt(std::min(tau2_drifted, tau_max2));
    }
    return entry;
}

auto RatingService::save_rating_locked(const RatingEntry& entry) -> util::Status {
    auto statement = database_->Prepare(
        "UPDATE players SET rating_mu = ?1, rating_tau = ?2, rating_sigma = ?3, "
        "rating_points = ?4, rating_level = ?5, rating_last_update_ts = ?6, "
        "rating_total_games = ?7 WHERE id = ?8;");
    if (!statement.ok()) return statement.status();
    auto& stmt = statement.value();
    auto status = stmt.BindText(1, DoubleToText(entry.mu));
    if (!status.ok()) return status;
    status = stmt.BindText(2, DoubleToText(entry.tau));
    if (!status.ok()) return status;
    status = stmt.BindText(3, DoubleToText(entry.sigma));
    if (!status.ok()) return status;
    status = stmt.BindText(4, DoubleToText(entry.points));
    if (!status.ok()) return status;
    status = stmt.BindInt64(5, entry.level);
    if (!status.ok()) return status;
    status = stmt.BindInt64(6, entry.last_update_ts);
    if (!status.ok()) return status;
    status = stmt.BindInt64(7, entry.total_games);
    if (!status.ok()) return status;
    status = stmt.BindInt64(8, entry.player_id);
    if (!status.ok()) return status;
    return StepDone(stmt);
}

}  // namespace mmcr::ranking

