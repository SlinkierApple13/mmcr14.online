#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <jsoncpp/json/json.h>

#include "game/config.h"
#include "game/engine/event_kind.h"

namespace mmcr::random {
class SeedContainer;
}

namespace mmcr::game {

// Decision returned by round-settlement hooks.
enum class TransitionDecision : std::uint8_t {
    kContinue,
    kEnd,
};

// Session-level play mode. Modes only alter session flow (round start,
// settlement, transitions, session end) and serialization; the fan library
// (qingque) is untouched.
class ModeController {
public:
    virtual ~ModeController() = default;

    [[nodiscard]] virtual std::string_view mode_name() const = 0;

    // Session initialized (inside ActiveSession::init, before the first kStart
    // transition). A mode may consume exactly one seed from the container to
    // generate reproducible per-session data (e.g. pass-five-gates targets).
    virtual void OnSessionStart(random::SeedContainer* /*seed_container*/) {}

    // A round has started (kStart already reset seats and incremented
    // round_counter). SetSeatPlayers has already been called with the
    // seat -> player mapping for this round.
    virtual void OnRoundStart(std::uint64_t /*round_counter*/) {}

    // Injects the current seat -> player mapping (player_ids in seat order)
    // after each round start, so modes can resolve team membership by player
    // id even though settlement hooks only receive seat indices.
    virtual void SetSeatPlayers(const std::array<std::int64_t, 4>& /*player_ids*/) {}

    // A round settled with a win (scores already updated for all seats).
    // raw_fans is the union of all fan indices across every winning
    // decomposition ("以原始的为准"). scores carries the post-settlement
    // scores in seat order for victory-condition checks. Return kEnd to
    // terminate the session immediately.
    virtual auto OnRoundSettled(int /*seat*/,
                                const std::vector<int>& /*raw_fans*/,
                                const std::array<int, 4>& /*scores*/) -> TransitionDecision {
        return TransitionDecision::kContinue;
    }

    // Choose the transition that ends a round. Default: standard behaviour
    // (kEnd once round_count is reached; kStart otherwise; 0 = unlimited).
    virtual auto NextTransition(std::uint64_t round_counter, int round_count) -> EventKind {
        return (round_count > 0 && round_counter >= static_cast<std::uint64_t>(round_count))
            ? EventKind::kEnd
            : EventKind::kStart;
    }

    // Session is ending. final_scores carries the end-of-session scores in
    // seat order. Returns a JSON value to attach to the end transition
    // (e.g. winner team / draw / completion) for replays; may return null
    // when the mode has nothing to attach.
    virtual auto OnSessionEnd(const std::array<int, 4>& /*final_scores*/) -> Json::Value {
        return Json::Value(Json::nullValue);
    }

    // Append mode state into the session snapshot payload (field "mode_state").
    virtual void SerializeState(Json::Value& /*payload*/) const {}

    // Append a mode update into a game event payload (field "mode_update").
    virtual void SerializeEvent(Json::Value& /*event_payload*/) const {}
};

// Creates the controller matching config.mode. Never returns nullptr.
[[nodiscard]] std::unique_ptr<ModeController> CreateModeController(const GameConfig& config);

}  // namespace mmcr::game
