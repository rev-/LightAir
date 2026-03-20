#pragma once
#include <stdint.h>
#include "../game/LightAir_TotemRunner.h"

// ----------------------------------------------------------------
// TotemRole — static descriptor for one totem role.
//
// Registered in LightAir_TotemRoleManager at boot via
// AllTotems::registerAllTotems().  The driver looks up the matching
// descriptor on activation (0xF1 payload[0] == roleId) and hands
// control to the runner singleton.
// ----------------------------------------------------------------
struct TotemRole {
    uint8_t               roleId;   // TotemRoleId value; 0 = NONE sentinel
    const char*           name;     // short label e.g. "BASE_O" (for logging/menus)
    uint8_t               team;     // 0=O, 1=X, 0xFF=teamless
    LightAir_TotemRunner* runner;   // singleton runner instance
};
