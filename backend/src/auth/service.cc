#include "auth/service.h"

#include <argon2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>
#include <vector>

#include "storage/migration.h"

namespace mmcr::auth {

namespace {

constexpr std::int64_t kMissingTimestampMs = -1;

struct TokenParts {
    std::string id;
    std::string secret;

    [[nodiscard]] auto combined() const -> std::string {
        return id + "." + secret;
    }
};

struct PlayerRecord {
    PlayerProfile profile;
    std::string password_hash;
};

struct SessionRecord {
    PlayerProfile profile;
    std::string session_id;
    std::string secret_hash;
    std::int64_t created_at_ms{0};
    std::int64_t expires_at_ms{0};
    std::int64_t revoked_at_ms{kMissingTimestampMs};
};

class Argon2Hasher {
public:
    Argon2Hasher(Argon2Config config, random::SeedContainer* seed_container)
        : config_(config), seed_container_(seed_container) {}

    [[nodiscard]] auto Hash(std::string_view value) const -> util::StatusOr<std::string> {
        auto salt = RandomBytes(config_.salt_bytes);
        if (!salt.ok()) {
            return salt.status();
        }

        std::array<char, 512> encoded{};
        const int error = argon2id_hash_encoded(
            config_.time_cost,
            config_.memory_cost_kib,
            config_.parallelism,
            value.data(),
            value.size(),
            salt.value().data(),
            salt.value().size(),
            config_.hash_bytes,
            encoded.data(),
            encoded.size());
        if (error != ARGON2_OK) {
            return util::Status::Internal(argon2_error_message(error));
        }

        return std::string(encoded.data());
    }

    [[nodiscard]] auto Verify(std::string_view encoded_hash, std::string_view value) const
        -> util::Status {
        const int error = argon2id_verify(encoded_hash.data(), value.data(), value.size());
        if (error == ARGON2_OK) {
            return util::Status::Ok();
        }
        if (error == ARGON2_VERIFY_MISMATCH) {
            return util::Status::InvalidArgument("secret mismatch");
        }

        return util::Status::Internal(argon2_error_message(error));
    }

private:
    auto RandomBytes(std::size_t count) const -> util::StatusOr<std::vector<unsigned char>> {
        return random::DrawBytes(*seed_container_, count);
    }

    Argon2Config config_;
    random::SeedContainer* seed_container_;
};

auto Normalize(std::string_view value) -> std::string {
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

auto ObserveTraffic(random::SeedContainer* seed_container) -> void {
    if (seed_container != nullptr) {
        seed_container->RecordTraffic();
    }
}

auto MakeOpaqueToken(random::SeedContainer* seed_container,
                     std::size_t id_bytes,
                     std::size_t secret_bytes)
    -> util::StatusOr<TokenParts> {
    auto id = random::DrawHex(*seed_container, id_bytes);
    if (!id.ok()) {
        return id.status();
    }

    auto secret = random::DrawHex(*seed_container, secret_bytes);
    if (!secret.ok()) {
        return secret.status();
    }

    return TokenParts{std::move(id.value()), std::move(secret.value())};
}

auto ParseOpaqueToken(std::string_view token) -> util::StatusOr<TokenParts> {
    const auto separator = token.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= token.size()) {
        return util::Status::InvalidArgument("token must use id.secret format");
    }

    return TokenParts{
        std::string(token.substr(0, separator)),
        std::string(token.substr(separator + 1)),
    };
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

auto ValidateRegistration(const RegisterRequest& request) -> util::Status {
    if (request.username.empty()) {
        return util::Status::InvalidArgument("username must not be empty");
    }
    if (request.username.size() > 30) {
        return util::Status::InvalidArgument("username must be at most 30 characters");
    }
    if (request.password.empty()) {
        return util::Status::InvalidArgument("password must not be empty");
    }
    if (request.password.size() > 30) {
        return util::Status::InvalidArgument("password must be at most 30 characters");
    }
    auto is_alphanumeric = [](std::string_view s) {
        for (const unsigned char c : s) {
            if (!std::isalnum(c)) {
                return false;
            }
        }
        return true;
    };
    if (!is_alphanumeric(request.password)) {
        return util::Status::InvalidArgument("password must be alphanumeric");
    }

    return util::Status::Ok();
}

auto ValidatePasswordUpdate(std::string_view password) -> util::Status {
    if (password.empty()) {
        return util::Status::InvalidArgument("password must not be empty");
    }

    return util::Status::Ok();
}

auto UsernameExists(storage::Database* database, std::string_view username_normalized)
    -> util::StatusOr<bool> {
    auto statement = database->Prepare(
        "SELECT 1 FROM players WHERE username_normalized = ?1 LIMIT 1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, username_normalized);
    if (!status.ok()) {
        return status;
    }

    auto step = stmt.Step();
    if (!step.ok()) {
        return step.status();
    }

    return step.value() == storage::Statement::StepResult::kRow;
}

auto LoadPlayerForIdentity(storage::Database* database, std::string_view identity_normalized)
    -> util::StatusOr<PlayerRecord> {
    auto statement = database->Prepare(
        "SELECT p.id, p.username, c.password_hash "
        "FROM players p "
        "JOIN credentials c ON c.player_id = p.id "
        "WHERE p.username_normalized = ?1 "
        "LIMIT 1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, identity_normalized);
    if (!status.ok()) {
        return status;
    }

    auto step = stmt.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() == storage::Statement::StepResult::kDone) {
        return util::Status::NotFound("player not found");
    }

    return PlayerRecord{
        PlayerProfile{
            stmt.ColumnInt64(0),
            stmt.ColumnText(1),
        },
        stmt.ColumnText(2),
    };
}

auto LoadPasswordHash(storage::Database* database, std::int64_t player_id)
    -> util::StatusOr<std::string> {
    auto statement = database->Prepare(
        "SELECT password_hash FROM credentials WHERE player_id = ?1 LIMIT 1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) {
        return status;
    }

    auto step = stmt.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() == storage::Statement::StepResult::kDone) {
        return util::Status::NotFound("credential not found");
    }

    return stmt.ColumnText(0);
}

auto LoadSession(storage::Database* database, std::string_view session_id)
    -> util::StatusOr<SessionRecord> {
    auto statement = database->Prepare(
        "SELECT p.id, p.username, s.id, s.secret_hash, s.created_at_ms, s.expires_at_ms, COALESCE(s.revoked_at_ms, -1) "
        "FROM sessions s "
        "JOIN players p ON p.id = s.player_id "
        "WHERE s.id = ?1 LIMIT 1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, session_id);
    if (!status.ok()) {
        return status;
    }

    auto step = stmt.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() == storage::Statement::StepResult::kDone) {
        return util::Status::NotFound("session not found");
    }

    return SessionRecord{
        PlayerProfile{
            stmt.ColumnInt64(0),
            stmt.ColumnText(1),
        },
        stmt.ColumnText(2),
        stmt.ColumnText(3),
        stmt.ColumnInt64(4),
        stmt.ColumnInt64(5),
        stmt.ColumnInt64(6),
    };
}

auto InsertPlayer(storage::Database* database,
                  std::string_view username,
                  std::string_view username_normalized,
                  std::int64_t now_ms) -> util::StatusOr<std::int64_t> {
    auto statement = database->Prepare(
        "INSERT INTO players("
        "username, username_normalized, created_at_ms, updated_at_ms"
        ") VALUES (?1, ?2, ?3, ?3);");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, username);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(2, username_normalized);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, now_ms);
    if (!status.ok()) {
        return status;
    }

    status = StepDone(stmt);
    if (!status.ok()) {
        return status;
    }

    return database->LastInsertRowId();
}

auto InsertCredential(storage::Database* database,
                      std::int64_t player_id,
                      std::string_view password_hash,
                      std::int64_t now_ms) -> util::Status {
    auto statement = database->Prepare(
        "INSERT INTO credentials(player_id, password_hash, password_changed_at_ms) VALUES (?1, ?2, ?3);");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(2, password_hash);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, now_ms);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto InsertSession(storage::Database* database,
                   std::string_view session_id,
                   std::int64_t player_id,
                   std::string_view secret_hash,
                   std::int64_t now_ms,
                   std::int64_t expires_at_ms) -> util::Status {
    auto statement = database->Prepare(
        "INSERT INTO sessions("
        "id, player_id, secret_hash, created_at_ms, expires_at_ms, last_refreshed_at_ms, revoked_at_ms, revoked_reason"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?4, NULL, NULL);");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, session_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(2, player_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(3, secret_hash);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(4, now_ms);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(5, expires_at_ms);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto RotateSession(storage::Database* database,
                   std::string_view session_id,
                   std::string_view secret_hash,
                   std::int64_t now_ms,
                   std::int64_t expires_at_ms) -> util::Status {
    auto statement = database->Prepare(
        "UPDATE sessions SET secret_hash = ?2, expires_at_ms = ?3, last_refreshed_at_ms = ?4 "
        "WHERE id = ?1 AND revoked_at_ms IS NULL;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, session_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(2, secret_hash);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, expires_at_ms);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(4, now_ms);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto RevokeSession(storage::Database* database,
                   std::string_view session_id,
                   std::int64_t now_ms,
                   std::string_view reason) -> util::Status {
    auto statement = database->Prepare(
        "UPDATE sessions SET revoked_at_ms = ?2, revoked_reason = ?3 "
        "WHERE id = ?1 AND revoked_at_ms IS NULL;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindText(1, session_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(2, now_ms);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(3, reason);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto RevokeOtherSessions(storage::Database* database,
                         std::int64_t player_id,
                         std::string_view keep_session_id,
                         std::int64_t now_ms,
                         std::string_view reason) -> util::Status {
    auto statement = database->Prepare(
        "UPDATE sessions SET revoked_at_ms = ?3, revoked_reason = ?4 "
        "WHERE player_id = ?1 AND id != ?2 AND revoked_at_ms IS NULL;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(2, keep_session_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, now_ms);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(4, reason);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto UpdatePasswordHash(storage::Database* database,
                        std::int64_t player_id,
                        std::string_view password_hash,
                        std::int64_t now_ms) -> util::Status {
    auto statement = database->Prepare(
        "UPDATE credentials SET password_hash = ?2, password_changed_at_ms = ?3 WHERE player_id = ?1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindText(2, password_hash);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(3, now_ms);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto TouchPlayer(storage::Database* database,
                 std::int64_t player_id,
                 std::int64_t now_ms) -> util::Status {
    auto statement = database->Prepare(
        "UPDATE players SET updated_at_ms = ?2 WHERE id = ?1;");
    if (!statement.ok()) {
        return statement.status();
    }

    auto& stmt = statement.value();
    auto status = stmt.BindInt64(1, player_id);
    if (!status.ok()) {
        return status;
    }
    status = stmt.BindInt64(2, now_ms);
    if (!status.ok()) {
        return status;
    }

    return StepDone(stmt);
}

auto BuildSession(const SessionRecord& record,
                  std::string token) -> AuthenticatedSession {
    return AuthenticatedSession{
        record.profile,
        SessionInfo{record.session_id, std::move(token), record.created_at_ms, record.expires_at_ms},
    };
}

}  // namespace

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