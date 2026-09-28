#pragma once
#include <stdint.h>
#include "LightAir_Game.h"
#include "../config.h"

// ----------------------------------------------------------------
// Config blob — the one packet the DM broadcasts before a match (and the
// NVS checkpoint S1 "Restart" reads back).  Format:
//
//   [uint16_t typeId]
//   [int32_t configVar0] … [int32_t configVarN]
//   [uint8_t teamMap0] … [uint8_t teamMap16]  ← MAX_PLAYER_ID bytes; only if game.teamCount > 0
//                                                values: 0..teamCount-1 = team index, 0xFF = unassigned
//   [uint8_t totemSlot0] … [uint8_t totemSlot15]  ← 16 entries (TotemRoleId per slot; 0 = unassigned)
//   [uint8_t totemOpt0]  … [uint8_t totemOpt15]   ← 16 entries (1-based option per slot; 0 = none)
//   [uint8_t sessionToken]                   ← last byte; 0 = no session isolation
//
// Receivers call game_apply_config() to update in-place.  A blob in the
// older format (no option bytes) is too short and is rejected whole.
// ----------------------------------------------------------------

// Serialize all config data of a game into a byte buffer.
// totemAssignment[slot] = roleId for each of the 16 totem slots (0 = unassigned).
// totemOption[slot]     = the DM's per-totem option, 1-based (0 = none).
// teamMap[id] = team index 0..teamCount-1 (or 0xFF) for each player; only written when game.teamCount > 0.
// sessionToken is appended as the final byte (0 = no session isolation).
// Returns bytes written, or 0 if maxLen is insufficient.
uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen,
                                const uint8_t totemAssignment[TotemDefs::MAX_TOTEMS] = nullptr,
                                const uint8_t teamMap[PlayerDefs::MAX_PLAYER_ID] = nullptr,
                                uint8_t sessionToken = 0,
                                const uint8_t totemOption[TotemDefs::MAX_TOTEMS] = nullptr);

// Apply a received config blob.
// Returns false if typeId doesn't match or blob is too short.
// Values are clamped to [min, max].  Writes roleIds into totemAssignmentOut if non-null.
// Writes per-player team indices into teamMapOut (size MAX_PLAYER_ID) if non-null.
// If sessionTokenOut is non-null, the trailing session token byte is written there.
// Writes per-slot options into totemOptionOut if non-null.
bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len,
                        uint8_t totemAssignmentOut[TotemDefs::MAX_TOTEMS] = nullptr,
                        uint8_t teamMapOut[PlayerDefs::MAX_PLAYER_ID] = nullptr,
                        uint8_t* sessionTokenOut = nullptr,
                        uint8_t totemOptionOut[TotemDefs::MAX_TOTEMS] = nullptr);
