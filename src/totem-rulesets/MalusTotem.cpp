#include <LightAir.h>
#include "TotemRoleIds.h"

// ================================================================
// MalusTotem — malus-pickup role runner (new architecture).
//
// Identical state machine to BonusTotem but uses MSG_MALUS_BEACON
// and TotemUIEvent::Malus.
//
// States
//   READY    : broadcasting MSG_MALUS_BEACON every BEACON_INTERVAL_MS.
//              On player reply (0x61) → COOLDOWN; triggers Malus anim.
//   COOLDOWN : silent; waits _cooldownSecs seconds then → READY; triggers Active (dim red).
//
// Activation
//   info.roleId       = MALUS
//   info.configSecs   = cooldown interval in seconds, if info.hasConfigSecs;
//                       otherwise DEFAULT_COOLDOWN_SECS.
//
// The effect of the malus is entirely determined by the player side.
// ================================================================

using RadioMsg::MSG_MALUS_BEACON;

static constexpr uint8_t  DEFAULT_COOLDOWN_SECS    = 30;
static constexpr uint32_t MALUS_BEACON_INTERVAL_MS = 2000;

enum MalusState : uint8_t { MALUS_READY, MALUS_COOLDOWN };

class MalusTotem : public LightAir_TotemRunner {
    MalusState _state;
    uint8_t    _cooldownSecs;
    uint32_t   _cooldownEnd;
    uint32_t   _lastBeacon;

public:
    MalusTotem()
        : _state(MALUS_READY), _cooldownSecs(DEFAULT_COOLDOWN_SECS),
          _cooldownEnd(0), _lastBeacon(0) {}

    void onActivate(const LightAir_TotemActivation& info,
                    LightAir_TotemOutput& out) override {
        _state        = MALUS_READY;
        _cooldownSecs = info.hasConfigSecs ? info.configSecs : DEFAULT_COOLDOWN_SECS;
        _cooldownEnd  = 0;
        _lastBeacon   = 0;
        out.ui.trigger(TotemUIEvent::Active, 180, 0, 0);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        if (_state != MALUS_READY)               return;
        if (msg.msgType != MSG_MALUS_BEACON + 1) return;
        // Player claimed the malus.
        _state       = MALUS_COOLDOWN;
        _cooldownEnd = millis() + (uint32_t)_cooldownSecs * 1000;
        out.ui.trigger(TotemUIEvent::Malus);
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();

        if (_state == MALUS_COOLDOWN) {
            if (now >= _cooldownEnd) {
                _state = MALUS_READY;
                out.ui.trigger(TotemUIEvent::Active, 180, 0, 0);
            }
            return;
        }

        // MALUS_READY: beacon periodically.
        if ((now - _lastBeacon) >= MALUS_BEACON_INTERVAL_MS) {
            _lastBeacon = now;
            uint8_t pl[1] = { 0 };   // 0 = ready
            out.radio.broadcast(MSG_MALUS_BEACON, pl, 1);
        }
    }

    void reset() override {
        _state        = MALUS_READY;
        _cooldownSecs = DEFAULT_COOLDOWN_SECS;
        _cooldownEnd  = 0;
        _lastBeacon   = 0;
    }
};

// ---- Singleton ----
static MalusTotem s_malus;

LightAir_TotemRunner* totemRunner_malus = &s_malus;
