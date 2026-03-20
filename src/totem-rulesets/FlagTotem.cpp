#include <LightAir.h>
#include "TotemRoleIds.h"

// ================================================================
// FlagTotem — flag-totem role runner (new architecture).
//
// States
//   FLAG_IN  (0) : flag at home; broadcasting MSG_FLAG_BEACON(state=IN).
//   FLAG_OUT (1) : flag carried by enemy; silent until returned.
//
// Lifecycle
//   onActivate() : enters FLAG_IN; shows generic Idle background.
//   update()     : in FLAG_IN, broadcasts MSG_FLAG_BEACON every
//                  FLAG_BEACON_INTERVAL_MS.
//   onMessage()  :
//     FLAG_IN  — on enemy-player reply (0x59) → FLAG_OUT + FlagMissing anim.
//     FLAG_OUT — on MSG_FLAG_RETURN or MSG_FLAG_SCORE flood matching our
//                team → FLAG_IN + FlagReturn anim.
//   reset()      : returns to FLAG_IN; clears timer.
//
// Two singletons: flagO (owns the O flag) and flagX (owns the X flag).
// The team is fixed at construction and encodes which team's flag this is.
//
// Flag pickup protocol
//   A player who spots MSG_FLAG_BEACON(state=IN) from the enemy flag with
//   sufficient RSSI sends a reply (0x59) to claim the flag.  The flag
//   totem detects this reply and transitions to FLAG_OUT.
//
// Flag return protocol
//   MSG_FLAG_RETURN : broadcast flood from carrier who was shot.
//   MSG_FLAG_SCORE  : broadcast flood from carrier who scored.
//   Both carry payload[0] = flagTeam; only packets matching _team are acted on.
// ================================================================

using RadioMsg::MSG_FLAG_BEACON;
using RadioMsg::MSG_FLAG_RETURN;
using RadioMsg::MSG_FLAG_SCORE;

static constexpr uint32_t FLAG_BEACON_INTERVAL_MS = 500;
static constexpr uint8_t  FLAG_STATE_IN  = 0;
static constexpr uint8_t  FLAG_STATE_OUT = 1;

class FlagTotem : public LightAir_TotemRunner {
    const uint8_t _team;        // 0=O, 1=X; fixed at construction
    uint8_t       _state;       // FLAG_STATE_IN or FLAG_STATE_OUT
    uint32_t      _lastBeacon;

    void teamColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = (_team == 0) ? 255 :   0;
        g = 80;
        b = (_team == 0) ?   0 : 255;
    }

    void returnFlag(LightAir_TotemOutput& out) {
        _state = FLAG_STATE_IN;
        uint8_t r, g, b;
        teamColor(r, g, b);
        out.ui.trigger(TotemUIEvent::FlagReturn, r, g, b);
    }

public:
    explicit FlagTotem(uint8_t team)
        : _team(team), _state(FLAG_STATE_IN), _lastBeacon(0) {}

    void onActivate(const uint8_t* /*payload*/, uint8_t /*len*/,
                    LightAir_TotemOutput& out) override {
        _state      = FLAG_STATE_IN;
        _lastBeacon = 0;
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        if (_state == FLAG_STATE_IN) {
            // Enemy-team player replies to our beacon → flag picked up.
            if (msg.msgType == MSG_FLAG_BEACON + 1 && msg.team != _team) {
                _state = FLAG_STATE_OUT;
                uint8_t r, g, b;
                teamColor(r, g, b);
                // Set looping background first, then play one-shot pickup flash on top.
                out.ui.trigger(TotemUIEvent::FlagMissing, r, g, b);
                out.ui.trigger(TotemUIEvent::FlagTaken,   r, g, b);
            }
            return;
        }

        // FLAG_STATE_OUT: wait for return or score broadcast.
        if (msg.payloadLen < 1) return;
        if (msg.payload[0] != _team) return;  // not our flag's event

        if (msg.msgType == MSG_FLAG_RETURN || msg.msgType == MSG_FLAG_SCORE) {
            returnFlag(out);
        }
    }

    void update(LightAir_TotemOutput& out) override {
        if (_state != FLAG_STATE_IN) return;
        uint32_t now = millis();
        if ((now - _lastBeacon) >= FLAG_BEACON_INTERVAL_MS) {
            _lastBeacon = now;
            uint8_t pl[2] = { FLAG_STATE_IN, _team };
            out.radio.broadcast(MSG_FLAG_BEACON, pl, 2);
        }
    }

    void reset() override {
        _state      = FLAG_STATE_IN;
        _lastBeacon = 0;
    }
};

// ---- Singletons ----
static FlagTotem s_flagO(0);
static FlagTotem s_flagX(1);

LightAir_TotemRunner* totemRunner_flagO = &s_flagO;
LightAir_TotemRunner* totemRunner_flagX = &s_flagX;
