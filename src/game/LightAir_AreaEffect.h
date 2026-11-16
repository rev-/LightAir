#pragma once
#include "../config.h"
#include "LightAir_Game.h"
#include "LightAir_RadioOutput.h"
#include "../radio/LightAir_Radio.h"

// ----------------------------------------------------------------
// Area effects — the wire format and the pure parts of the mechanism.
//
// One owner for what an area beacon and an area hit look like, so the
// runner (receives, triggers, credits), the Lua binding (la.area_emit) and,
// later, the totem firmware cannot drift apart.  The policies are data
// (AreaPolicy, LightAir_Game.h §7b); the decisions that need the session —
// who is me, which team, whether a hold is on — are the runner's.
// ----------------------------------------------------------------

// The policy with this id, or nullptr.
const AreaPolicy* areaFind(const LightAir_Game& game, uint8_t id);

// The effects word of a LIT (0 for one too short to carry it).
uint16_t litEffects(const RadioPacket& lit);

// The policy a landed hit carrying these effects triggers, or nullptr.
const AreaPolicy* areaForEffects(const LightAir_Game& game, uint16_t effects);

// The hit strength a beacon read at `rssi` dBm carries under this policy:
// the first band (strongest first) the reading reaches.  0 = out of the area.
uint8_t areaMagnitude(const AreaPolicy& p, int8_t rssi);

// Queue the beacon: MSG_AREA [policy, originator, originator's team, role
// tag, kind], single-hop.  The only place it is ever built.
void areaBeacon(RadioOutput& out, uint8_t policyId, uint8_t originator,
                uint8_t originatorTeam, uint8_t roleTag, uint8_t kind);

// The LIT an area beacon becomes for the ruleset: from the originator, at
// the band's strength, with the beacon's role tag and kind, the policy's
// own effects (never its trigger bit), no RSSI gate, and the area flag set
// (MSG_LIT payload[LitPayload::FLAGS]) so the ruleset can tell it apart.
RadioPacket areaHit(const AreaPolicy& p, uint8_t originator,
                    uint8_t originatorTeam, uint8_t magnitude,
                    uint8_t roleTag, uint8_t kind);

// True for a LIT built by areaHit().  A real LIT never carries the flag.
bool areaIsHit(const RadioPacket& pkt);
