#include "auth/auth_internal.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "storage/migration.h"

namespace mmcr::auth {

AuthService::AuthService(storage::Database* database,
                         AuthConfig config,
                         random::SeedContainer* seed_container)
    : owned_seed_container_(seed_container == nullptr ? std::make_unique<random::SeedContainer>()
                                                      : nullptr),
      database_(database),
      config_(std::move(config)),
      seed_container_(seed_container != nullptr ? seed_container : owned_seed_container_.get()) {}

util::Status AuthService::InitializeSchema(const std::filesystem::path& migrations_dir) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    storage::MigrationRunner runner(database_);
    return runner.ApplyDirectory(migrations_dir);
}

util::StatusOr<RegisterResult> AuthService::Register(const RegisterRequest& request) {
    ObserveTraffic(seed_container_);

    auto status = ValidateRegistration(request);
    if (!status.ok()) {
        return status;
    }

    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    const auto username_normalized = Normalize(request.username);

    auto username_exists = UsernameExists(database_, username_normalized);
    if (!username_exists.ok()) {
        return username_exists.status();
    }
    if (username_exists.value()) {
        return util::Status::InvalidArgument("username is already taken");
    }

    Argon2Hasher hasher(config_.hashing, seed_container_);

    auto password_hash = hasher.Hash(request.password);
    if (!password_hash.ok()) {
        return password_hash.status();
    }

    auto verification_token = MakeOpaqueToken(seed_container_, 16, 32);
    if (!verification_token.ok()) {
        return verification_token.status();
    }

    auto verification_hash = hasher.Hash(verification_token.value().secret);
    if (!verification_hash.ok()) {
        return verification_hash.status();
    }

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    auto player_id = InsertPlayer(
        database_,
        request.username,
        username_normalized,
        request.now_ms);
    if (!player_id.ok()) {
        return player_id.status();
    }

    status = InsertCredential(database_, player_id.value(), password_hash.value(), request.now_ms);
    if (!status.ok()) {
        return status;
    }

    status = transaction.value().Commit();
    if (!status.ok()) {
        return status;
    }

    return RegisterResult{
        PlayerProfile{player_id.value(), request.username},
    };
}

util::StatusOr<AuthenticatedSession> AuthService::Login(const LoginRequest& request) {
    ObserveTraffic(seed_container_);

    if (request.identity.empty()) {
        return util::Status::InvalidArgument("identity must not be empty");
    }
    if (request.password.empty()) {
        return util::Status::InvalidArgument("password must not be empty");
    }
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    auto player = LoadPlayerForIdentity(database_, Normalize(request.identity));
    if (!player.ok()) {
        return player.status();
    }

    Argon2Hasher hasher(config_.hashing, seed_container_);
    auto status = hasher.Verify(player.value().password_hash, request.password);
    if (!status.ok()) {
        return util::Status::InvalidArgument("invalid identity or password");
    }

    auto session_token = MakeOpaqueToken(seed_container_, 16, 32);
    if (!session_token.ok()) {
        return session_token.status();
    }

    auto session_hash = hasher.Hash(session_token.value().secret);
    if (!session_hash.ok()) {
        return session_hash.status();
    }

    const auto expires_at_ms = request.now_ms + config_.session_ttl_ms;
    status = InsertSession(
        database_,
        session_token.value().id,
        player.value().profile.player_id,
        session_hash.value(),
        request.now_ms,
        expires_at_ms);
    if (!status.ok()) {
        return status;
    }

    return AuthenticatedSession{
        player.value().profile,
        SessionInfo{
            session_token.value().id,
            session_token.value().combined(),
            request.now_ms,
            expires_at_ms,
        },
    };
}

util::StatusOr<AuthenticatedSession> AuthService::Authenticate(
    std::string_view session_token,
    std::int64_t now_ms) const {
    ObserveTraffic(seed_container_);
    return AuthenticateInternal(session_token, now_ms);
}

util::StatusOr<AuthenticatedSession> AuthService::AuthenticateInternal(
    std::string_view session_token,
    std::int64_t now_ms) const {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }

    auto token = ParseOpaqueToken(session_token);
    if (!token.ok()) {
        return token.status();
    }

    auto session = LoadSession(database_, token.value().id);
    if (!session.ok()) {
        return session.status();
    }
    if (session.value().revoked_at_ms != kMissingTimestampMs) {
        return util::Status::InvalidArgument("session has been revoked");
    }
    if (now_ms > session.value().expires_at_ms) {
        return util::Status::InvalidArgument("session has expired");
    }

    Argon2Hasher hasher(config_.hashing, seed_container_);
    auto status = hasher.Verify(session.value().secret_hash, token.value().secret);
    if (!status.ok()) {
        return util::Status::InvalidArgument("invalid session token");
    }

    return BuildSession(session.value(), std::string(session_token));
}

util::StatusOr<AuthenticatedSession> AuthService::RefreshSession(
    std::string_view session_token,
    std::int64_t now_ms) {
    ObserveTraffic(seed_container_);

    auto current = AuthenticateInternal(session_token, now_ms);
    if (!current.ok()) {
        return current.status();
    }

    Argon2Hasher hasher(config_.hashing, seed_container_);
    auto refreshed_token = MakeOpaqueToken(seed_container_, 16, 32);
    if (!refreshed_token.ok()) {
        return refreshed_token.status();
    }

    auto refreshed_hash = hasher.Hash(refreshed_token.value().secret);
    if (!refreshed_hash.ok()) {
        return refreshed_hash.status();
    }

    const auto expires_at_ms = now_ms + config_.session_ttl_ms;
    auto status = RotateSession(
        database_,
        current.value().session.session_id,
        refreshed_hash.value(),
        now_ms,
        expires_at_ms);
    if (!status.ok()) {
        return status;
    }

    current.value().session.token = current.value().session.session_id + "." + refreshed_token.value().secret;
    current.value().session.expires_at_ms = expires_at_ms;
    return std::move(current.value());
}

util::Status AuthService::Logout(std::string_view session_token, std::int64_t now_ms) {
    ObserveTraffic(seed_container_);

    auto current = AuthenticateInternal(session_token, now_ms);
    if (!current.ok()) {
        return current.status();
    }

    return RevokeSession(database_, current.value().session.session_id, now_ms, "logout");
}

util::Status AuthService::ChangePassword(const ChangePasswordRequest& request) {
    ObserveTraffic(seed_container_);

    auto status = ValidatePasswordUpdate(request.new_password);
    if (!status.ok()) {
        return status;
    }

    auto current = AuthenticateInternal(request.session_token, request.now_ms);
    if (!current.ok()) {
        return current.status();
    }

    auto password_hash = LoadPasswordHash(database_, current.value().player.player_id);
    if (!password_hash.ok()) {
        return password_hash.status();
    }

    Argon2Hasher hasher(config_.hashing, seed_container_);
    status = hasher.Verify(password_hash.value(), request.current_password);
    if (!status.ok()) {
        return util::Status::InvalidArgument("current password is incorrect");
    }

    auto new_hash = hasher.Hash(request.new_password);
    if (!new_hash.ok()) {
        return new_hash.status();
    }

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    status = UpdatePasswordHash(database_, current.value().player.player_id, new_hash.value(), request.now_ms);
    if (!status.ok()) {
        return status;
    }

    status = TouchPlayer(database_, current.value().player.player_id, request.now_ms);
    if (!status.ok()) {
        return status;
    }

    status = RevokeOtherSessions(
        database_,
        current.value().player.player_id,
        current.value().session.session_id,
        request.now_ms,
        "password_change");
    if (!status.ok()) {
        return status;
    }

    return transaction.value().Commit();
}

}  // namespace mmcr::auth
