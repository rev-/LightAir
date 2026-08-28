#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemUIEvent — semantic events the totem reacts to visually.
//
// Each role has a UNIQUE idle background (distinct LED footprint + motion,
// recognizable even as a frozen frame, so colour is never the only cue) and
// its events reuse that footprint family at higher intensity/speed so the
// strip always reads as "this totem" while the variation reads as "this
// just happened".  The concrete (zone, effect, timing) binding for each
// event lives in LightAir_TotemUICtrl.
//
// One-shot events play once then return to the background.  Several in the
// same tick queue up and play in order (see LightAir_LEDStrip::MAX_ONESHOTS)
// rather than overwriting one another.
// Background states loop until replaced by another loop() call.
// ----------------------------------------------------------------
enum class TotemUIEvent : uint8_t {
    // ---- One-shot events ----
    Respawn,       // player respawned here; one LED runs the whole strip once
                   //   (~1 s) in the respawning player's colour
    FlagTaken,     // flag picked up from this totem; frantic vertical scan
    FlagReturn,    // flag returned/scored; single fast vertical scan (button untouched)
    Bonus,         // bonus awarded; bright green sparkle burst
    Malus,         // malus imposed; bright red flicker burst
    Roster,        // game ended (roster exchange); brief white fill

    // ---- Looping background states ----
    Idle,          // fully-stateless / unassigned totem (driver only, before
                   //   activation / after revert): single dim center LED blink,
                   //   RGB off.  NOT used by active roles.
    BaseIdle,      // base ready: breathing perimeter ring, team colour + rhythm.
    CPIdle,        // control point unclaimed: roaming dot on the perimeter, grey.
    FlagIdle,      // flag at home: breathing vertical scan, team colour + rhythm.
    BonusIdle,     // bonus ready: slow smooth green sparkle.
    MalusIdle,     // malus ready: fast hard red flicker (sparse).

    FlagMissing,   // flag away from home: faint spine "heartbeat" in flag colour.
    Control,       // CP owned: perimeter wipe settling to a steady ring.
                   //   cmd.r = 0 or 1  → team index; colour from TeamColors::kColors.
                   //   cmd.r = 0xFF    → player-based; cmd.g = player ID (0–16);
                   //                    colour from PlayerColors::kColors.
    ControlContest,// contested; alternating team colours on the perimeter.

    // ---- Extensibility ----
    Custom1,
    Custom2,
    Custom3,
    Custom4,
};

// ----------------------------------------------------------------
// TotemUICmd — one queued UI command, with optional colour + tempo.
//
// Colour (r,g,b) and tempo (periodMs/pulseCount) tune the fixed footprint
// the controller binds to each event.  periodMs/pulseCount let team-aware
// roles pass their per-team rhythm (see config.h TeamLedRhythm); 0/1 mean
// "use this event's built-in default speed/beat".
// ----------------------------------------------------------------
struct TotemUICmd {
    TotemUIEvent event;
    uint8_t      r, g, b;     // colour param (see per-event docs above)
    uint16_t     periodMs;    // 0 = use the event's built-in default speed
    uint8_t      pulseCount;  // 1 = no extra beat; >1 = per-team beat count
};

// ----------------------------------------------------------------
// TotemUIOutput — output queue for one loop iteration.
// ----------------------------------------------------------------
struct TotemUIOutput {
    static constexpr uint8_t MAX_CMDS = 8;
    TotemUICmd cmds[MAX_CMDS];
    uint8_t    count = 0;

    void trigger(TotemUIEvent ev,
                 uint8_t r = 0, uint8_t g = 0, uint8_t b = 0,
                 uint16_t periodMs = 0, uint8_t pulseCount = 1) {
        if (count >= MAX_CMDS) return;
        cmds[count++] = { ev, r, g, b, periodMs, pulseCount };
    }
};
