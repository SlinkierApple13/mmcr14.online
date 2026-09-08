#include "duplicate/manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

#include "storage/database.h"
#include "storage/migration.h"

namespace mmcr::duplicate {
namespace {

constexpr std::uint32_t kMinRoundCount = 1;
constexpr std::uint32_t kMaxRoundCount = 32;
constexpr std::size_t kSeedsPerRound = 16;
constexpr std::int64_t kMsPerHour = 3600 * 1000;

[[nodiscard]] bool AllowedExpiryHours(std::int64_t hours) {
    for (const auto allowed : kAllowedExpiryHours) {
        if (hours == allowed) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string ToLowerHex(std::string_view value) {
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized;
}

[[nodiscard]] std::string BytesToHex(const std::array<std::uint8_t, 16>& bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(kHexDigits[(byte >> 4) & 0x0f]);
        result.push_back(kHexDigits[byte & 0x0f]);
    }
    return result;
}

[[nodiscard]] std::array<std::uint8_t, 16> RandomTokenBytes() {
    std::random_device device;
    std::mt19937_64 engine(device());
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 8) {
        const auto value = distribution(engine);
        for (std::size_t index = 0; index < 8 && offset + index < bytes.size(); ++index) {
            bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
        }
    }
    return bytes;
}

[[nodiscard]] std::string SeedsToHex(const std::vector<std::uint64_t>& seeds) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto seed : seeds) {
        stream << std::setw(16) << seed;
    }
    return stream.str();
}

[[nodiscard]] util::StatusOr<std::vector<std::uint64_t>> HexToSeeds(std::string_view hex) {
    if (hex.size() % 16 != 0) {
        return util::Status::Internal("corrupt seed list (odd hex length)");
    }
    std::vector<std::uint64_t> seeds;
    seeds.reserve(hex.size() / 16);
    for (std::size_t offset = 0; offset < hex.size(); offset += 16) {
        try {
            seeds.push_back(std::stoull(std::string(hex.substr(offset, 16)), nullptr, 16));
        } catch (...) {
            return util::Status::Internal("corrupt seed list (bad hex)");
        }
    }
    return seeds;
}

struct Row {
    std::int64_t id{0};
    std::string creation_token;
    std::string master_token;
    std::int64_t round_count{0};
    std::string seeds_hex;
    std::int64_t created_at_ms{0};
    std::int64_t expires_at_ms{0};
    std::int64_t next_session_number{0};
    std::int64_t live_session_count{0};
    std::int64_t started_session_count{0};
};

[[nodiscard]] util::Status ReadRow(storage::Statement& statement, Row& row) {
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }
    if (step.value() != storage::Statement::StepResult::kRow) {
        return util::Status::NotFound("seed list not found");
    }
    row = Row{
        .id = statement.ColumnInt64(0),
        .creation_token = statement.ColumnText(1),
        .master_token = statement.ColumnText(2),
        .round_count = statement.ColumnInt64(3),
        .seeds_hex = statement.ColumnText(4),
        .created_at_ms = statement.ColumnInt64(5),
        .expires_at_ms = statement.ColumnInt64(6),
        .next_session_number = statement.ColumnInt64(7),
        .live_session_count = statement.ColumnInt64(8),
        .started_session_count = statement.ColumnInt64(9),
    };
    return util::Status::Ok();
}

[[nodiscard]] SeedListInfo RowToInfo(const Row& row) {
    auto seeds = HexToSeeds(row.seeds_hex);
    std::vector<std::uint64_t> seed_values;
    if (seeds.ok()) {
        seed_values = std::move(seeds.value());
    }
    SeedListInfo info;
    info.id = row.id;
    info.creation_token = row.creation_token;
    info.master_token = row.master_token;
    info.round_count = static_cast<std::uint32_t>(row.round_count);
    info.seeds = std::move(seed_values);
    info.created_at_ms = row.created_at_ms;
    info.expires_at_ms = row.expires_at_ms;
    info.next_session_number = row.next_session_number;
    info.live_session_count = row.live_session_count;
    info.started_session_count = row.started_session_count;
    return info;
}

}  // namespace

std::string TokenDisplayPrefix(std::string_view creation_token) {
    const std::string normalized = ToLowerHex(creation_token);
    return normalized.substr(0, 5);
}

DuplicateManager::DuplicateManager(storage::Database* database)
    : database_(database) {}

util::Status DuplicateManager::InitializeSchema(const std::filesystem::path& migrations_dir) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }
    storage::MigrationRunner runner(database_);
    return runner.ApplyDirectory(migrations_dir);
}

util::Status DuplicateManager::OnServerStartup(std::int64_t now_ms) {
    if (database_ == nullptr || !database_->is_open()) {
        return util::Status::Internal("database is not open");
    }
    // No sessions survive a restart: clear the live counters for every list
    // before removing the expired ones.
    {
        auto reset = database_->Prepare(
            "UPDATE duplicate_seed_lists"
            " SET live_session_count = 0, started_session_count = 0;");
        if (!reset.ok()) {
            return reset.status();
        }
        auto step = reset.value().Step();
        if (!step.ok()) {
            return step.status();
        }
    }
    std::vector<std::string> expired_tokens;
    {
        auto statement = database_->Prepare(
            "SELECT creation_token FROM duplicate_seed_lists WHERE expires_at_ms <= ?1;");
        if (!statement.ok()) {
            return statement.status();
        }
        if (auto status = statement.value().BindInt64(1, now_ms); !status.ok()) {
            return status;
        }
        while (true) {
            auto step = statement.value().Step();
            if (!step.ok()) {
                return step.status();
            }
            if (step.value() != storage::Statement::StepResult::kRow) {
                break;
            }
            expired_tokens.push_back(statement.value().ColumnText(0));
        }
    }
    for (const auto& token : expired_tokens) {
        auto status = ReleaseSeedList(token, true);
        if (!status.ok()) {
            return status;
        }
    }
    return util::Status::Ok();
}

util::StatusOr<SeedListCreateResult> DuplicateManager::CreateSeedList(
    std::uint32_t round_count,
    std::int64_t expiry_hours,
    std::int64_t now_ms) {
    if (round_count < kMinRoundCount || round_count > kMaxRoundCount) {
        return util::Status::InvalidArgument("round count must be between 1 and 32");
    }
    if (!AllowedExpiryHours(expiry_hours)) {
        return util::Status::InvalidArgument("expiry hours must be one of 3, 6, 12, 24, 72, 168");
    }

    std::random_device device;
    std::mt19937_64 engine(device());
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::vector<std::uint64_t> seeds;
    seeds.reserve(static_cast<std::size_t>(round_count) * kSeedsPerRound);
    for (std::uint32_t index = 0; index < round_count * kSeedsPerRound; ++index) {
        seeds.push_back(distribution(engine));
    }

    const std::string creation_token = BytesToHex(RandomTokenBytes());
    const std::string master_token = BytesToHex(RandomTokenBytes());
    const std::int64_t expires_at_ms = now_ms + expiry_hours * kMsPerHour;

    auto statement = database_->Prepare(
        "INSERT INTO duplicate_seed_lists"
        "(creation_token, master_token, round_count, seeds_hex, created_at_ms,"
        " expires_at_ms, next_session_number, live_session_count, started_session_count)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0, 0, 0);");
    if (!statement.ok()) {
        return statement.status();
    }
    if (auto status = statement.value().BindText(1, creation_token); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindText(2, master_token); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindInt64(3, round_count); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindText(4, SeedsToHex(seeds)); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindInt64(5, now_ms); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindInt64(6, expires_at_ms); !status.ok()) {
        return status;
    }
    auto step = statement.value().Step();
    if (!step.ok()) {
        return step.status();
    }

    SeedListCreateResult result;
    result.creation_token = creation_token;
    result.master_token = master_token;
    result.expires_at_ms = expires_at_ms;
    return result;
}

util::StatusOr<SeedListInfo> DuplicateManager::StartSession(
    std::string_view creation_token,
    std::int64_t now_ms) {
    const std::string token = ToLowerHex(creation_token);

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    auto select = database_->Prepare(
        "SELECT id, creation_token, master_token, round_count, seeds_hex,"
        " created_at_ms, expires_at_ms, next_session_number, live_session_count,"
        " started_session_count"
        " FROM duplicate_seed_lists WHERE creation_token = ?1;");
    if (!select.ok()) {
        return select.status();
    }
    if (auto status = select.value().BindText(1, token); !status.ok()) {
        return status;
    }

    Row row;
    if (auto status = ReadRow(select.value(), row); !status.ok()) {
        return status;
    }
    if (row.expires_at_ms <= now_ms) {
        return util::Status::InvalidArgument("the duplicate token has expired");
    }

    const auto next_number = row.next_session_number;
    auto update = database_->Prepare(
        "UPDATE duplicate_seed_lists"
        " SET next_session_number = next_session_number + 1,"
        " live_session_count = live_session_count + 1"
        " WHERE id = ?1;");
    if (!update.ok()) {
        return update.status();
    }
    if (auto status = update.value().BindInt64(1, row.id); !status.ok()) {
        return status;
    }
    auto step = update.value().Step();
    if (!step.ok()) {
        return step.status();
    }

    if (auto status = transaction.value().Commit(); !status.ok()) {
        return status;
    }

    SeedListInfo info = RowToInfo(row);
    info.next_session_number = next_number;
    info.live_session_count = row.live_session_count + 1;
    return info;
}

util::Status DuplicateManager::EndSession(std::string_view creation_token,
                                          std::int64_t now_ms,
                                          bool session_started) {
    const std::string token = ToLowerHex(creation_token);

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    auto select = database_->Prepare(
        "SELECT id, creation_token, master_token, round_count, seeds_hex,"
        " created_at_ms, expires_at_ms, next_session_number, live_session_count,"
        " started_session_count"
        " FROM duplicate_seed_lists WHERE creation_token = ?1;");
    if (!select.ok()) {
        return select.status();
    }
    if (auto status = select.value().BindText(1, token); !status.ok()) {
        return status;
    }

    Row row;
    auto read_status = ReadRow(select.value(), row);
    if (!read_status.ok()) {
        // Row already removed (expired and fully completed) — nothing to do.
        return transaction.value().Commit();
    }

    const bool expired = row.expires_at_ms <= now_ms;
    const bool last_session = row.live_session_count <= 1;
    if (expired && last_session) {
        if (auto status = transaction.value().Commit(); !status.ok()) {
            return status;
        }
        return ReleaseSeedList(token, true);
    }

    auto update = database_->Prepare(
        "UPDATE duplicate_seed_lists"
        " SET live_session_count = live_session_count - 1,"
        " started_session_count = MAX(started_session_count - ?2, 0)"
        " WHERE id = ?1;");
    if (!update.ok()) {
        return update.status();
    }
    if (auto status = update.value().BindInt64(1, row.id); !status.ok()) {
        return status;
    }
    if (auto status = update.value().BindInt64(2, session_started ? 1 : 0); !status.ok()) {
        return status;
    }
    auto step = update.value().Step();
    if (!step.ok()) {
        return step.status();
    }

    return transaction.value().Commit();
}

util::StatusOr<SeedListInfo> DuplicateManager::Query(std::string_view token,
                                                     std::int64_t now_ms) const {
    (void)now_ms;
    const std::string normalized = ToLowerHex(token);

    auto statement = database_->Prepare(
        "SELECT id, creation_token, master_token, round_count, seeds_hex,"
        " created_at_ms, expires_at_ms, next_session_number, live_session_count,"
        " started_session_count"
        " FROM duplicate_seed_lists WHERE creation_token = ?1 OR master_token = ?1;");
    if (!statement.ok()) {
        return statement.status();
    }
    if (auto status = statement.value().BindText(1, normalized); !status.ok()) {
        return status;
    }

    Row row;
    if (auto status = ReadRow(statement.value(), row); !status.ok()) {
        return status;
    }
    return RowToInfo(row);
}

util::Status DuplicateManager::ExtendExpiry(std::string_view master_token,
                                            std::int64_t expiry_hours,
                                            std::int64_t now_ms) {
    if (!AllowedExpiryHours(expiry_hours)) {
        return util::Status::InvalidArgument("expiry hours must be one of 3, 6, 12, 24, 72, 168");
    }
    const std::string token = ToLowerHex(master_token);

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    auto select = database_->Prepare(
        "SELECT id, creation_token, master_token, round_count, seeds_hex,"
        " created_at_ms, expires_at_ms, next_session_number, live_session_count,"
        " started_session_count"
        " FROM duplicate_seed_lists WHERE master_token = ?1;");
    if (!select.ok()) {
        return select.status();
    }
    if (auto status = select.value().BindText(1, token); !status.ok()) {
        return status;
    }

    Row row;
    if (auto status = ReadRow(select.value(), row); !status.ok()) {
        return status;
    }
    if (row.expires_at_ms <= now_ms) {
        return util::Status::InvalidArgument("the seed list has already expired");
    }

    auto update = database_->Prepare(
        "UPDATE duplicate_seed_lists SET expires_at_ms = ?2 WHERE id = ?1;");
    if (!update.ok()) {
        return update.status();
    }
    if (auto status = update.value().BindInt64(1, row.id); !status.ok()) {
        return status;
    }
    if (auto status = update.value().BindInt64(2, now_ms + expiry_hours * kMsPerHour); !status.ok()) {
        return status;
    }
    auto step = update.value().Step();
    if (!step.ok()) {
        return step.status();
    }

    return transaction.value().Commit();
}

util::Status DuplicateManager::ForceExpire(std::string_view master_token,
                                           std::int64_t now_ms) {
    const std::string token = ToLowerHex(master_token);

    auto transaction = database_->BeginTransaction();
    if (!transaction.ok()) {
        return transaction.status();
    }

    auto select = database_->Prepare(
        "SELECT id, creation_token, master_token, round_count, seeds_hex,"
        " created_at_ms, expires_at_ms, next_session_number, live_session_count,"
        " started_session_count"
        " FROM duplicate_seed_lists WHERE master_token = ?1;");
    if (!select.ok()) {
        return select.status();
    }
    if (auto status = select.value().BindText(1, token); !status.ok()) {
        return status;
    }

    Row row;
    if (auto status = ReadRow(select.value(), row); !status.ok()) {
        return status;
    }
    if (row.expires_at_ms <= now_ms) {
        return util::Status::InvalidArgument("the seed list has already expired");
    }

    if (row.live_session_count <= 0) {
        if (auto status = transaction.value().Commit(); !status.ok()) {
            return status;
        }
        // ReleaseSeedList keys on the creation token.
        return ReleaseSeedList(row.creation_token, true);
    }

    auto update = database_->Prepare(
        "UPDATE duplicate_seed_lists SET expires_at_ms = ?2 WHERE id = ?1;");
    if (!update.ok()) {
        return update.status();
    }
    if (auto status = update.value().BindInt64(1, row.id); !status.ok()) {
        return status;
    }
    if (auto status = update.value().BindInt64(2, now_ms); !status.ok()) {
        return status;
    }
    auto step = update.value().Step();
    if (!step.ok()) {
        return step.status();
    }

    return transaction.value().Commit();
}

util::StatusOr<bool> DuplicateManager::IsTokenActive(std::string_view creation_token,
                                                     std::int64_t now_ms) const {
    const std::string token = ToLowerHex(creation_token);

    auto statement = database_->Prepare(
        "SELECT 1 FROM duplicate_seed_lists"
        " WHERE creation_token = ?1 AND expires_at_ms > ?2 LIMIT 1;");
    if (!statement.ok()) {
        return statement.status();
    }
    if (auto status = statement.value().BindText(1, token); !status.ok()) {
        return status;
    }
    if (auto status = statement.value().BindInt64(2, now_ms); !status.ok()) {
        return status;
    }
    auto step = statement.value().Step();
    if (!step.ok()) {
        return step.status();
    }
    return step.value() == storage::Statement::StepResult::kRow;
}

void DuplicateManager::SetReleaseCallback(ReleaseCallback callback) {
    release_callback_ = std::move(callback);
}

util::Status DuplicateManager::MarkSessionStarted(std::string_view creation_token) {
    const std::string token = ToLowerHex(creation_token);
    auto update = database_->Prepare(
        "UPDATE duplicate_seed_lists"
        " SET started_session_count = started_session_count + 1"
        " WHERE creation_token = ?1;");
    if (!update.ok()) {
        return update.status();
    }
    if (auto status = update.value().BindText(1, token); !status.ok()) {
        return status;
    }
    auto step = update.value().Step();
    if (!step.ok()) {
        return step.status();
    }
    return util::Status::Ok();
}

util::StatusOr<bool> DuplicateManager::StageRecord(
    std::string_view creation_token,
    std::string_view session_identifier,
    std::int64_t round_number,
    std::string payload_json,
    std::int64_t now_ms) {
    const std::string token = ToLowerHex(creation_token);

    auto exists = database_->Prepare(
        "SELECT 1 FROM duplicate_seed_lists WHERE creation_token = ?1 LIMIT 1;");
    if (!exists.ok()) {
        return exists.status();
    }
    if (auto status = exists.value().BindText(1, token); !status.ok()) {
        return status;
    }
    auto exists_step = exists.value().Step();
    if (!exists_step.ok()) {
        return exists_step.status();
    }
    if (exists_step.value() != storage::Statement::StepResult::kRow) {
        // Seed list already destroyed — release straight into stats.
        return false;
    }

    auto insert = database_->Prepare(
        "INSERT OR REPLACE INTO duplicate_staged_records"
        "(creation_token, session_identifier, round_number, payload_json, created_at_ms)"
        " VALUES (?1, ?2, ?3, ?4, ?5);");
    if (!insert.ok()) {
        return insert.status();
    }
    if (auto status = insert.value().BindText(1, token); !status.ok()) {
        return status;
    }
    if (auto status = insert.value().BindText(2, session_identifier); !status.ok()) {
        return status;
    }
    if (auto status = insert.value().BindInt64(3, round_number); !status.ok()) {
        return status;
    }
    if (auto status = insert.value().BindText(4, payload_json); !status.ok()) {
        return status;
    }
    if (auto status = insert.value().BindInt64(5, now_ms); !status.ok()) {
        return status;
    }
    auto step = insert.value().Step();
    if (!step.ok()) {
        return step.status();
    }
    return true;
}

util::Status DuplicateManager::ReleaseSeedList(std::string_view creation_token,
                                               bool erase_seed_row) {
    const std::string token = ToLowerHex(creation_token);

    std::vector<std::string> payloads;
    {
        auto transaction = database_->BeginTransaction();
        if (!transaction.ok()) {
            return transaction.status();
        }

        auto select = database_->Prepare(
            "SELECT payload_json FROM duplicate_staged_records"
            " WHERE creation_token = ?1 ORDER BY created_at_ms, round_number;");
        if (!select.ok()) {
            return select.status();
        }
        if (auto status = select.value().BindText(1, token); !status.ok()) {
            return status;
        }
        while (true) {
            auto step = select.value().Step();
            if (!step.ok()) {
                return step.status();
            }
            if (step.value() != storage::Statement::StepResult::kRow) {
                break;
            }
            payloads.push_back(select.value().ColumnText(0));
        }

        auto erase_staging = database_->Prepare(
            "DELETE FROM duplicate_staged_records WHERE creation_token = ?1;");
        if (!erase_staging.ok()) {
            return erase_staging.status();
        }
        if (auto status = erase_staging.value().BindText(1, token); !status.ok()) {
            return status;
        }
        auto erase_step = erase_staging.value().Step();
        if (!erase_step.ok()) {
            return erase_step.status();
        }

        if (erase_seed_row) {
            auto erase_seed = database_->Prepare(
                "DELETE FROM duplicate_seed_lists WHERE creation_token = ?1;");
            if (!erase_seed.ok()) {
                return erase_seed.status();
            }
            if (auto status = erase_seed.value().BindText(1, token); !status.ok()) {
                return status;
            }
            auto seed_step = erase_seed.value().Step();
            if (!seed_step.ok()) {
                return seed_step.status();
            }
        }

        if (auto status = transaction.value().Commit(); !status.ok()) {
            return status;
        }
    }

    if (release_callback_) {
        for (auto& payload : payloads) {
            release_callback_(std::move(payload));
        }
    }
    return util::Status::Ok();
}

}  // namespace mmcr::duplicate
