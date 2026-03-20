#include <LightAir.h>
#include "TotemRoleIds.h"

// ================================================================
// CPTotem — control-point role runner (new architecture).
//
// States (internal; no explicit enum needed)
//   cpTeam == 0xFF : NEUTRAL — no team attached; no points awarded.
//   cpTeam == 0    : OWNED_O — team O attached; scoring every 10 s.
//   cpTeam == 1    : OWNED_X — team X attached; scoring every 10 s.
//
// Protocol
//   Every CP_BEACON_INTERVAL_MS the runner:
//     1. Evaluates the presence flags collected from player replies.
//     2. Updates cpTeam if exactly one team is present (contested = hold).
//     3. Awards a point (MSG_CP_SCORE) if attachment is unchanged for
//        CP_SCORE_INTERVAL_MS.
//     4. Broadcasts MSG_CP_BEACON with payload[0] = cpTeam.
//     5. Clears presence flags for the next window.
//
//   Players reply to MSG_CP_BEACON with subType 1 (team-O) or 2 (team-X)
//   when their RSSI to this totem is above their own proximity threshold.
//   subType 0 (auto empty-reply) is ignored.
//
// Activation payload
//   payload[0] = roleId (CP).  No additional config bytes needed.
// ================================================================

using RadioMsg::MSG_CP_BEACON;
using RadioMsg::MSG_CP_SCORE;

static constexpr uint32_t CP_BEACON_INTERVAL_MS = 2000;
static constexpr uint32_t CP_SCORE_INTERVAL_MS  = 10000;
static constexpr uint8_t  CP_TEAM_NONE          = 0xFF;

class CPTotem : public LightAir_TotemRunner {
    uint8_t  _cpTeam;
    bool     _presenceO;
    bool     _presenceX;
    uint32_t _windowStart;
    uint32_t _attachStart;

    void updateBackground(LightAir_TotemOutput& out) const {
        if      (_cpTeam == 0) out.ui.trigger(TotemUIEvent::ControlO);
        else if (_cpTeam == 1) out.ui.trigger(TotemUIEvent::ControlX);
        else                   out.ui.trigger(TotemUIEvent::Idle);
    }

public:
    CPTotem()
        : _cpTeam(CP_TEAM_NONE), _presenceO(false), _presenceX(false),
          _windowStart(0), _attachStart(0) {}

    void onActivate(const uint8_t* /*payload*/, uint8_t /*len*/,
                    LightAir_TotemOutput& out) override {
        _cpTeam      = CP_TEAM_NONE;
        _presenceO   = false;
        _presenceX   = false;
        _windowStart = millis();
        _attachStart = _windowStart;
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& /*out*/) override {
        // Collect player presence replies: subType 1=team-O, 2=team-X.
        if (msg.msgType != MSG_CP_BEACON + 1) return;
        if (msg.payloadLen == 0) return;
        uint8_t sub = msg.payload[0];
        if (sub == 1) _presenceO = true;
        if (sub == 2) _presenceX = true;
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();
        if ((now - _windowStart) < CP_BEACON_INTERVAL_MS) return;

        // ---- Evaluate the window that just closed ----
        if (_presenceO || _presenceX) {
            uint8_t newTeam = _cpTeam;
            if  (_presenceO && !_presenceX) newTeam = 0;
            if  (_presenceX && !_presenceO) newTeam = 1;
            // Both teams present → contested; hold current attachment.

            if (newTeam != _cpTeam) {
                // Team switched: reset scoring countdown.
                _cpTeam      = newTeam;
                _attachStart = now;
                updateBackground(out);
            } else if (_cpTeam != CP_TEAM_NONE) {
                // Same team: award a point if 10 s have elapsed.
                if ((now - _attachStart) >= CP_SCORE_INTERVAL_MS) {
                    uint8_t pl[1] = { _cpTeam };
                    out.radio.broadcast(MSG_CP_SCORE, pl, 1);
                    out.ui.trigger(TotemUIEvent::Bonus);
                    _attachStart = now;  // restart scoring countdown
                }
            }

            if (_presenceO && _presenceX) {
                out.ui.trigger(TotemUIEvent::ControlContest);
            }
        } else if (_cpTeam == CP_TEAM_NONE) {
            out.ui.trigger(TotemUIEvent::Idle);
        }

        // ---- Open next window ----
        _presenceO   = false;
        _presenceX   = false;
        _windowStart = now;
        uint8_t pl[1] = { _cpTeam };
        out.radio.broadcast(MSG_CP_BEACON, pl, 1);
    }

    void reset() override {
        _cpTeam      = CP_TEAM_NONE;
        _presenceO   = false;
        _presenceX   = false;
        _windowStart = 0;
        _attachStart = 0;
    }
};

// ---- Singleton ----
static CPTotem s_cp;

LightAir_TotemRunner* totemRunner_cp = &s_cp;
