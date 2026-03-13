#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemVar — one named totem role that the host assigns to a
// specific totem device ID during setup.
//
// Totem device IDs run downward from 254 (totem01=254, …, totem16=239).
// id points to a file-scope int in the ruleset.  0 = unassigned.
//
// The host selects which physical totem (by short ID "01"…"16")
// fills each named role in LightAir_GameSetupMenu (S4c).
//
// Serialized as one int32_t per entry in the config blob, after the
// configVars section.  game_apply_config() updates *id in-place on
// receiving devices so all players share the same role assignments.
// ----------------------------------------------------------------
struct TotemVar {
    const char* name;      // role label, e.g. "Base", "Flag"
    int*        id;        // assigned totem device ID (0=unassigned, 239–254=totem16–01)
    uint8_t     team;      // 0=O, 1=X, 0xFF=no team association
    bool        required;  // if true: *id must be != 0 before game can start
};
