#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// TotemUIEvent — semantic events the totem reacts to visually.
//
// One-shot events play once then return to the background.
// Background states loop until replaced by another loop() call.
// ----------------------------------------------------------------
enum class TotemUIEvent : uint8_t {
    // ---- One-shot events ----
    Respawn,       // player respawned here; wipe strip in player colour
    FlagTaken,     // flag picked up from this totem; fast blink
    FlagReturn,    // flag returned to this totem; brief fill
    Bonus,         // bonus awarded; green pulse
    Malus,         // malus imposed; red pulse
    Roster,        // game ended (roster exchange); brief white fill then off
    // ---- Looping background states ----
    Idle,          // fully stateless / unassigned totem — single dim marker LED.
                   //   Only used by the driver before activation / after revert.
    Active,        // role assigned, nothing happening right now; slow pulse in
                   //   cmd colour. Used by every role's idle/ready/neutral state
                   //   so an assigned totem never looks like a stateless one.
    FlagMissing,   // flag away from home; double-blink in flag-team colour
    Control,       // CP owned: steady fill in team or player colour.
                   //   cmd.r = 0 or 1  → team index; colour from TeamColors::kColors.
                   //   cmd.r = 0xFF    → player-based; cmd.g = player ID (0–16);
                   //                    colour from PlayerColors::kColors.
    ControlContest,// contested; alternating team colours (no cmd colour used)
    // ---- Extensibility ----
    Custom1,
    Custom2,
    Custom3,
    Custom4,
};

// ----------------------------------------------------------------
// TotemUICmd — one queued UI command, with optional colour param.
// ----------------------------------------------------------------
struct TotemUICmd {
    TotemUIEvent event;
    uint8_t      r, g, b;  // colour param:
                            //   Respawn       → player RGB colour
                            //   FlagMissing/Return/Taken → flag-team (or player) colour
                            //   Control       → cmd.r = team (0/1) or 0xFF; cmd.g = player ID
                            //   Active        → role-appropriate colour (strip + RGB)
                            //   Idle          → RGB LED colour (0,0,0 = off); strip ignores colour
                            //   others        → ignored (use 0,0,0)
};

// ----------------------------------------------------------------
// TotemUIOutput — output queue for one loop iteration.
// ----------------------------------------------------------------
struct TotemUIOutput {
    static constexpr uint8_t MAX_CMDS = 8;
    TotemUICmd cmds[MAX_CMDS];
    uint8_t    count = 0;

    void trigger(TotemUIEvent ev,
                 uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) {
        if (count >= MAX_CMDS) return;
        cmds[count++] = { ev, r, g, b };
    }
};
