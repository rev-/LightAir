#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemRoleId — sequential role identifiers for the totem role
// registry.
//
// totemRoleName(roleId) returns a short label string for display.
//
// 0 = NONE sentinel (unassigned).
// Values are transmitted in the activation reply (0xF1 payload[0]).
//
// The old architecture used totemVarIdx in payload[0]; the new
// architecture uses TotemRoleId values.  0xFF is reserved for the
// old generic-totem path and must never be used as a roleId here.
// ----------------------------------------------------------------
namespace TotemRoleId {
    constexpr uint8_t NONE   = 0;   // sentinel — not a real role
    constexpr uint8_t BASE_O = 1;   // respawn base, team O
    constexpr uint8_t BASE_X = 2;   // respawn base, team X
    constexpr uint8_t FLAG_O = 3;   // flag totem, team O owns this flag
    constexpr uint8_t FLAG_X = 4;   // flag totem, team X owns this flag
    constexpr uint8_t CP     = 5;   // control point (teamless)
    constexpr uint8_t BONUS  = 6;   // bonus pickup (teamless)
    constexpr uint8_t MALUS  = 7;   // malus pickup (teamless)
    constexpr uint8_t BASE   = 8;   // respawn base, teamless (respawns any nearby player)
}

inline const char* totemRoleName(uint8_t roleId) {
    switch (roleId) {
        case TotemRoleId::BASE_O: return "BASE_O";
        case TotemRoleId::BASE_X: return "BASE_X";
        case TotemRoleId::FLAG_O: return "FLAG_O";
        case TotemRoleId::FLAG_X: return "FLAG_X";
        case TotemRoleId::CP:     return "CP";
        case TotemRoleId::BONUS:  return "BONUS";
        case TotemRoleId::MALUS:  return "MALUS";
        case TotemRoleId::BASE:   return "BASE";
        default:                  return "?";
    }
}
