#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "random/seed.h"
#include "storage/database.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::auth {

struct Argon2Config {
    std::uint32_t time_cost{2};
    std::uint32_t memory_cost_kib{4096};
    std::uint32_t parallelism{1};
    std::size_t salt_bytes{16};
    std::size_t hash_bytes{32};
};

struct AuthConfig {
    std::int64_t session_ttl_ms{7LL * 24LL * 60LL * 60LL * 1000LL};
    Argon2Config hashing{};
};

struct PlayerProfile {
    std::int64_t player_id{0};
    std::string username;
};

class PlayerProfilePtr {
public:
    PlayerProfilePtr() = default;

    explicit PlayerProfilePtr(const std::shared_ptr<PlayerProfile>& player)
        : player_id_(player != nullptr ? player->player_id : 0),
          player_(player) {}

    [[nodiscard]] auto valid() const noexcept -> bool {
        return player_id_ > 0 && !player_.expired() && player_.lock() != nullptr;
    }

    [[nodiscard]] auto player_id() const noexcept -> std::int64_t {
        return player_id_;
    }

    [[nodiscard]] auto matches(std::int64_t player_id) const noexcept -> bool {
        return player_id_ != 0 && player_id_ == player_id;
    }

    [[nodiscard]] auto lock() const -> std::shared_ptr<const PlayerProfile> {
        return player_.lock();
    }

    void reset() noexcept {
        player_id_ = 0;
        player_.reset();
    }

private:
    std::int64_t player_id_{0};
    std::weak_ptr<PlayerProfile> player_;
};

struct SessionInfo {
    std::string session_id;
    std::string token;
    std::int64_t created_at_ms{0};
    std::int64_t expires_at_ms{0};
};

struct AuthenticatedSession {
    PlayerProfile player;
    SessionInfo session;
};

struct RegisterRequest {
    std::string username;
    std::string password;
    std::int64_t now_ms{0};
};

struct RegisterResult {
    PlayerProfile player;
};

struct LoginRequest {
    std::string identity;
    std::string password;
    std::int64_t now_ms{0};
};

struct ChangePasswordRequest {
    std::string session_token;
    std::string current_password;
    std::string new_password;
    std::int64_t now_ms{0};
};

class AuthService {
public:
    explicit AuthService(storage::Database* database,
                         AuthConfig config = {},
                         random::SeedContainer* seed_container = nullptr);

    [[nodiscard]] util::Status InitializeSchema(const std::filesystem::path& migrations_dir);
    [[nodiscard]] util::StatusOr<RegisterResult> Register(const RegisterRequest& request);
    [[nodiscard]] util::StatusOr<AuthenticatedSession> Login(const LoginRequest& request);
    [[nodiscard]] util::StatusOr<AuthenticatedSession> Authenticate(
        std::string_view session_token,
        std::int64_t now_ms) const;
    [[nodiscard]] util::StatusOr<AuthenticatedSession> RefreshSession(
        std::string_view session_token,
        std::int64_t now_ms);
    [[nodiscard]] util::Status Logout(std::string_view session_token, std::int64_t now_ms);
    [[nodiscard]] util::Status ChangePassword(const ChangePasswordRequest& request);

private:
    [[nodiscard]] util::StatusOr<AuthenticatedSession> AuthenticateInternal(
        std::string_view session_token,
        std::int64_t now_ms) const;

    std::unique_ptr<random::SeedContainer> owned_seed_container_;
    storage::Database* database_;
    AuthConfig config_;
    random::SeedContainer* seed_container_;
};

}  // namespace mmcr::auth