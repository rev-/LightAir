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
    Idle,          // game active, no specific state; gentle slow pulse
    FlagMissing,   // flag away from home; slow blink in flag-team colour
    ControlO,      // control point held by team O; steady fill team-O colour
    ControlX,      // control point held by team X; steady fill team-X colour
    ControlContest,// contested; alternating team colours
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
                            //   FlagMissing/Return/Taken → flag-team colour
                            //   ControlO/X    → team colour
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
