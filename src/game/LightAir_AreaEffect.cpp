#include "LightAir_AreaEffect.h"
#include <string.h>

const AreaPolicy* areaFind(const LightAir_Game& game, uint8_t id) {
    for (uint8_t i = 0; i < game.areaPolicyCount; i++)
        if (game.areaPolicies[i].id == id) return &game.areaPolicies[i];
    return nullptr;
}

const AreaPolicy* areaForProjector(const LightAir_Game& game, uint8_t projector) {
    for (uint8_t i = 0; i < game.areaPolicyCount; i++) {
        const AreaPolicy& p = game.areaPolicies[i];
        if (p.on != AreaTrigger::NONE && p.projector == projector) return &p;
    }
    return nullptr;
}

uint8_t areaMagnitude(const AreaPolicy& p, int8_t rssi) {
    for (uint8_t i = 0; i < p.bandCount; i++)
        if (rssi >= p.bands[i].rssi) return p.bands[i].magnitude;
    return 0;
}

void areaBeacon(RadioOutput& out, uint8_t policyId, uint8_t originator,
                uint8_t originatorTeam) {
    const uint8_t payload[3] = { policyId, originator, originatorTeam };
    out.broadcast(RadioMsg::MSG_AREA, payload, sizeof(payload), /*resend*/ 0);
}

RadioPacket areaHit(const AreaPolicy& p, uint8_t originator,
                    uint8_t originatorTeam, uint8_t magnitude) {
    RadioPacket hit;
    memset(&hit, 0, sizeof(hit));
    hit.senderId   = originator;
    hit.team       = originatorTeam;
    hit.msgType    = RadioMsg::MSG_LIT;
    hit.payloadLen = 5;
    hit.payload[0] = magnitude;
    hit.payload[1] = (p.on != AreaTrigger::NONE) ? p.projector : 0;
    hit.payload[2] = p.roleTag;
    hit.payload[3] = 0;                         // the reach was the beacon's
    hit.payload[4] = AreaDefaults::HIT_FLAG_AREA;
    return hit;
}

bool areaIsHit(const RadioPacket& pkt) {
    return pkt.msgType == RadioMsg::MSG_LIT && pkt.payloadLen >= 5 &&
           (pkt.payload[4] & AreaDefaults::HIT_FLAG_AREA) != 0;
}
