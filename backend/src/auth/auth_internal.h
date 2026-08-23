#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "auth/service.h"
#include "random/seed.h"
#include "storage/database.h"
#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::auth {

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

// ---------------------------------------------------------------------------
// Password hashing and opaque tokens
// ---------------------------------------------------------------------------

class Argon2Hasher {
public:
    Argon2Hasher(Argon2Config config, random::SeedContainer* seed_container);

    [[nodiscard]] util::StatusOr<std::string> Hash(std::string_view value) const;
    [[nodiscard]] util::Status Verify(std::string_view encoded_hash,
                                      std::string_view value) const;

private:
    [[nodiscard]] util::StatusOr<std::vector<unsigned char>> RandomBytes(
        std::size_t count) const;

    Argon2Config config_;
    random::SeedContainer* seed_container_;
};

[[nodiscard]] std::string Normalize(std::string_view value);
void ObserveTraffic(random::SeedContainer* seed_container);
[[nodiscard]] util::StatusOr<TokenParts> MakeOpaqueToken(
    random::SeedContainer* seed_container,
    std::size_t id_bytes,
    std::size_t secret_bytes);
[[nodiscard]] util::StatusOr<TokenParts> ParseOpaqueToken(std::string_view token);

[[nodiscard]] util::Status ValidateRegistration(const RegisterRequest& request);
[[nodiscard]] util::Status ValidatePasswordUpdate(std::string_view password);

// ---------------------------------------------------------------------------
// Database storage operations
// ---------------------------------------------------------------------------

[[nodiscard]] util::Status StepDone(storage::Statement& statement);

[[nodiscard]] util::StatusOr<bool> UsernameExists(
    storage::Database* database, std::string_view username_normalized);
[[nodiscard]] util::StatusOr<PlayerRecord> LoadPlayerForIdentity(
    storage::Database* database, std::string_view identity_normalized);
[[nodiscard]] util::StatusOr<std::string> LoadPasswordHash(
    storage::Database* database, std::int64_t player_id);
[[nodiscard]] util::StatusOr<SessionRecord> LoadSession(
    storage::Database* database, std::string_view session_id);

[[nodiscard]] util::StatusOr<std::int64_t> InsertPlayer(
    storage::Database* database,
    std::string_view username,
    std::string_view username_normalized,
    std::int64_t now_ms);
[[nodiscard]] util::Status InsertCredential(
    storage::Database* database,
    std::int64_t player_id,
    std::string_view password_hash,
    std::int64_t now_ms);
[[nodiscard]] util::Status InsertSession(
    storage::Database* database,
    std::string_view session_id,
    std::int64_t player_id,
    std::string_view secret_hash,
    std::int64_t now_ms,
    std::int64_t expires_at_ms);

[[nodiscard]] util::Status RotateSession(
    storage::Database* database,
    std::string_view session_id,
    std::string_view secret_hash,
    std::int64_t now_ms,
    std::int64_t expires_at_ms);
[[nodiscard]] util::Status RevokeSession(
    storage::Database* database,
    std::string_view session_id,
    std::int64_t now_ms,
    std::string_view reason);
[[nodiscard]] util::Status RevokeOtherSessions(
    storage::Database* database,
    std::int64_t player_id,
    std::string_view keep_session_id,
    std::int64_t now_ms,
    std::string_view reason);
[[nodiscard]] util::Status UpdatePasswordHash(
    storage::Database* database,
    std::int64_t player_id,
    std::string_view password_hash,
    std::int64_t now_ms);
[[nodiscard]] util::Status TouchPlayer(
    storage::Database* database,
    std::int64_t player_id,
    std::int64_t now_ms);

[[nodiscard]] AuthenticatedSession BuildSession(const SessionRecord& record,
                                                std::string token);

}  // namespace mmcr::auth
