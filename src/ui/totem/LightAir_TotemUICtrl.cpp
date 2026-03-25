#include "LightAir_TotemUICtrl.h"
#include "../../config.h"

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
        case TotemUIEvent::Control:
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
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;

        case TotemUIEvent::FlagMissing: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Blink, 1200 };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::Control: {
            uint8_t r, g, b;
            if (cmd.r == 0xFF) {
                // Player-based: look up by player ID in cmd.g
                uint8_t pid = (cmd.g < PlayerDefs::MAX_PLAYER_ID) ? cmd.g : 0;
                r = PlayerColors::kColors[pid][0];
                g = PlayerColors::kColors[pid][1];
                b = PlayerColors::kColors[pid][2];
            } else {
                // Team-based: look up by team index in cmd.r
                uint8_t team = (cmd.r < 2) ? cmd.r : 0;
                r = TeamColors::kColors[team][0];
                g = TeamColors::kColors[team][1];
                b = TeamColors::kColors[team][2];
            }
            StripAnimation a = { r, g, b, StripEffect::Fill, 0 };
            _strip.loop(a);
            _rgb.set(r, g, b);
            break;
        }

        case TotemUIEvent::ControlContest: {
            StripAnimation a = {
                TeamColors::kColors[0][0], TeamColors::kColors[0][1], TeamColors::kColors[0][2],
                StripEffect::Alternate, 600,
                TeamColors::kColors[1][0], TeamColors::kColors[1][1], TeamColors::kColors[1][2]
            };
            _strip.loop(a);
            _rgb.set(255, 255, 255);  // white = contested
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
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Wipe, 0 };
            _strip.play(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::FlagTaken: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::BlinkFast, 800 };
            _strip.play(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::FlagReturn: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Fill, 600 };
            _strip.play(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::Bonus: {
            _strip.play(kAnimGreenPulse);
            _rgb.set(0, 255, 0);
            break;
        }

        case TotemUIEvent::Malus: {
            _strip.play(kAnimRedPulse);
            _rgb.set(255, 0, 0);
            break;
        }

        case TotemUIEvent::Roster: {
            _strip.stopLoop();
            _strip.play(kAnimWhiteFill);
            _rgb.set(255, 255, 255);
            break;
        }

        // Custom events: callers set r/g/b; generic pulse
        case TotemUIEvent::Custom1:
        case TotemUIEvent::Custom2:
        case TotemUIEvent::Custom3:
        case TotemUIEvent::Custom4: {
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Pulse, 600 };
            _strip.play(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        default:
            break;
    }
}
