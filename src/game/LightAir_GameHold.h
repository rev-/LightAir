#pragma once

// ----------------------------------------------------------------
// In-game hold — a tool borrows the device from a running game.
//
// The player holds A+B; the runner puts the game on HOLD and runs its hold
// tool (the in-game tools menu, which runs whatever tool is picked).  The
// tool owns the screen, the keypad and the optics until it returns.
// Meanwhile the player is busy, not out of the game:
//
//   * other players' messages still reach the ruleset's handlers for the
//     state the player is in — a LIT is taken, answered and scored exactly
//     as it would be, and so is a splash, a team point report or a flag
//     event.  Totem messages are dropped: no pickups, no CP presence, no
//     BASE respawn.  A ruleset can narrow this further (`hold.accept`).
//   * the game's own update body does not run, so nothing is fired, sent or
//     picked up on the player's behalf.  State rules DO run, with an empty
//     InputReport: the player's state follows from what happens to them
//     (shone to zero lives -> out; a respawn clock -> back) but never from
//     what they press.  Declarative clocks (countdown_in) keep running.
//   * infrastructure always runs: END GAME, the end-game score exchange
//     and the host's totem activations.  An END GAME that arrives during a
//     hold does its data work at once (state, transition action, own score
//     out, round-robin answered) and its presentation — end screen,
//     EndGame cue, winner — when the tool returns.
//
// A tool is blocking: it calls host.service() from every wait loop, which
// runs one reduced game cycle (rate-limited to GameDefaults::LOOP_MS, so
// calling it more often costs nothing).  The runner never polls the keypad
// while held — the tool does, and one-shot RELEASED edges must reach it.
// ----------------------------------------------------------------
class LightAir_HoldHost {
public:
    virtual ~LightAir_HoldHost() {}
    virtual void service() = 0;
};

class LightAir_HoldTool {
public:
    virtual ~LightAir_HoldTool() {}
    // Menu label, <= 16 chars.
    virtual const char* holdName() const = 0;
    // Blocking; returns when the tool is done or cancelled.
    virtual void runHeld(LightAir_HoldHost& host) = 0;
};
