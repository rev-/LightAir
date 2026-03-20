#pragma once
#include <stdint.h>
#include "LightAir_TotemRole.h"
#include "../config.h"

// ----------------------------------------------------------------
// LightAir_TotemRoleManager — boot-time registry of TotemRole
// descriptors.
//
// Populated once at startup via registerRole() before any game
// session begins.  The TotemDriver holds an optional pointer to
// this registry; on receiving an activation reply (0xF1) with a
// known roleId it bypasses the old typeId-based lookup path.
//
// Usage:
//   LightAir_TotemRoleManager roleMgr;
//   registerAllTotems(roleMgr);
//   LightAir_TotemDriver driver(radio, gameMgr, ui, &roleMgr);
// ----------------------------------------------------------------
class LightAir_TotemRoleManager {
public:
    // Register a role.  Returns false if the registry is full or a
    // role with the same non-zero roleId already exists.
    bool registerRole(const TotemRole& role);

    // Look up by roleId.  Returns nullptr if not found.
    const TotemRole* findById(uint8_t roleId) const;

    uint8_t count() const { return _count; }

private:
    TotemRole _roles[TotemDefs::MAX_TOTEM_ROLES];
    uint8_t   _count = 0;
};
