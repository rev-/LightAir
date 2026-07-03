#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemProgramEntry — one serialized TotemVM program for one totem
// role of a game (see docs/totem-behavior-handshake.md).
//
// The bytes travel inside the 0xF1 activation reply:
//   [0] roleId  [1] sessionToken  [2:3] gameTimeLeft
//   [4] vmVersion  [5:6] progLen (u16 LE)  [7..] program
//
// A game exposes its programs through LightAir_Game::totemProgram —
// a provider function rather than a static table because the Lua
// binding patches config-derived immediates ({"cfg"} seconds) into
// the bytes with the *current* config value at reply time.
// ----------------------------------------------------------------
struct TotemProgramEntry {
    uint8_t        roleId;   // TotemRoleId constant
    uint8_t        len;      // program bytes (≤ TotemVMDefs::MAX_PROG)
    const uint8_t* bytes;
};
