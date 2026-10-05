#pragma once
#include "../radio/LightAir_Radio.h"
#include "LightAir_TotemOutput.h"
#include "../ui/totem/LightAir_TotemUICtrl.h"
#include "../config.h"
#include "LightAir_TotemVM.h"

// ----------------------------------------------------------------
// LightAir_TotemDriver — main loop driver for a totem device.
//
// A totem is a passive game object (respawn base, flag, control
// point) that:
//   1. Periodically broadcasts MSG_TOTEM_BEACON (0xF0) via
//      broadcastUniversal() so players can detect it by RSSI.
//   2. Starts IDLE (typeId = UNIVERSAL): accepts all packets.
//   3. On the first incoming 0xF1 activation reply, loads the
//      TotemVM program it carries and activates the interpreter
//      (docs/totem-behavior-handshake.md).  Totems hold no game
//      files: the whole behaviour arrives in this one packet.
//   4. Forwards every subsequent game-type-matching packet to the
//      VM (RSSI-aware).  Calls update() every tick.
//   5. On MSG_TOTEM_ROSTER (universal): calls runner->onRoster(), then
//      runner->reset() and returns to IDLE (typeId = UNIVERSAL).
//   6. While ACTIVE, answers MSG_TOTEM_TOUCH itself (handleTouch): the
//      program never sees a touch, so it cannot move the role's state.
//
// Lifecycle:
//   LightAir_TotemDriver driver(radio, ui);
//   driver.begin();
//   loop() { driver.loop(); }
//
// The same physical firmware image handles every game role — and
// every role not invented yet: the behaviour is data, not code.
// ----------------------------------------------------------------
class LightAir_TotemDriver {
public:
    LightAir_TotemDriver(LightAir_Radio&       radio,
                         LightAir_TotemUICtrl& ui);

    // Calls radio.begin() and triggers the Idle background animation.
    bool begin();

    // One full loop tick: poll radio, handle events, advance animations.
    // Call from Arduino loop() with no delay.
    void loop();

private:
    LightAir_Radio&       _radio;
    LightAir_TotemUICtrl& _ui;

    LightAir_TotemRunner* _runner;          // nullptr = IDLE
    LightAir_TotemVM      _vm;              // interpreter for over-the-air programs
    uint8_t               _roleId;          // from the activation, for touch replies
    uint32_t              _lastBeacon;      // millis() of last beacon broadcast
    uint32_t              _revertDeadline;  // millis() deadline for self-revert; 0 = no watchdog armed

    // Flush all queued radio and UI commands to the hardware.
    void flushOutput(LightAir_TotemOutput& out);

    // MSG_TOTEM_TOUCH while ACTIVE (config.h).  `animated` is true once a
    // chaser was queued this tick: the strip only learns about it at flush.
    void handleTouch(const RadioEvent& ev, LightAir_TotemOutput& out, bool& animated);

    // Shared teardown: reset()s the runner, clears role/token/typeId, and
    // returns to the Idle animation.  Used both when MSG_TOTEM_ROSTER arrives
    // and when the self-revert watchdog elapses without one.
    void revertToIdle(LightAir_TotemOutput& out);
};
