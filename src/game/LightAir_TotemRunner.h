#pragma once
#include "../radio/LightAir_Radio.h"
#include "LightAir_TotemOutput.h"

// ----------------------------------------------------------------
// LightAir_TotemRunner — abstract base for per-ruleset totem logic.
//
// One subclass is defined per game role that has totem-side
// behaviour (e.g. FlagRunner, BaseRunner, CPRunner).
// Multiple instances of the same subclass may run on different
// totem devices simultaneously.
//
// Lifecycle managed by LightAir_TotemDriver:
//
//   IDLE  → (first game-typeId packet received) → ACTIVE
//            driver calls onMessage() with the activation packet,
//            then update() every tick.
//
//   ACTIVE → (MSG_TOTEM_ROSTER received) → IDLE
//            driver calls onRoster() (which queues the reply),
//            then reset() to clear all state.
//
// Activation protocol — MSG_TOTEM_BEACON reply (msgType 0xF1):
//   The GameRunner infrastructure intercept replies to every
//   MSG_TOTEM_BEACON with a 0xF1 carrying the totem's assigned role:
//
//     payload[0] = 0x80 | totemVarIdx   (named totem)
//     payload[0] = 0xFF                  (generic totem)
//     payload[1] = GenericTotemRoles value  (only when payload[0] == 0xFF)
//
//   High bit of payload[0] is the activation marker, distinguishing
//   this host reply from player respawn replies (which use payload[0]
//   = team+1, always < 0x80).  Runners must check _role == ROLE_UNKNOWN
//   and payload[0] & 0x80 to identify the activation message, then
//   return early without triggering any game interaction.
//
// Implementing a subclass:
//
//   class MyBaseRunner : public LightAir_TotemRunner {
//   public:
//       void onMessage(const RadioPacket& msg,
//                      LightAir_TotemOutput& out) override {
//           out.ui.trigger(TotemUIEvent::Respawn,
//                          playerColour[msg.senderId].r, ...);
//           out.radio.reply(msg);
//       }
//       void reset() override { /* clear counters */ }
//   };
// ----------------------------------------------------------------
class LightAir_TotemRunner {
public:
    virtual ~LightAir_TotemRunner() = default;

    // Called for every incoming game message while ACTIVE.
    // The first call is the activation message.
    // Must not block.
    virtual void onMessage(const RadioPacket& msg,
                           LightAir_TotemOutput& out) = 0;

    // Called when MSG_ROSTER arrives (typeId == 0, universal).
    // Implementation may populate out.radio with a reply carrying
    // aggregated data (e.g. Upkeep CP reports per-team totals).
    // Default: send an empty reply and trigger Roster UI event.
    virtual void onRoster(const RadioPacket& msg,
                          LightAir_TotemOutput& out) {
        out.radio.reply(msg);
        out.ui.trigger(TotemUIEvent::Roster);
    }

    // Called every loop tick while ACTIVE (before output flush).
    // Use for timed actions (periodic beacons, countdowns).
    // Default: no-op.
    virtual void update(LightAir_TotemOutput& out) { (void)out; }

    // Called by the driver after onRoster() completes.
    // Must clear all internal state so the runner is ready to
    // be activated again by the next game session.
    virtual void reset() = 0;
};
