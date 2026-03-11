#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemRole — describes one type of totem a game can use.
//
// Totems use the same hardware as players but are deployed in a
// fixed in-field role (respawn base, control point, flag, mine…).
// Each game lists the roles it supports together with the minimum
// count required before the game can start.
//
// A minCount of 0 means the role is optional: the participant menu
// will still offer the role for configuration but will not block
// the user from starting without any totem of that type.
// ----------------------------------------------------------------
struct TotemRole {
    const char* name;      // display name, e.g. "Respawn Base"
    uint8_t     minCount;  // minimum totems of this role required to start (0 = optional)
};
