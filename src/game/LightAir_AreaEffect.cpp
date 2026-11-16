#include "LightAir_AreaEffect.h"
#include <string.h>

const AreaPolicy* areaFind(const LightAir_Game& game, uint8_t id) {
    for (uint8_t i = 0; i < game.areaPolicyCount; i++)
        if (game.areaPolicies[i].id == id) return &game.areaPolicies[i];
    return nullptr;
}

uint16_t litEffects(const RadioPacket& lit) {
    if (lit.payloadLen <= LitPayload::EFFECTS_HI) return 0;
    return (uint16_t)(lit.payload[LitPayload::EFFECTS_LO] |
                      (lit.payload[LitPayload::EFFECTS_HI] << 8));
}

const AreaPolicy* areaForEffects(const LightAir_Game& game, uint16_t effects) {
    if (!effects) return nullptr;
    for (uint8_t i = 0; i < game.areaPolicyCount; i++) {
        const AreaPolicy& p = game.areaPolicies[i];
        if (p.on != AreaTrigger::NONE && (p.effect & effects)) return &p;
    }
    return nullptr;
}

uint8_t areaMagnitude(const AreaPolicy& p, int8_t rssi) {
    for (uint8_t i = 0; i < p.bandCount; i++)
        if (rssi >= p.bands[i].rssi) return p.bands[i].magnitude;
    return 0;
}

void areaBeacon(RadioOutput& out, uint8_t policyId, uint8_t originator,
                uint8_t originatorTeam, uint8_t roleTag, uint8_t kind) {
    const uint8_t payload[5] = { policyId, originator, originatorTeam, roleTag, kind };
    out.broadcast(RadioMsg::MSG_AREA, payload, sizeof(payload), /*resend*/ 0);
}

RadioPacket areaHit(const AreaPolicy& p, uint8_t originator,
                    uint8_t originatorTeam, uint8_t magnitude,
                    uint8_t roleTag, uint8_t kind) {
    RadioPacket hit;
    memset(&hit, 0, sizeof(hit));
    hit.senderId   = originator;
    hit.team       = originatorTeam;
    hit.msgType    = RadioMsg::MSG_LIT;
    // Never the trigger bit: an area hit must not look like a beam that
    // could start another area, whatever else guards against it.
    const uint16_t effects = (uint16_t)(p.effects & ~p.effect);
    hit.payloadLen = LitPayload::FLAGS + 1;
    hit.payload[LitPayload::STRENGTH]   = magnitude;
    hit.payload[LitPayload::KIND]       = kind;
    hit.payload[LitPayload::ROLE_TAG]   = roleTag;
    hit.payload[LitPayload::RSSI_GATE]  = 0;    // the reach was the beacon's
    hit.payload[LitPayload::EFFECTS_LO] = (uint8_t)(effects & 0xFF);
    hit.payload[LitPayload::EFFECTS_HI] = (uint8_t)(effects >> 8);
    hit.payload[LitPayload::FLAGS]      = AreaDefaults::HIT_FLAG_AREA;
    return hit;
}

bool areaIsHit(const RadioPacket& pkt) {
    return pkt.msgType == RadioMsg::MSG_LIT && pkt.payloadLen > LitPayload::FLAGS &&
           (pkt.payload[LitPayload::FLAGS] & AreaDefaults::HIT_FLAG_AREA) != 0;
}
