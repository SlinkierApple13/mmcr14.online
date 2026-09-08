#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jsoncpp/json/json.h>

#include "game/engine/session.h"

namespace mmcr::random {
class SeedContainer;
}

namespace mmcr::game {

// ---------------------------------------------------------------------------
// Clocks and randomness
// ---------------------------------------------------------------------------

[[nodiscard]] std::uint64_t now_ns();
[[nodiscard]] std::int64_t now_ms();
[[nodiscard]] std::vector<mahjong::tile_t> DebugInitialTiles(random::SeedContainer* seeder);

// ---------------------------------------------------------------------------
// Inbound message parsing
// ---------------------------------------------------------------------------

[[nodiscard]] const Json::Value* FindPayload(const Json::Value& message);
[[nodiscard]] std::optional<EventKind> ParseEventKind(std::string_view value);
[[nodiscard]] std::optional<bool> ReadOptionalBool(const Json::Value& object,
                                                   std::string_view name);
[[nodiscard]] std::optional<std::uint64_t> ReadOptionalUInt64(const Json::Value& object,
                                                              std::string_view name);
[[nodiscard]] std::optional<mahjong::tile_t> ReadOptionalTile(const Json::Value& object,
                                                              std::string_view name);
[[nodiscard]] bool IsPassMarginClaim(EventKind kind);
[[nodiscard]] Json::Value BuildPassAckEnvelope(std::uint64_t stage_counter);

// ---------------------------------------------------------------------------
// Record serialization (replay/stats persistence)
// ---------------------------------------------------------------------------

[[nodiscard]] Json::Value SerializeRecordGameConfig(const GameConfig& config);
[[nodiscard]] Json::Value SerializeRecordTiles(const std::vector<mahjong::tile_t>& tiles);
[[nodiscard]] Json::Value SerializeRecordRoundStartSnapshot(const RoundStartSnapshot& snapshot);
[[nodiscard]] bool StartsStoredRoundTurn(EventKind kind);
[[nodiscard]] bool CountsAsStoredMeld(EventKind kind);
void AdvanceStoredRoundTurn(int next_actor, int* current_actor, std::int64_t* turn);
[[nodiscard]] Json::Value SerializeStringArray(const std::vector<std::string>& values);
[[nodiscard]] Json::Value SerializeFanCodeArray(const std::vector<qingque::fan_code>& fan_codes);
[[nodiscard]] Json::Value SerializeMeldCount(const std::array<int, 4>& meld_count);
[[nodiscard]] bool IsRoundResultTerminal(EventKind kind);
[[nodiscard]] const Event* FindRecordRoundResultTransition(
    const std::vector<Event>& transitions,
    std::size_t start_index);
[[nodiscard]] Json::Value SerializeRecordRoundResult(const Event& terminal,
                                                     const std::array<int, 4>& meld_count,
                                                     std::int64_t total_turn);
[[nodiscard]] Json::Value SerializeRecordEvent(
    const Event& event,
    std::optional<std::int64_t> round_total_turn = std::nullopt);
[[nodiscard]] WinData BuildWinData(const mahjong::hand& h);

// ---------------------------------------------------------------------------
// Display / viewer serialization
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<mahjong::tile_t> SortTilesForDisplay(
    std::vector<mahjong::tile_t> tiles);
[[nodiscard]] Json::Value SerializeScores(const std::array<int, 4>& scores);
[[nodiscard]] int CountRemainingForViewer(const std::array<Seat, 4>& seats,
                                          int viewer_seat,
                                          mahjong::tile_t tile);
[[nodiscard]] Json::Value SerializeViewerWaitData(const std::array<Seat, 4>& seats,
                                                  int viewer_seat);

// ---------------------------------------------------------------------------
// Outbound message serialization
// ---------------------------------------------------------------------------

[[nodiscard]] Json::Value BuildEnvelope(std::string_view type, Json::Value payload);
[[nodiscard]] std::string_view EventKindName(EventKind kind);
[[nodiscard]] std::string_view PendingStatusName(PendingStatus status);
[[nodiscard]] std::string_view MeldTypeName(mahjong::meld_type type);
[[nodiscard]] bool IsSyncCheckpoint(EventKind kind);
[[nodiscard]] bool IsVisibleTransition(EventKind kind);
[[nodiscard]] bool IsPublicClaim(EventKind kind);
[[nodiscard]] int WaitDurationMs(const GameConfig& config,
                                 PendingStatus pending,
                                 int auxiliary_ms = 0);
[[nodiscard]] Json::Value SerializeTiles(const std::vector<mahjong::tile_t>& tiles);
[[nodiscard]] Json::Value SerializeMeld(const MeldWrapper& wrapper);
[[nodiscard]] Json::Value SerializeWinData(const WinData& data);
[[nodiscard]] Json::Value SerializeAvailableActions(
    const Seat& seat,
    PendingStatus pending,
    bool include_discard,
    int relative_to_target,
    std::optional<mahjong::tile_t> reaction_tile = std::nullopt);
[[nodiscard]] Json::Value SerializeVisibleEvent(const Event& event,
                                                int viewer_seat,
                                                std::uint64_t stage_counter);

// ---------------------------------------------------------------------------
// Message delivery policies
// ---------------------------------------------------------------------------

struct MsgPolicy {
    Json::Value msg{};
    int delay_ms{0};
    PendingStatus set_pending{PendingStatus::kPendingNone};
};

[[nodiscard]] std::array<MsgPolicy, 4> MsgOnClaim(
    const Event& event,
    const std::array<Seat, 4>& seats,
    const std::array<bool, 4>& interval_delayed_seats,
    int meld_offset_ms,
    std::int64_t dispatch_now_ms);

[[nodiscard]] std::array<MsgPolicy, 4> MsgOnTransition(
    const Event& transition,
    const std::array<Seat, 4>& seats,
    const std::array<bool, 4>& interval_delayed_seats,
    const std::array<bool, 4>& next_interval_delayed_seats,
    const std::optional<Event>& previous_transition,
    int current_meld_offset_ms,
    int next_meld_offset_ms,
    std::int64_t last_claim_event_ms,
    std::int64_t dispatch_now_ms,
    int random_pause_ms);

}  // namespace mmcr::game
