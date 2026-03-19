#pragma once
#include <stdint.h>

// ================================================================
// RadioMessages — central registry of all game and infrastructure
// radio message type bytes.
//
// Layout
// ──────
// 0x10 block  player game messages (shared by every game ruleset)
// 0x50 block  totem-mediated game messages
// 0xA0 block  infrastructure (config, roster discovery, end-game)
// 0xF0 block  totem protocol (beacon, roster)  — see TotemProtocol.h
//
// Convention: even = request, odd = reply (same across all blocks).
// typeId + sessionToken keep game sessions isolated on the wire, so
// each game reuses the same byte values rather than carving out its
// own private range.
//
// Adding a new message
// ────────────────────
// 1. Pick the next even slot in the appropriate block.
// 2. Add the constant here with a short description.
// 3. If the message is game-specific, note which ruleset(s) use it.
// 4. Never reuse a retired value; leave a comment in its place.
// ================================================================

namespace RadioMsg {

// ── 0x10 block: player game messages ───────────────────────────
// Used by every game that has direct player-to-player hits.

// Unicast hit notification sent by shooter to target.
// Reply (0x11) payload[0] = ReplySubType (TAKEN / SHONE / DOWN / FRIEND).
constexpr uint8_t MSG_LIT           = 0x10;

// End-game score broadcast, one packet per player.
constexpr uint8_t MSG_SCORE_COLLECT = 0x12;

// Periodic team-score update so teammates track aggregate points (Teams).
constexpr uint8_t MSG_POINT_REPORT  = 0x14;

// Next available in 0x10 block: 0x16

// ── 0x50 block: totem-mediated game messages ────────────────────
// Messages that travel between a player and a totem (not player→player).

// Flag state change broadcast: pickup / capture / drop (Flag game).
// payload[0] = FlagEventType; no meaningful reply expected.
constexpr uint8_t MSG_FLAG_EVENT    = 0x50;

// Control-point beacon broadcast by CP totem every 2 s (Upkeep game).
// payload[0] = cpTeam (0=O, 1=X, 0xFF=teamless).
// Reply (0x53) subType = 1 (team-O) or 2 (team-X) to declare presence.
constexpr uint8_t MSG_CP_BEACON     = 0x52;

// Control-point score award broadcast by CP totem (Upkeep game).
// payload[0] = team (0=O, 1=X) receiving the point.
constexpr uint8_t MSG_CP_SCORE      = 0x54;

// Next available in 0x50 block: 0x56

// ── 0xA0 block: infrastructure ──────────────────────────────────
// Sent with typeId == UNIVERSAL (0x0000); not game-scoped.

// Game configuration broadcast from host to all players.
constexpr uint8_t MSG_CONFIG        = 0xA0;

// Roster presence broadcast; players announce themselves during discovery.
constexpr uint8_t MSG_ROSTER        = 0xA2;

// End-of-game signal; forces any device still in-game into scoringState.
constexpr uint8_t MSG_END_GAME      = 0xAE;

// Next available in 0xA0 block: 0xA4 (before 0xAE) or 0xB0 (after)

} // namespace RadioMsg
