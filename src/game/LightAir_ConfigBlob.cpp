// ----------------------------------------------------------------
// LightAir_ConfigBlob.cpp — serialize / apply the pre-match config blob.
// Format in LightAir_ConfigBlob.h.  Kept apart from the setup menu so the
// host test suite can round-trip it without the menu's NVS/display deps.
// ----------------------------------------------------------------
#include "LightAir_ConfigBlob.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "GameConfig";

static uint16_t blobSize(const LightAir_Game& game) {
    return 2
        + (uint16_t)game.configCount * 4
        + (game.teamCount > 0 ? PlayerDefs::MAX_PLAYER_ID : 0)
        + TotemDefs::MAX_TOTEMS   // 16 × uint8_t roleId
        + TotemDefs::MAX_TOTEMS   // 16 × uint8_t option
        + 1;                      // session token
}

uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen,
                                const uint8_t totemAssignment[TotemDefs::MAX_TOTEMS],
                                const uint8_t teamMap[PlayerDefs::MAX_PLAYER_ID],
                                uint8_t sessionToken,
                                const uint8_t totemOption[TotemDefs::MAX_TOTEMS]) {
    if (maxLen < blobSize(game)) return 0;

    uint16_t id = game.typeId;
    memcpy(buf, &id, 2);
    uint16_t pos = 2;

    // configVars
    for (uint8_t v = 0; v < game.configCount; v++) {
        int32_t val = (int32_t)*game.configVars[v].value;
        memcpy(buf + pos, &val, 4);
        pos += 4;
    }

    // teamMap (MAX_PLAYER_ID bytes; only if game.teamCount > 0)
    if (game.teamCount > 0) {
        for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++)
            buf[pos++] = teamMap ? teamMap[i] : 0xFF;
    }

    // 16 totem slot assignments (roleId per slot; 0 = unassigned)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++)
        buf[pos++] = totemAssignment ? totemAssignment[s] : 0;

    // 16 totem options (1-based per slot; 0 = none)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++)
        buf[pos++] = totemOption ? totemOption[s] : 0;

    // Session token (1 byte; 0 = no session isolation)
    buf[pos++] = sessionToken;

    return pos;
}

bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len,
                        uint8_t totemAssignmentOut[TotemDefs::MAX_TOTEMS],
                        uint8_t teamMapOut[PlayerDefs::MAX_PLAYER_ID],
                        uint8_t* sessionTokenOut,
                        uint8_t totemOptionOut[TotemDefs::MAX_TOTEMS]) {
    uint16_t minNeeded = blobSize(game);
    if (len < minNeeded) {
        ESP_LOGW(TAG, "config blob too short: got %u, need %u", (unsigned)len, (unsigned)minNeeded);
        return false;
    }

    uint16_t id;
    memcpy(&id, buf, 2);
    if (id != game.typeId) return false;

    uint16_t pos = 2;

    // configVars
    for (uint8_t v = 0; v < game.configCount; v++) {
        const ConfigVar& var = game.configVars[v];
        int32_t val;
        memcpy(&val, buf + pos, 4);
        pos += 4;
        if (val < var.min) val = var.min;
        if (val > var.max) val = var.max;
        *var.value = (int)val;
    }

    // teamMap (MAX_PLAYER_ID bytes; only if game.teamCount > 0)
    if (game.teamCount > 0) {
        for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++) {
            uint8_t t = buf[pos++];
            if (teamMapOut)    teamMapOut[i]    = t;
            if (game.teamMap) game.teamMap[i]   = t;
        }
    }

    // totem slot assignments (roleId per slot)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t r = buf[pos++];
        if (totemAssignmentOut) totemAssignmentOut[s] = r;
    }

    // totem options (1-based per slot)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t o = buf[pos++];
        if (totemOptionOut) totemOptionOut[s] = o;
    }

    // Session token (last byte)
    if (sessionTokenOut) *sessionTokenOut = buf[pos];

    return true;
}
