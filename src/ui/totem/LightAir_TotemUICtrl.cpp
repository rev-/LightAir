#include "LightAir_TotemUICtrl.h"
#include "../../config.h"

// Strip animation period defaults (ms for one motion cycle), used when a
// role doesn't override the tempo via cmd.periodMs.
namespace {
constexpr uint16_t kStatelessPeriod = 2000;
constexpr uint16_t kBaseIdlePeriod  = 1800;
constexpr uint16_t kCPIdlePeriod    = 1500;
constexpr uint16_t kFlagIdlePeriod  = 1800;
constexpr uint16_t kBonusIdlePeriod = 2500;
constexpr uint16_t kMalusIdlePeriod =  400;
constexpr uint16_t kFlagMissPeriod  = 1500;
constexpr uint16_t kControlPeriod   = 1500;
constexpr uint16_t kContestPeriod   =  600;
constexpr uint16_t kRespawnPeriod   = 2000;   // one full lap of the strip

inline uint16_t periodOr(const TotemUICmd& cmd, uint16_t fallback) {
    return cmd.periodMs ? cmd.periodMs : fallback;
}
}  // namespace

// ----------------------------------------------------------------
LightAir_TotemUICtrl::LightAir_TotemUICtrl(LightAir_TotemRGB& rgb,
                                            LightAir_LEDStrip& strip)
    : _rgb(rgb), _strip(strip)
{}

void LightAir_TotemUICtrl::begin() {
    // Stateless marker: single dim center LED, slow blink; RGB off.
    StripAnimation idle = { 60,60,60, StripEffect::Blink, kStatelessPeriod,
                            0,0,0, StripZone::Center, 1 };
    _strip.loop(idle);
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
        case TotemUIEvent::BaseIdle:
        case TotemUIEvent::CPIdle:
        case TotemUIEvent::FlagIdle:
        case TotemUIEvent::BonusIdle:
        case TotemUIEvent::MalusIdle:
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
        case TotemUIEvent::Idle: {
            // Fully-stateless marker: single dim center LED blink, RGB off.
            StripAnimation a = { 60,60,60, StripEffect::Blink, kStatelessPeriod,
                                 0,0,0, StripZone::Center, 1 };
            _strip.loop(a);
            _rgb.off();
            break;
        }

        case TotemUIEvent::BaseIdle: {
            // Breathing perimeter ring; tempo carries the team rhythm.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Pulse,
                                 periodOr(cmd, kBaseIdlePeriod),
                                 0,0,0, StripZone::Perimeter, cmd.pulseCount };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::CPIdle: {
            // Unclaimed control point: single dot roaming the perimeter.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Chase,
                                 periodOr(cmd, kCPIdlePeriod),
                                 0,0,0, StripZone::Perimeter, 0 };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::FlagIdle: {
            // Breathing vertical scan along the rectangle; team tempo.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::VerticalScan,
                                 periodOr(cmd, kFlagIdlePeriod),
                                 0,0,0, StripZone::All, cmd.pulseCount };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::BonusIdle: {
            // Slow, smooth, sparse green twinkle — "good, soft".
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Sparse,
                                 periodOr(cmd, kBonusIdlePeriod),
                                 0,0,0, StripZone::All, 0,
                                 /*density*/ 4, StripPulseStyle::Smooth };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::MalusIdle: {
            // Fast, hard, sparse red flicker — "bad, pointy".
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Sparse,
                                 periodOr(cmd, kMalusIdlePeriod),
                                 0,0,0, StripZone::All, 0,
                                 /*density*/ 4, StripPulseStyle::Hard };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::FlagMissing: {
            // Faint spine "heartbeat" where the flag should be.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Blink,
                                 periodOr(cmd, kFlagMissPeriod),
                                 0,0,0, StripZone::CenterLine, 1 };
            _strip.loop(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::Control: {
            uint8_t r, g, b;
            if (cmd.r == 0xFE) {
                // Slot-based (TotemVM): cmd.g = owner slot 0-15.
                // Slots 0/1 are teams; 2+ map to player id = slot+1.
                // Keeps the colour policy in the renderer so the VM
                // needs no arithmetic (docs/totem-behavior-handshake.md).
                uint8_t slot = cmd.g;
                if (slot < 2) {
                    r = TeamColors::kColors[slot][0];
                    g = TeamColors::kColors[slot][1];
                    b = TeamColors::kColors[slot][2];
                } else {
                    uint8_t pid = (uint8_t)(slot + 1);
                    if (pid >= PlayerDefs::MAX_PLAYER_ID) pid = 0;
                    r = PlayerColors::kColors[pid][0];
                    g = PlayerColors::kColors[pid][1];
                    b = PlayerColors::kColors[pid][2];
                }
            } else if (cmd.r == 0xFF) {
                // Player-based: look up by player ID in cmd.g
                uint8_t pid = (cmd.g < PlayerDefs::MAX_PLAYER_ID) ? cmd.g : 0;
                r = PlayerColors::kColors[pid][0];
                g = PlayerColors::kColors[pid][1];
                b = PlayerColors::kColors[pid][2];
            } else {
                // Team-based: look up by team index in cmd.r
                uint8_t team = (cmd.r < TeamColors::kCount) ? cmd.r : 0;
                r = TeamColors::kColors[team][0];
                g = TeamColors::kColors[team][1];
                b = TeamColors::kColors[team][2];
            }
            // Owned point: the roaming dot "catches" and fills the ring solid.
            StripAnimation a = { r, g, b, StripEffect::Wipe,
                                 periodOr(cmd, kControlPeriod),
                                 0,0,0, StripZone::Perimeter, 0 };
            _strip.loop(a);
            _rgb.set(r, g, b);
            break;
        }

        case TotemUIEvent::ControlContest: {
            StripAnimation a = {
                TeamColors::kColors[0][0], TeamColors::kColors[0][1], TeamColors::kColors[0][2],
                StripEffect::Alternate, kContestPeriod,
                TeamColors::kColors[1][0], TeamColors::kColors[1][1], TeamColors::kColors[1][2],
                StripZone::Perimeter
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
    // For one-shots, durationMs is one motion cycle and pulseCount is the
    // number of cycles to play before the background resumes.
    switch (cmd.event) {
        case TotemUIEvent::Respawn: {
            // One lit LED runs the whole strip once, in the respawning
            // player's colour: unmistakable across the room, and its single
            // travelling dot reads nothing like Base's breathing perimeter
            // idle.  One 2 s lap, matching how long a respawn feels.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::Chase,
                                 kRespawnPeriod,
                                 0,0,0, StripZone::All, /*cycles*/ 1 };
            _strip.play(a);
            // RGB button untouched: it keeps showing whose base this is,
            // which would otherwise stay stuck on the visitor's colour —
            // the base only repaints it when its idle state is re-entered.
            break;
        }

        case TotemUIEvent::FlagTaken: {
            // Frantic vertical scan in the picking player's colour. 4 @250ms.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::VerticalScan, 250,
                                 0,0,0, StripZone::All, /*cycles*/ 4 };
            _strip.play(a);
            _rgb.set(cmd.r, cmd.g, cmd.b);
            break;
        }

        case TotemUIEvent::FlagReturn: {
            // Single fast vertical scan; RGB button deliberately left unchanged
            // so it keeps showing the flag's steady idle colour.
            StripAnimation a = { cmd.r, cmd.g, cmd.b, StripEffect::VerticalScan, 600,
                                 0,0,0, StripZone::All, /*cycles*/ 0 };
            _strip.play(a);
            // (no _rgb.set — button untouched)
            break;
        }

        case TotemUIEvent::Bonus: {
            // Bright green sparkle burst — dense, smooth, fast. 4 @250ms.
            StripAnimation a = { 0,255,0, StripEffect::Sparse, 250,
                                 0,0,0, StripZone::All, /*cycles*/ 4,
                                 /*density*/ 1, StripPulseStyle::Smooth };
            _strip.play(a);
            _rgb.set(0, 255, 0);
            break;
        }

        case TotemUIEvent::Malus: {
            // Frantic red flicker burst — dense, hard, very fast. 6 @150ms.
            StripAnimation a = { 255,0,0, StripEffect::Sparse, 150,
                                 0,0,0, StripZone::All, /*cycles*/ 6,
                                 /*density*/ 1, StripPulseStyle::Hard };
            _strip.play(a);
            _rgb.set(255, 0, 0);
            break;
        }

        case TotemUIEvent::Roster: {
            _strip.stopLoop();
            StripAnimation a = { 255,255,255, StripEffect::Fill, 500 };
            _strip.play(a);
            _rgb.set(255, 255, 255);
            break;
        }

        // Custom events: callers set r/g/b; generic pulse.
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
