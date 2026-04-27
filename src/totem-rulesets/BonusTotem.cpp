#include <LightAir.h>
#include "TotemRoleIds.h"

// ================================================================
// BonusTotem — bonus-pickup role runner (new architecture).
//
// States
//   READY    : broadcasting MSG_BONUS_BEACON every BEACON_INTERVAL_MS.
//              On player reply (0x5F) → COOLDOWN; triggers Bonus anim.
//   COOLDOWN : silent; waits _cooldownSecs seconds then → READY; triggers Idle.
//
// Activation payload
//   payload[0] = roleId (BONUS)
//   payload[1] = cooldown interval in seconds (optional; default 30).
//
// The effect of the bonus is entirely determined by the player side.
// The totem only governs timing: when it is claimable and when it resets.
// ================================================================

using RadioMsg::MSG_BONUS_BEACON;

static constexpr uint8_t  DEFAULT_COOLDOWN_SECS    = 30;
static constexpr uint32_t BONUS_BEACON_INTERVAL_MS = 2000;

enum BonusState : uint8_t { BONUS_READY, BONUS_COOLDOWN };

class BonusTotem : public LightAir_TotemRunner {
    BonusState _state;
    uint8_t    _cooldownSecs;
    uint32_t   _cooldownEnd;
    uint32_t   _lastBeacon;

public:
    BonusTotem()
        : _state(BONUS_READY), _cooldownSecs(DEFAULT_COOLDOWN_SECS),
          _cooldownEnd(0), _lastBeacon(0) {}

    void onActivate(const uint8_t* payload, uint8_t len,
                    LightAir_TotemOutput& out) override {
        _state        = BONUS_READY;
        _cooldownSecs = (len >= 2) ? payload[1] : DEFAULT_COOLDOWN_SECS;
        _cooldownEnd  = 0;
        _lastBeacon   = 0;
        out.ui.trigger(TotemUIEvent::Idle);
    }

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        if (_state != BONUS_READY)               return;
        if (msg.msgType != MSG_BONUS_BEACON + 1) return;
        // Player claimed the bonus.
        _state       = BONUS_COOLDOWN;
        _cooldownEnd = millis() + (uint32_t)_cooldownSecs * 1000;
        out.ui.trigger(TotemUIEvent::Bonus);
    }

    void update(LightAir_TotemOutput& out) override {
        uint32_t now = millis();

        if (_state == BONUS_COOLDOWN) {
            if (now >= _cooldownEnd) {
                _state = BONUS_READY;
                out.ui.trigger(TotemUIEvent::Idle);
            }
            return;
        }

        // BONUS_READY: beacon periodically.
        if ((now - _lastBeacon) >= BONUS_BEACON_INTERVAL_MS) {
            _lastBeacon = now;
            uint8_t pl[1] = { 0 };   // 0 = ready
            out.radio.broadcast(MSG_BONUS_BEACON, pl, 1);
        }
    }

    void reset() override {
        _state        = BONUS_READY;
        _cooldownSecs = DEFAULT_COOLDOWN_SECS;
        _cooldownEnd  = 0;
        _lastBeacon   = 0;
    }
};

// Singleton and runner export moved to CPTotem.cpp.
