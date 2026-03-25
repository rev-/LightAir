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
// Three singletons share this class: baseO (team 0), baseX (team 1),
// and base (teamless, _team=0xFF).  The team is fixed at construction time.
//
// Teamless mode (_team == 0xFF)
//   The totem respawns any nearby player, independently of team.
//   Idle RGB LED shows white.  Beacon payload[0] = 0xFF so games
//   that support teamless bases can accept it from any player.
//   Respawn animation still shows the respawning player's team colour.
//
// Activation payload
//   payload[0] = roleId (BASE_O, BASE_X, or BASE) — team already known
//                from construction; payload is ignored beyond confirming roleId.
// ================================================================

using RadioMsg::MSG_BASE_BEACON;

static constexpr uint32_t BASE_BEACON_INTERVAL_MS = 1000;

class BaseTotem : public LightAir_TotemRunner {
    const uint8_t _team;        // 0=O, 1=X, 0xFF=teamless; fixed at construction
    uint32_t      _lastBeacon;

    void teamColor(uint8_t& r, uint8_t& g, uint8_t& b) const {
        if (_team == 0xFF) { r = 255; g = 255; b = 255; return; }  // white = teamless
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
        uint8_t r = 0, g = 0, b = 0;
        teamColor(r, g, b);
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
static BaseTotem s_base(0xFF);   // teamless

LightAir_TotemRunner* totemRunner_baseO = &s_baseO;
LightAir_TotemRunner* totemRunner_baseX = &s_baseX;
LightAir_TotemRunner* totemRunner_base  = &s_base;
