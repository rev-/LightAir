#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// WinnerVar — one variable that participates in winner election.
//
// A game defines a winnerVars[] table in its descriptor to specify
// which runtime variables determine the winner and how they compare.
// GameRunner reads this table at scoringState entry to:
//   1. Collect each player's values into the broadcast payload.
//   2. Compare collected slots to elect the winner.
//
// Priority is determined by position in the array:
//   index 0 = primary criterion
//   index 1 = first tie-breaker (used only when index 0 is equal)
//   index 2 = second tie-breaker, etc.
//
// Example (Free for All):
//
//   static const WinnerVar winnerVars[] = {
//       { &points,     WinnerDir::MAX },  // most points wins
//       { &shoneTimes, WinnerDir::MIN },  // tie-break: fewest times shone
//   };
// ----------------------------------------------------------------

enum class WinnerDir : uint8_t {
    MAX,  // higher value wins (e.g. points scored)
    MIN,  // lower value wins  (e.g. times eliminated — fewer is better)
};

struct WinnerVar {
    int*      value;  // pointer to the game-namespace runtime variable to sample
    WinnerDir dir;    // comparison direction
};
