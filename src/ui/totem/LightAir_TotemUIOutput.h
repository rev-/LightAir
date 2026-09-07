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
                   //   cmd.r = 0xFE    → slot-based (team CP games): cmd.g = 0/1 is
                   //                    a team index (TeamColors::kColors); cmd.g >= 2
                   //                    is player id = cmd.g + 1 (PlayerColors::kColors).
                   //   cmd.r = 0xFD    → slot-based (teamless CP games): cmd.g is
                   //                    ALWAYS player id - 1, never a team index —
                   //                    player id = cmd.g + 1 unconditionally.  Needed
                   //                    because a teamless game's low player slots
                   //                    (id 1, 2) are numerically indistinguishable
                   //                    from team indices 0/1 under the 0xFE form.
                   //   cmd.r = 0xFF    → player-based; cmd.g = player ID (0–16);
                   //                    colour from PlayerColors::kColors.
    ControlContest,// contested; alternating team colours on the perimeter.
    ControlScore,  // CP paid a point to its current owner: brief sparkle burst in
                   //   the owner's colour (same cmd.r encoding as Control above),
                   //   distinct in shape from Control's steady wipe/fill so "point
                   //   scored" reads as a discrete event over the steady hold colour.

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
