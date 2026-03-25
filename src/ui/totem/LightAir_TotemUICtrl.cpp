#include "LightAir_TotemUICtrl.h"

// ---- Totem team colours ----------------------------------------------------------------
static constexpr uint8_t kTeamO_R = 255, kTeamO_G =  80, kTeamO_B =   0;  // orange
static constexpr uint8_t kTeamX_R =   0, kTeamX_G =  80, kTeamX_B = 255;  // blue

// ---- Strip animation presets -----------------------------------------------------------
static const StripAnimation kAnimOff        = { 0,0,0, StripEffect::Off,       0 };
static const StripAnimation kAnimIdlePulse  = { 0,30,60, StripEffect::Pulse,  2000 };
static const StripAnimation kAnimWhiteFill  = { 255,255,255, StripEffect::Fill, 500 };
static const StripAnimation kAnimGreenPulse = { 0,255,0, StripEffect::Pulse,   400 };
static const StripAnimation kAnimRedPulse   = { 255,0,0, StripEffect::Pulse,   400 };

// ----------------------------------------------------------------
LightAir_TotemUICtrl::LightAir_TotemUICtrl(LightAir_TotemRGB& rgb,
                                            LightAir_LEDStrip& strip)
    : _rgb(rgb), _strip(strip)
{}

void LightAir_TotemUICtrl::begin() {
    _strip.loop(kAnimIdlePulse);
    _rgb.off();
}

// ----------------------------------------------------------------
void LightAir_TotemUICtrl::apply(const TotemUIOutput& output) {
    for (uint8_t i = 0; i < output.count; i++) {
        const TotemUICmd& cmd = output.cmds[i];
        if (isBackground(cmd.event))
            dispatchBackground(cmd);
        else
            dispatchOneShot(cmd);
    }
}

void LightAir_TotemUICtrl::update() {
    _strip.update();
}

// ----------------------------------------------------------------
bool LightAir_TotemUICtrl::isBackground(TotemUIEvent ev) const {
    switch (ev) {
        case TotemUIEvent::Idle:
        case TotemUIEvent::FlagMissing:
        case TotemUIEvent::ControlO:
        case TotemUIEvent::ControlX:
        case TotemUIEvent::ControlContest:
            return true;
        default:
            return false;
    }
}

// ----------------------------------------------------------------
void LightAir_TotemUICtrl::dispatchBackground(const TotemUICmd& cmd) {
    switch (cmd.event) {
        case TotemUIEvent::Idle:
            _strip.loop(kAnimIdlePulse);
            if (cmd.r > 0 || cmd.g > 0 || cmd.b > 0)
                _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            else
                _rgb.off();
            break;

        case TotemUIEvent::FlagMissing: {
            // Slow blink in the flag team's colour
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Blink, 1200 };
            _strip.loop(a);
            _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            break;
        }

        case TotemUIEvent::ControlO: {
            StripAnimation a = { kTeamO_R, kTeamO_G, kTeamO_B, StripEffect::Fill, 0 };
            _strip.loop(a);
            _rgb.set(true, false, false);  // approximate orange: R only
            break;
        }

        case TotemUIEvent::ControlX: {
            StripAnimation a = { kTeamX_R, kTeamX_G, kTeamX_B, StripEffect::Fill, 0 };
            _strip.loop(a);
            _rgb.set(false, false, true);
            break;
        }

        case TotemUIEvent::ControlContest: {
            StripAnimation a = {
                kTeamO_R, kTeamO_G, kTeamO_B,
                StripEffect::Alternate, 600,
                kTeamX_R, kTeamX_G, kTeamX_B
            };
            _strip.loop(a);
            _rgb.set(true, false, true);  // magenta = both teams
            break;
        }

        default:
            break;
    }
}

// ----------------------------------------------------------------
void LightAir_TotemUICtrl::dispatchOneShot(const TotemUICmd& cmd) {
    switch (cmd.event) {
        case TotemUIEvent::Respawn: {
            // Wipe in the player's colour (~50 ms × numLeds, total ≤ 1 s)
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Wipe, 0 };
            _strip.play(a);
            _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            break;
        }

        case TotemUIEvent::FlagTaken: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::BlinkFast, 800 };
            _strip.play(a);
            _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            break;
        }

        case TotemUIEvent::FlagReturn: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Fill, 600 };
            _strip.play(a);
            _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            break;
        }

        case TotemUIEvent::Bonus: {
            _strip.play(kAnimGreenPulse);
            _rgb.set(false, true, false);
            break;
        }

        case TotemUIEvent::Malus: {
            _strip.play(kAnimRedPulse);
            _rgb.set(true, false, false);
            break;
        }

        case TotemUIEvent::Roster: {
            // Brief white fill then off — handled by stopLoop + play
            _strip.stopLoop();
            _strip.play(kAnimWhiteFill);
            _rgb.set(true, true, true);
            break;
        }

        // Custom events: callers set r/g/b; use a generic wipe
        case TotemUIEvent::Custom1:
        case TotemUIEvent::Custom2:
        case TotemUIEvent::Custom3:
        case TotemUIEvent::Custom4: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Pulse, 600 };
            _strip.play(a);
            _rgb.set(cmd.r > 0, cmd.g > 0, cmd.b > 0);
            break;
        }

        default:
            break;
    }
}
