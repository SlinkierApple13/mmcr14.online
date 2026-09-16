#pragma once

#include <cstdint>

namespace mmcr::game {

// Session-level event kinds (transition/claim taxonomy). Extracted from
// session.h so that mode controllers can reference them without pulling in
// the full ActiveSession definition.
enum class EventKind : std::uint8_t {
    kNone,
    kStart,
    kPredraw,
    kDrawTile,
    kDiscardTile,
    kChow,
    kPung,
    kMeldedKong,
    kAddedKong,
    kConcealedKong,
    kDiscardWin,
    kRobAddedKongWin,
    kSelfDrawnWin,
    kPass,
    kFinalPass,
    kDrawnGame,
    kEnd,
    kPlayerLeft,
    kPlayerResumed,
};

}  // namespace mmcr::game
