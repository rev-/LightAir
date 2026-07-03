#pragma once
#include "../radio/LightAir_Radio.h"
#include "LightAir_TotemOutput.h"

// ----------------------------------------------------------------
// LightAir_TotemRunner — abstract lifecycle interface for totem
// behaviour.  The single production implementation is
// LightAir_TotemVM, which interprets the behaviour program received
// in the 0xF1 activation reply (docs/totem-behavior-handshake.md).
//
// A totem is stateless (no role, no session token, no typeId) only
// *between* games.  Lifecycle managed by LightAir_TotemDriver:
//
//   IDLE  → (first 0xF1 activation reply received) → ACTIVE
//            driver decodes the reply into a LightAir_TotemActivation,
//            adopts its session token, and calls onActivate(info, out);
//            then onMessage() for subsequent packets, update() every tick.
//
//   ACTIVE → (MSG_TOTEM_ROSTER received, or the self-revert watchdog
//             elapses without one arriving) → IDLE
//            driver calls onRoster() (which queues the reply) when roster
//            arrives, then reset() and clears role/token/typeId either way
//            so the totem is fully stateless again for the next game.
//
// Activation protocol — MSG_TOTEM_BEACON reply (msgType 0xF1):
//   The GameRunner infrastructure (on the host device only) replies to
//   every MSG_TOTEM_BEACON from a configured totem with 0xF1, carrying:
//     - the assigned roleId
//     - the host's current session token (the totem adopts it for the game)
//     - the game's live remaining-time counter, used to arm a self-revert
//       watchdog (~10s margin) in case MSG_TOTEM_ROSTER is never received
//     - optional per-role config (e.g. cooldown seconds for BONUS/MALUS)
//
//   TotemDriver decodes the reply into a LightAir_TotemActivation, loads
//   the TotemVM program it carries, and calls onActivate(info, out).
//   onMessage() is NOT called with the activation packet.
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

// Decoded contents of one 0xF1 activation reply.  Built once by
// LightAir_TotemDriver from the raw packet payload and passed to every
// role's onActivate(), instead of each role re-deriving its own fields
// from raw payload[] indices.
struct LightAir_TotemActivation {
    uint8_t  roleId;
    uint8_t  sessionToken;
    uint16_t gameTimeLeftSecs;   // 0xFFFF = unknown / no watchdog budget
    bool     hasConfigSecs;
    uint8_t  configSecs;        // valid only if hasConfigSecs
};

class LightAir_TotemRunner {
public:
    virtual ~LightAir_TotemRunner() = default;

    // Called once when the activation reply (0xF1) is received with a
    // known roleId.  Use for identity animations and reading per-role
    // config out of info.
    // Default: calls reset() — backward-compatible with legacy runners
    // that do not override this method.
    virtual void onActivate(const LightAir_TotemActivation& info,
                            LightAir_TotemOutput& out) {
        (void)info; (void)out;
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
