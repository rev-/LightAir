#include <LightAir.h>
#include "TotemRoleIds.h"
#include "../config.h"

// ================================================================
// CPTotem — control-point role runner (new architecture).
//
// States (internal; no explicit enum needed)
//   cpTeam == 0xFF : NEUTRAL — no team attached; no points awarded.
//   cpTeam == 0–15 : OWNED   — team/player index attached; scoring every 10 s.
//
// Team encoding
//   cpTeam is a 0-indexed team/player slot:
//     0 = team O  (two-team games) — display: cyan  (TeamColors::kColors[0])
//     1 = team X  (two-team games) — display: magenta (TeamColors::kColors[1])
//     2–15 = individual players in King-of-Hill (player ID = cpTeam+1);
//            display: PlayerColors::kColors[cpTeam+1]
//
// Protocol
//   Every CP_BEACON_INTERVAL_MS the runner:
//     1. Evaluates the presence flags collected from player replies.
//     2. Updates cpTeam if exactly one team/player is present (contested = hold).
//     3. Awards a point (MSG_CP_SCORE) if attachment is unchanged for
//        CP_SCORE_INTERVAL_MS.
//     4. Broadcasts MSG_CP_BEACON with payload[0] = cpTeam (0–15 or 0xFF).
//     5. Clears presence flags for the next window.
//
//   Players reply to MSG_CP_BEACON with subType = myTeam + 1
//   (1 = team 0/player 1, 2 = team 1/player 2, … 16 = player 16)
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
    uint16_t _presenceMask;  // bit i = team/player i present this window (subType i+1)
    uint32_t _windowStart;
    uint32_t _attachStart;

    void updateBackground(LightAir_TotemOutput& out) const {
        if (_cpTeam == CP_TEAM_NONE) {
            out.ui.trigger(TotemUIEvent::Idle);
        } else if (_cpTeam < 2) {
            // Two-team games: use team colour (cyan for O, magenta for X).
            out.ui.trigger(TotemUIEvent::Control, _cpTeam);
        } else {
            // King-of-Hill individual player: player ID = cpTeam + 1.
            out.ui.trigger(TotemUIEvent::Control, 0xFF, _cpTeam + 1);
        }
    }

public:
    CPTotem()
        : _cpTeam(CP_TEAM_NONE), _presenceMask(0),
          _windowStart(0), _attachStart(0) {}

    void onActivate(const uint8_t* /*payload*/, uint8_t /*len*/,
                    LightAir_TotemOutput& out) override {
        _cpTeam       = CP_TEAM_NONE;
        _presenceMask = 0;
        _windowStart  = millis();
        _attachStart  = _windowStart;
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& /*out*/) override {
        // Collect player presence replies.
        // subType 1–16 → bit 0–15 in presence mask (subType = team + 1).
        if (msg.msgType != MSG_CP_BEACON + 1) return;
        if (msg.payloadLen == 0) return;
        uint8_t sub = msg.payload[0];
        if (sub >= 1 && sub <= 16)
            _presenceMask |= (uint16_t)(1u << (sub - 1));
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();
        if ((now - _windowStart) < CP_BEACON_INTERVAL_MS) return;

        // ---- Evaluate the window that just closed ----
        bool anyPresent    = (_presenceMask != 0);
        bool singlePresent = anyPresent && (_presenceMask & (_presenceMask - 1)) == 0;

        if (anyPresent) {
            uint8_t newTeam = _cpTeam;
            if (singlePresent) {
                // Find the index of the single set bit (0–15).
                newTeam = 0;
                uint16_t m = _presenceMask;
                while (!((m >> newTeam) & 1u)) newTeam++;
            }
            // Multiple teams present → contested; hold current attachment.

            if (newTeam != _cpTeam) {
                // Owner switched: reset scoring countdown.
                _cpTeam      = newTeam;
                _attachStart = now;
                updateBackground(out);
            } else if (singlePresent && _cpTeam != CP_TEAM_NONE) {
                // Same owner: award a point if 10 s have elapsed.
                if ((now - _attachStart) >= CP_SCORE_INTERVAL_MS) {
                    uint8_t pl[1] = { _cpTeam };
                    out.radio.broadcast(MSG_CP_SCORE, pl, 1);
                    out.ui.trigger(TotemUIEvent::Bonus);
                    _attachStart = now;  // restart scoring countdown
                }
            }

            if (!singlePresent) {
                out.ui.trigger(TotemUIEvent::ControlContest);
            }
        } else if (_cpTeam == CP_TEAM_NONE) {
            out.ui.trigger(TotemUIEvent::Idle);
        }

        // ---- Open next window ----
        _presenceMask = 0;
        _windowStart  = now;
        uint8_t pl[1] = { _cpTeam };
        out.radio.broadcast(MSG_CP_BEACON, pl, 1);
    }

    void reset() override {
        _cpTeam       = CP_TEAM_NONE;
        _presenceMask = 0;
        _windowStart  = 0;
        _attachStart  = 0;
    }
};

// ---- Singleton ----
static CPTotem s_cp;

LightAir_TotemRunner* totemRunner_cp = &s_cp;
