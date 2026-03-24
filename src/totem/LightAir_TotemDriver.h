#pragma once
#include "../radio/LightAir_Radio.h"
#include "LightAir_TotemOutput.h"
#include "../ui/totem/LightAir_TotemUICtrl.h"
#include "../config.h"
#include "LightAir_TotemRoleManager.h"

// ----------------------------------------------------------------
// LightAir_TotemDriver — main loop driver for a totem device.
//
// A totem is a passive game object (respawn base, flag, control
// point) that:
//   1. Periodically broadcasts MSG_TOTEM_BEACON (0xF0) via
//      broadcastUniversal() so players can detect it by RSSI.
//   2. Starts IDLE (typeId = UNIVERSAL): accepts all packets.
//   3. On the first incoming 0xF1 activation reply, looks up the
//      roleId (payload[0]) in the role registry and activates the
//      matching runner via onActivate().
//   4. Forwards every subsequent game-type-matching packet to
//      runner->onMessage().  Calls runner->update() every tick.
//   5. On MSG_TOTEM_ROSTER (universal): calls runner->onRoster(), then
//      runner->reset() and returns to IDLE (typeId = UNIVERSAL).
//
// Lifecycle:
//   LightAir_TotemDriver driver(radio, ui, roleMgr);
//   driver.begin();
//   loop() { driver.loop(); }
//
// The same physical firmware image handles every game role
// (base, flag, CP): the correct runner is looked up at runtime.
// ----------------------------------------------------------------
class LightAir_TotemDriver {
public:
    LightAir_TotemDriver(LightAir_Radio&            radio,
                         LightAir_TotemUICtrl&      ui,
                         LightAir_TotemRoleManager& roleMgr);

    // Calls radio.begin() and triggers the Idle background animation.
    bool begin();

    // One full loop tick: poll radio, handle events, advance animations.
    // Call from Arduino loop() with no delay.
    void loop();

private:
    LightAir_Radio&            _radio;
    LightAir_TotemUICtrl&      _ui;
    LightAir_TotemRoleManager& _roleMgr;

    LightAir_TotemRunner* _runner;      // nullptr = IDLE
    uint32_t              _lastBeacon;  // millis() of last beacon broadcast

    // Flush all queued radio and UI commands to the hardware.
    void flushOutput(LightAir_TotemOutput& out);
};
