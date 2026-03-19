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
//   IDLE  → (first message received) → ACTIVE
//            driver calls reset() then onMessage() with the
//            activation packet, then update() every tick.
//
//   ACTIVE → (MSG_TOTEM_ROSTER received) → IDLE
//            driver calls onRoster() (which queues the reply),
//            then reset() to clear all state.
//
// Role payload convention (activation message, msgType 0xF1):
//   payload[0..1]  = game typeId (uint16_t LE)  — read by driver
//   payload[2]     = role byte — meaning is runner-specific
//   payload[3..]   = optional extra data
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
