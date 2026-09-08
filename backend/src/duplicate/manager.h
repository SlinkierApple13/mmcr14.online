#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "util/status.h"
#include "util/status_or.h"

namespace mmcr::storage {
class Database;
}

namespace mmcr::duplicate {

// Expiry choices offered by the UI (hours).
constexpr std::int64_t kAllowedExpiryHours[] = {3, 6, 12, 24, 72, 168};

// Full state of one duplicate seed list.
struct SeedListInfo {
    std::int64_t id{0};
    std::string creation_token;
    std::string master_token;
    std::uint32_t round_count{0};
    std::vector<std::uint64_t> seeds;
    std::int64_t created_at_ms{0};
    std::int64_t expires_at_ms{0};
    // Sessions created from this list (starts pending or active).
    std::int64_t next_session_number{0};
    // Sessions still alive (pending or active).
    std::int64_t live_session_count{0};
    // Sessions that have actually started (active only).
    std::int64_t started_session_count{0};
    [[nodiscard]] bool expired(std::int64_t now_ms) const {
        return expires_at_ms <= now_ms;
    }
};

struct SeedListCreateResult {
    std::string creation_token;
    std::string master_token;
    std::int64_t expires_at_ms{0};
};

// Returns the first five characters of the creation token as lowercase hex —
// the "xxxxx" prefix shown in replay lists for duplicate sessions.
[[nodiscard]] std::string TokenDisplayPrefix(std::string_view creation_token);

// Persistent manager for duplicate-game seed lists. Rows live in a table of
// the shared backend SQLite database. Expired lists are deleted once their
// last live session completes.
class DuplicateManager {
public:
    explicit DuplicateManager(storage::Database* database);

    [[nodiscard]] util::Status InitializeSchema(const std::filesystem::path& migrations_dir);

    // Startup maintenance: removes seed lists whose expiry time has passed
    // (no sessions survive a restart, so all of them are safe to delete).
    [[nodiscard]] util::Status OnServerStartup(std::int64_t now_ms);

    // Generates a seed list with 16 * round_count random seeds and two fresh
    // random tokens. Returns the tokens for the requesting user.
    [[nodiscard]] util::StatusOr<SeedListCreateResult> CreateSeedList(
        std::uint32_t round_count,
        std::int64_t expiry_hours,
        std::int64_t now_ms);

    // Validates a creation token for a new session and atomically allocates
    // the session number (and bumps the live-session count).
    [[nodiscard]] util::StatusOr<SeedListInfo> StartSession(
        std::string_view creation_token,
        std::int64_t now_ms);

    // Increments the started-session counter when a pending duplicate table
    // actually begins play.
    [[nodiscard]] util::Status MarkSessionStarted(std::string_view creation_token);

    // Marks one session of the seed list as completed. `session_started` is
    // true when the session actually began play (decrements the started
    // counter); pending sessions that never started pass false. Removes the
    // row once it is expired and no live sessions remain.
    [[nodiscard]] util::Status EndSession(std::string_view creation_token,
                                          std::int64_t now_ms,
                                          bool session_started);

    // Query by either key (creation or master). Returns the info including
    // the expiry time, even when expired (until the row is removed).
    [[nodiscard]] util::StatusOr<SeedListInfo> Query(std::string_view token,
                                                     std::int64_t now_ms) const;

    // Master-key operations. Both reject unknown keys and already-expired
    // (or already-removed) seed lists.
    [[nodiscard]] util::Status ExtendExpiry(std::string_view master_token,
                                            std::int64_t expiry_hours,
                                            std::int64_t now_ms);
    [[nodiscard]] util::Status ForceExpire(std::string_view master_token,
                                           std::int64_t now_ms);

    // True while the token exists and has not expired — duplicate sessions
    // stay invisible to stats/replays during this window.
    [[nodiscard]] util::StatusOr<bool> IsTokenActive(std::string_view creation_token,
                                                     std::int64_t now_ms) const;

    // Records released into the stats module once a seed list is destroyed.
    // The callback receives the serialized round-record payload JSON.
    using ReleaseCallback = std::function<void(std::string payload_json)>;
    void SetReleaseCallback(ReleaseCallback callback);

    // Stages one round record under the seed list. Returns false when the
    // seed list has already been destroyed — the caller should then write
    // the record straight into the stats module.
    [[nodiscard]] util::StatusOr<bool> StageRecord(
        std::string_view creation_token,
        std::string_view session_identifier,
        std::int64_t round_number,
        std::string payload_json,
        std::int64_t now_ms);

    // Collects and erases the staged records of a seed list (and optionally
    // the seed-list row itself) in one transaction, then invokes the release
    // callback for each record after the commit.
    [[nodiscard]] util::Status ReleaseSeedList(std::string_view creation_token,
                                               bool erase_seed_row);

private:
    storage::Database* database_;
    ReleaseCallback release_callback_;
};

}  // namespace mmcr::duplicate
