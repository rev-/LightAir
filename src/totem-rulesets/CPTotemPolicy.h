#pragma once
#include <stdint.h>

// ================================================================
// CPTotemPolicy — shared constants for the generalised CP totem.
//
// Included by both the totem runner (CPTotem.cpp) and any player-
// side game ruleset that builds activation payloads or interprets
// CP beacons.
//
// Mode word (uint32_t)
// ────────────────────
// Build by OR-ing one value from each group plus any feature flags:
//
//   uint32_t mode = CPPolicy::ASSOCIATION_PLAYER
//                 | CPPolicy::CONTEST_RESET
//                 | CPPolicy::POSTSCORE_INACTIVE
//                 | CPPolicy::TIMER_SIMPLE;
//
// Read back using the CPTotem accessor helpers (assocScope(),
// timerMode(), …) which return the extracted sub-field as a uint8_t;
// compare against the CPPolicy::Scope / Timer / … sub-namespaces:
//
//   if (timerMode() == CPPolicy::Timer::ACCUMULATE) { … }
//
// Activation payload layout (0xF1 reply, after payload[0]=roleId)
// ───────────────────────────────────────────────────────────────
//   [1..4]  mode uint32_t little-endian
//   [5..6]  countdown_secs uint16_t LE  (0 → 10 s default)
//   [7..8]  initial context uint16_t LE
//   [9]     cooldown_secs               (0 → 30 s default)
//   [10]    suspend_secs                (0 → indefinite)
//   [11]    prime_secs                  (0 → 5 s default)
//
// Beacon payload layout (MSG_CP_BEACON, 0x52)
// ────────────────────────────────────────────
//   [0]     CPState
//   [1]     associated playerID   (0 = none)
//   [2]     associated team       (0xFF = none)
//   [3]     associated role       (0 = none)
//   [4]     context low byte
//   [5]     context high byte
//   [6]     countdown remaining seconds (0xFF = not running)
//
// Player reply payload layout (MSG_CP_BEACON+1, 0x53)
// ────────────────────────────────────────────────────
//   [0]     CPAction
//   [1..2]  new context uint16_t LE  (CPAction::CONTEXT only)
//   team and role are read from RadioPacket header fields, not payload.
//
// Score payload layout (MSG_CP_SCORE, 0x54)
// ──────────────────────────────────────────
//   [0]     associated playerID
//   [1]     associated team
//   [2]     associated role
// ================================================================

// ----------------------------------------------------------------
// CPState — totem state, broadcast in beacon payload[0].
// ----------------------------------------------------------------
enum class CPState : uint8_t {
    IDLE       = 0,  // no association; ready
    PRIMED     = 1,  // entity warming up; main countdown not yet started
    ASSOCIATED = 2,  // countdown running; one entity is associated
    CONTESTED  = 3,  // competing entities present; timer may be paused
    SUSPENDED  = 4,  // player-triggered freeze; all timers paused
    COOLDOWN   = 5,  // post-score lockout; returns to IDLE on expiry
    INACTIVE   = 6,  // terminal until game reset (or REACTIVATE if enabled)
};

// ----------------------------------------------------------------
// CPAction — action type in player reply payload[0].
// ----------------------------------------------------------------
enum class CPAction : uint8_t {
    PRESENCE   = 0,  // "I am here" — standard keepalive / association bid
    SUSPEND    = 1,  // request SUSPENDED state (requires FLAG_SUSPEND_ENABLED)
    RESUME     = 2,  // request exit from SUSPENDED → IDLE
    REACTIVATE = 3,  // request INACTIVE → IDLE (requires FLAG_REACTIVATABLE)
    CONTEXT    = 4,  // update context; payload[1..2] = new uint16_t value
};

// ----------------------------------------------------------------
// CPPolicy — mode-word constants.
// ----------------------------------------------------------------
namespace CPPolicy {

// ── Bits 0–1: Association scope ──────────────────────────────────
// Which field of the radio packet identifies the "entity" tracked.
constexpr uint32_t ASSOCIATION_TEAM   = 0u;  // track by RadioPacket.team
constexpr uint32_t ASSOCIATION_PLAYER = 1u;  // track by RadioPacket.senderId
constexpr uint32_t ASSOCIATION_ROLE   = 2u;  // track by RadioPacket.role
constexpr uint32_t ASSOCIATION_ANY    = 3u;  // first arrival, no filter

// ── Bits 2–3: New-entity behavior ────────────────────────────────
// What happens when a *different* entity sends PRESENCE while
// the totem is in PRIMED or ASSOCIATED.
constexpr uint32_t CONTEST_HOLD            = (0u << 2);  // ignore new entity
constexpr uint32_t CONTEST_RESET           = (1u << 2);  // new entity takes over; timer restarts
constexpr uint32_t CONTEST_PAUSE           = (2u << 2);  // enter CONTESTED; preserve elapsed time on return
constexpr uint32_t CONTEST_PAUSE_THEN_RESET= (3u << 2);  // enter CONTESTED; reset timer on return

// ── Bits 4–5: Post-score behavior ────────────────────────────────
constexpr uint32_t POSTSCORE_LOOP     = (0u << 4);  // clear assoc and return to IDLE
constexpr uint32_t POSTSCORE_COOLDOWN = (1u << 4);  // enter COOLDOWN, then IDLE
constexpr uint32_t POSTSCORE_INACTIVE = (2u << 4);  // terminal until game reset
constexpr uint32_t POSTSCORE_REPEAT   = (3u << 4);  // score repeatedly while held (classic CP)

// ── Bits 6–7: Contest resolution ─────────────────────────────────
// How CONTESTED resolves when presence changes.
constexpr uint32_t RESOLUTION_PREV_WINS     = (0u << 6);  // original holder reclaims; others neutralise
constexpr uint32_t RESOLUTION_LAST_STANDING = (1u << 6);  // whoever remains when others leave
constexpr uint32_t RESOLUTION_NEUTRALIZE    = (2u << 6);  // always go to IDLE when contest resolves
constexpr uint32_t RESOLUTION_FIRST_REPLY   = (3u << 6);  // first PRESENCE in window wins

// ── Bits 8–9: Timer mode ─────────────────────────────────────────
constexpr uint32_t TIMER_SIMPLE     = (0u << 8);  // straight wall-clock countdown
constexpr uint32_t TIMER_ACCUMULATE = (1u << 8);  // timer pauses when entity not present
constexpr uint32_t TIMER_IMMEDIATE  = (2u << 8);  // score instantly on association (no wait)
constexpr uint32_t TIMER_PRIMED     = (3u << 8);  // PRIMED phase required before countdown

// ── Bits 10–16: Feature flags ─────────────────────────────────────
constexpr uint32_t FLAG_TIMER_PAUSE_IN_CONTEST = (1u << 10);  // pause countdown while CONTESTED
constexpr uint32_t FLAG_LOCK_ON_PRIME          = (1u << 11);  // ignore new entities once PRIMED
constexpr uint32_t FLAG_LOCK_ON_COUNT          = (1u << 12);  // ignore new entities once counting
constexpr uint32_t FLAG_ROLE_GATED             = (1u << 13);  // only non-zero role may initiate
constexpr uint32_t FLAG_SUSPEND_ENABLED        = (1u << 14);  // players may trigger SUSPENDED
constexpr uint32_t FLAG_CONTEST_TIMER          = (1u << 15);  // CONTESTED times out after _countdownMs → IDLE
constexpr uint32_t FLAG_REACTIVATABLE          = (1u << 16);  // REACTIVATE action reverts INACTIVE → IDLE

// ── Bits 17–31: Reserved ──────────────────────────────────────────

// ── Extracted-value sub-namespaces ───────────────────────────────
// Return values of CPTotem's mode accessor helpers (assocScope(),
// contestBeh(), postScore(), resolution(), timerMode()).
namespace Scope {
    constexpr uint8_t TEAM   = 0;
    constexpr uint8_t PLAYER = 1;
    constexpr uint8_t ROLE   = 2;
    constexpr uint8_t ANY    = 3;
}
namespace Contest {
    constexpr uint8_t HOLD            = 0;
    constexpr uint8_t RESET           = 1;
    constexpr uint8_t PAUSE           = 2;
    constexpr uint8_t PAUSE_THEN_RESET = 3;
}
namespace PostScore {
    constexpr uint8_t LOOP     = 0;
    constexpr uint8_t COOLDOWN = 1;
    constexpr uint8_t INACTIVE = 2;
    constexpr uint8_t REPEAT   = 3;
}
namespace Resolution {
    constexpr uint8_t PREV_WINS      = 0;
    constexpr uint8_t LAST_STANDING  = 1;
    constexpr uint8_t NEUTRALIZE     = 2;
    constexpr uint8_t FIRST_REPLY    = 3;
}
namespace Timer {
    constexpr uint8_t SIMPLE     = 0;
    constexpr uint8_t ACCUMULATE = 1;
    constexpr uint8_t IMMEDIATE  = 2;
    constexpr uint8_t PRIMED     = 3;
}

} // namespace CPPolicy
