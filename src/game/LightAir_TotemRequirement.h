#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_TotemRequirement — describes one totem role that a game
// uses, including how many totems of that role are needed and an
// optional pointer to a config variable that controls a per-totem
// cooldown (sent to the totem in the activation reply payload[1]).
//
// roleId      — TotemRoleId constant (BASE_O, FLAG_X, CP, BONUS, …)
// minCount    — minimum totems with this role required to start; 0 = optional
// maxCount    — maximum totems that can be assigned this role
// configSecs  — optional pointer to a game config int (seconds).
//               When non-null, its current value is sent as payload[1]
//               of the 0xF1 activation reply so the totem can apply a
//               game-specific cooldown instead of its built-in default.
//               nullptr = totem uses its own default.
// ----------------------------------------------------------------
struct LightAir_TotemRequirement {
    uint8_t     roleId;
    uint8_t     minCount;
    uint8_t     maxCount;
    const int*  configSecs;   // optional; points to a game config var
};
