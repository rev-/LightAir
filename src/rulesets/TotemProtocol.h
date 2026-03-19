#pragma once
#include <stdint.h>

// ================================================================
// Totem Protocol — shared constants for all totem hardware.
//
// A single MSG_TOTEM_BEACON message type is used by every totem
// regardless of its in-game role (base, flag, control-point, …).
// The totem broadcasts this message periodically.  Receiving players
// decide whether to reply, and the reply subType carries the
// game-specific semantic (e.g. "respawn request", "flag pickup").
//
// By unifying on one beacon type the totem firmware remains
// game-agnostic: the same firmware image can run on any totem;
// role behaviour is determined entirely by how the active game's
// player software interprets the message and any replies.
//
// 0xF0/0xF1 are reserved for the totem beacon/reply pair and must
// not be re-used by any per-game message table.
// ================================================================

// Even: totem → player beacon.
// Odd (0xF1): auto-reply or player reply; subType carries meaning.
constexpr uint8_t MSG_TOTEM_BEACON = 0xF0;

// Universal end-of-game roster broadcast (typeId == UNIVERSAL).
// Sent by the host to every totem at the end of the game.
// TotemDriver calls runner->onRoster(), then reset() on receipt.
constexpr uint8_t MSG_ROSTER = 0xF2;
