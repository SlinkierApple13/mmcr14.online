#include "auth/auth_internal.h"

#include <utility>

namespace mmcr::auth {

util::Status StepDone(storage::Statement& statement) {
    auto step = statement.Step();
    if (!step.ok()) {
        return step.status();
    }

    if (step.value() != storage::Statement::StepResult::kDone) {
        return util::Status::Internal("statement unexpectedly returned a row");
    }

    return util::Status::Ok();
}

util::StatusOr<bool> UsernameExists(storage::Database* database,
    std::string_view username_normalized) {
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

util::StatusOr<PlayerRecord> LoadPlayerForIdentity(storage::Database* database,
    std::string_view identity_normalized) {
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

util::StatusOr<std::string> LoadPasswordHash(storage::Database* database,
    std::int64_t player_id) {
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

util::StatusOr<SessionRecord> LoadSession(storage::Database* database,
    std::string_view session_id) {
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

util::StatusOr<std::int64_t> InsertPlayer(storage::Database* database,
                  std::string_view username,
                  std::string_view username_normalized,
                  std::int64_t now_ms) {
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

util::Status InsertCredential(storage::Database* database,
                      std::int64_t player_id,
                      std::string_view password_hash,
                      std::int64_t now_ms) {
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

util::Status InsertSession(storage::Database* database,
                   std::string_view session_id,
                   std::int64_t player_id,
                   std::string_view secret_hash,
                   std::int64_t now_ms,
                   std::int64_t expires_at_ms) {
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

util::Status RotateSession(storage::Database* database,
                   std::string_view session_id,
                   std::string_view secret_hash,
                   std::int64_t now_ms,
                   std::int64_t expires_at_ms) {
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

util::Status RevokeSession(storage::Database* database,
                   std::string_view session_id,
                   std::int64_t now_ms,
                   std::string_view reason) {
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

util::Status RevokeOtherSessions(storage::Database* database,
                         std::int64_t player_id,
                         std::string_view keep_session_id,
                         std::int64_t now_ms,
                         std::string_view reason) {
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

util::Status UpdatePasswordHash(storage::Database* database,
                        std::int64_t player_id,
                        std::string_view password_hash,
                        std::int64_t now_ms) {
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

util::Status TouchPlayer(storage::Database* database,
                 std::int64_t player_id,
                 std::int64_t now_ms) {
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

AuthenticatedSession BuildSession(const SessionRecord& record,
                  std::string token) {
    return AuthenticatedSession{
        record.profile,
        SessionInfo{record.session_id, std::move(token), record.created_at_ms, record.expires_at_ms},
    };
}

}  // namespace mmcr::auth
