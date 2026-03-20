#include <LightAir.h>
#include "TotemRoleIds.h"

// ================================================================
// BaseTotem — respawn-base role runner (new architecture).
//
// Lifecycle
//   onActivate() : starts identity animation; resets beacon timer.
//   update()     : broadcasts MSG_BASE_BEACON every BASE_BEACON_INTERVAL_MS.
//   onMessage()  : on player reply (0x57) shows Respawn animation in
//                  the replying player's team colour.
//   reset()      : clears beacon timer; runner ready for next session.
//
// Two singletons share this class: baseO (team 0) and baseX (team 1).
// The team is fixed at construction time.
//
// Activation payload
//   payload[0] = roleId (BASE_O or BASE_X) — team already known from
//                construction; payload is ignored beyond confirming roleId.
// ================================================================

using RadioMsg::MSG_BASE_BEACON;

static constexpr uint32_t BASE_BEACON_INTERVAL_MS = 1000;

class BaseTotem : public LightAir_TotemRunner {
    const uint8_t _team;        // 0=O, 1=X; fixed at construction
    uint32_t      _lastBeacon;

    void teamColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = (_team == 0) ? 255 :   0;
        g                = 80;
        b = (_team == 0) ?   0 : 255;
    }

public:
    explicit BaseTotem(uint8_t team)
        : _team(team), _lastBeacon(0) {}

    void onActivate(const uint8_t* /*payload*/, uint8_t /*len*/,
                    LightAir_TotemOutput& out) override {
        _lastBeacon = 0;
        uint8_t r, g, b;
        teamColor(r, g, b);
        // Brief team-colour identity flash; Idle event is used as the
        // generic "show team colour" background trigger.
        out.ui.trigger(TotemUIEvent::Idle, r, g, b);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        // Accept player reply to our beacon only.
        if (msg.msgType != MSG_BASE_BEACON + 1) return;
        // Show Respawn animation in the replying player's team colour.
        uint8_t r = (msg.team == 0) ? 255 :   0;
        uint8_t g = 80;
        uint8_t b = (msg.team == 0) ?   0 : 255;
        out.ui.trigger(TotemUIEvent::Respawn, r, g, b);
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();
        if ((now - _lastBeacon) >= BASE_BEACON_INTERVAL_MS) {
            _lastBeacon = now;
            uint8_t pl[1] = { _team };
            out.radio.broadcast(MSG_BASE_BEACON, pl, 1);
        }
    }

    void reset() override {
        _lastBeacon = 0;
    }
};

// ---- Singletons ----
static BaseTotem s_baseO(0);
static BaseTotem s_baseX(1);

LightAir_TotemRunner* totemRunner_baseO = &s_baseO;
LightAir_TotemRunner* totemRunner_baseX = &s_baseX;
