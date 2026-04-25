#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_TotemRequirement — describes one totem role that a game
// uses, including how many totems of that role are needed and how
// to build the per-role activation payload sent in the 0xF1 reply.
//
// roleId        — TotemRoleId constant (BASE_O, FLAG_X, CP, BONUS, …)
// minCount      — minimum totems with this role required to start; 0 = optional
// maxCount      — maximum totems that can be assigned this role
// configSecs    — legacy single-byte config: when non-null, its value is
//                 sent as payload[1] of the activation reply.  Used by
//                 BONUS/MALUS for cooldown overrides.  Ignored when
//                 buildPayload is non-null.
// buildPayload  — optional callback that writes payload bytes [1..] into
//                 buf (caller has already placed roleId in buf[0]).
//                 maxLen is the number of bytes available starting at buf[0],
//                 so the callback may write up to maxLen-1 bytes.
//                 Returns the number of bytes written (0 if none).
//                 Takes priority over configSecs when non-null.
// ----------------------------------------------------------------
struct LightAir_TotemRequirement {
    uint8_t     roleId;
    uint8_t     minCount;
    uint8_t     maxCount;
    const int*  configSecs;                               // legacy; see above
    uint8_t   (*buildPayload)(uint8_t* buf, uint8_t maxLen); // optional; see above
};
