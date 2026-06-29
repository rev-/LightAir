#include <LightAir.h>
#include "TotemRoleIds.h"
#include "../config.h"

// ================================================================
// FlagTotem — flag-totem role runner (new architecture).
//
// States
//   FLAG_IN  (0) : flag at home; broadcasting MSG_FLAG_BEACON(state=IN).
//   FLAG_OUT (1) : flag carried by enemy; silent until returned.
//
// Lifecycle
//   onActivate() : enters FLAG_IN; shows Idle background (team colour glow).
//   update()     : in FLAG_IN, broadcasts MSG_FLAG_BEACON every
//                  FLAG_BEACON_INTERVAL_MS.
//   onMessage()  : driven by MSG_FLAG_NOTIFY unicasts addressed to this
//                  totem (payload[0] = FlagEvent sub-type):
//     FlagEvent::TAKEN   — FLAG_IN → FLAG_OUT + FlagMissing anim (team colour)
//                          + FlagTaken flash (picker's colour).
//     FlagEvent::DROPPED — FLAG_OUT → FLAG_IN + FlagReturn anim (flash).
//     FlagEvent::SCORED  — FLAG_OUT → FLAG_IN + FlagReturn anim (flash).
//   reset()      : returns to FLAG_IN; clears timer.
//
// Two singletons: flagO (owns the O flag) and flagX (owns the X flag).
// The team is fixed at construction and encodes which team's flag this is.
//
// Flag protocol (must stay in sync with the Flag ruleset, GameFlag.cpp)
//   The carrier remembers which flag totem it took and UNICASTS take/drop/
//   score (MSG_FLAG_NOTIFY) to that one totem.  Because the player emits the
//   take only within its own RSSI proximity gate of the flag, the totem
//   inherits that gate; and because it is unicast, only the specific flag the
//   carrier holds reacts (multiple flags per team work correctly).
//   MSG_FLAG_EVENT remains a broadcast used by *players* to sync flag-carrier
//   state and team points — this totem no longer listens to it.
// ================================================================

using RadioMsg::MSG_FLAG_BEACON;
using RadioMsg::MSG_FLAG_NOTIFY;
namespace FE = FlagEvent;

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

    void showIdle(LightAir_TotemOutput& out) const {
        uint8_t r, g, b;
        teamColor(r, g, b);
        TeamLedRhythm::Rhythm rh = TeamLedRhythm::forTeam(_team);
        out.ui.trigger(TotemUIEvent::FlagIdle, r, g, b, rh.periodMs, rh.pulseCount);
    }

    void returnFlag(LightAir_TotemOutput& out) {
        _state = FLAG_STATE_IN;
        uint8_t r, g, b;
        teamColor(r, g, b);
        // Restore the home-idle background, then flash the return one-shot
        // over it so the totem reads as "home" again once the flash ends.
        showIdle(out);
        out.ui.trigger(TotemUIEvent::FlagReturn, r, g, b);
    }

public:
    explicit FlagTotem(uint8_t team)
        : _team(team), _state(FLAG_STATE_IN), _lastBeacon(0) {}

    void onActivate(const LightAir_TotemActivation& /*info*/,
                    LightAir_TotemOutput& out) override {
        _state      = FLAG_STATE_IN;
        _lastBeacon = 0;
        showIdle(out);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        // This totem is driven by MSG_FLAG_NOTIFY unicasts addressed to it
        // specifically: the carrier remembers which flag totem it took and
        // unicasts take/drop/score to that one totem. Unicast targeting means
        // no team filter is needed (only the right totem receives it), and
        // multiple flags per team work correctly. payload[0] = FlagEvent sub.
        // (MSG_FLAG_EVENT remains a broadcast for player-to-player state sync.)
        if (msg.msgType != MSG_FLAG_NOTIFY) return;
        if (msg.payloadLen < 1)             return;

        const uint8_t sub = msg.payload[0];

        if (_state == FLAG_STATE_IN) {
            if (sub == FE::TAKEN) {
                _state = FLAG_STATE_OUT;
                uint8_t r, g, b;
                teamColor(r, g, b);
                // Looping background uses the flag's home-team colour;
                // the pickup flash uses the picking player's own colour.
                uint8_t pid = (msg.senderId < PlayerDefs::MAX_PLAYER_ID) ? msg.senderId : 0;
                out.ui.trigger(TotemUIEvent::FlagMissing, r, g, b);
                out.ui.trigger(TotemUIEvent::FlagTaken,
                               PlayerColors::kColors[pid][0],
                               PlayerColors::kColors[pid][1],
                               PlayerColors::kColors[pid][2]);
            }
            return;
        }

        // FLAG_STATE_OUT: a drop (carrier shot) or a score returns the flag home.
        if (sub == FE::DROPPED || sub == FE::SCORED) {
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
