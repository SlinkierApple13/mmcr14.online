#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <jsoncpp/json/json.h>

#include "game/engine/session_internal.h"
#include "game/hub/hub.h"

namespace mmcr::game {

// -------------------------------------------------------------------------
// Message parsing helpers (BuildEnvelope/FindPayload come from
// game/engine/session_internal.h)
// -------------------------------------------------------------------------

[[nodiscard]] std::optional<std::string> FindMessageType(const Json::Value& message);
[[nodiscard]] util::StatusOr<std::int64_t> ReadRequiredInt64(
    const Json::Value& object, std::string_view name);
[[nodiscard]] util::StatusOr<std::optional<std::int64_t>> ReadOptionalInt64(
    const Json::Value& object, std::string_view name);
[[nodiscard]] util::StatusOr<bool> ReadRequiredBool(
    const Json::Value& object, std::string_view name);

// ---------------------------------------------------------------------------
// Known-player registry
// ---------------------------------------------------------------------------

[[nodiscard]] std::shared_ptr<auth::PlayerProfile> UpsertKnownPlayer(
    std::unordered_map<std::int64_t, std::shared_ptr<auth::PlayerProfile>>& known_players,
    const auth::PlayerProfile& player);

// ---------------------------------------------------------------------------
// Snapshot serialization and broadcast
// ---------------------------------------------------------------------------

[[nodiscard]] Json::Value BuildResumeRequiredEnvelope(std::int64_t session_id);
[[nodiscard]] PendingSessionSummary BuildPendingSummary(const PendingSession& session);
[[nodiscard]] Json::Value SerializePendingSummary(const PendingSessionSummary& summary);
[[nodiscard]] Json::Value SerializePendingSummaryList(
    const std::vector<PendingSessionSummary>& sessions);
[[nodiscard]] Json::Value SerializeActiveSummaryList(
    const std::vector<ActiveSessionSummary>& sessions);
[[nodiscard]] Json::Value SerializePendingSeat(const PendingSeat& seat);
[[nodiscard]] PendingSessionSnapshot BuildPendingSnapshot(const PendingSession& session);
[[nodiscard]] Json::Value SerializePendingSnapshot(const PendingSessionSnapshot& snapshot);
void BroadcastPendingSnapshot(GameHub& hub, const PendingSession& session);
[[nodiscard]] ActiveSessionSummary BuildActiveSummary(const ActiveSession& session);

}  // namespace mmcr::game
