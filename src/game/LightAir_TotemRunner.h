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
//   The GameRunner infrastructure intercept (on the host device only)
//   replies to every MSG_TOTEM_BEACON from a configured totem with 0xF1:
//
//   New role-registry path (payload[0] = TotemRoleId, not 0xFF):
//     payload[0] = roleId  (TotemRoleId::BASE_O … MALUS)
//     payload[1] = optional per-role config (e.g. cooldown seconds)
//     TotemDriver finds the runner via LightAir_TotemRoleManager and
//     calls runner->onActivate(payload, len, out).  onMessage() is NOT
//     called with the activation packet.
//
//   Legacy path (payload[0] = totemVarIdx or 0xFF):
//     payload[0] = totemVarIdx           (named totem)
//     payload[0] = 0xFF                  (generic totem)
//     payload[1] = GenericTotemRoles value  (only when payload[0] == 0xFF)
//     TotemDriver finds the runner via LightAir_GameManager (typeId lookup)
//     and calls runner->onMessage() — legacy runners handle the 0xF1 there.
//
//   Unconfigured and non-totem senders receive no reply.
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

    // Called once when the activation reply (0xF1) is received with a
    // known roleId.  payload[0] = roleId; payload[1..] = optional per-role
    // config bytes (e.g. cooldown seconds for BONUS/MALUS).
    // Use for identity animations and reading config from the payload.
    // Default: calls reset() — backward-compatible with legacy runners
    // that do not override this method.
    virtual void onActivate(const uint8_t* payload, uint8_t len,
                            LightAir_TotemOutput& out) {
        (void)payload; (void)len; (void)out;
        reset();
    }

    // Called for every incoming game message while ACTIVE.
    // In the new activation path, activation is handled by onActivate()
    // and this method never receives the 0xF1 packet.
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
