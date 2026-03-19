#pragma once
#include "../radio/LightAir_Radio.h"
#include "../game/LightAir_GameManager.h"
#include "../game/LightAir_TotemOutput.h"
#include "../ui/totem/LightAir_TotemUICtrl.h"
#include "../config.h"

// ----------------------------------------------------------------
// LightAir_TotemDriver — main loop driver for a totem device.
//
// A totem is a passive game object (respawn base, flag, control
// point) that:
//   1. Starts IDLE (typeId = UNIVERSAL): accepts all packets.
//   2. On the first incoming packet whose typeId != UNIVERSAL,
//      looks up the matching LightAir_TotemRunner in the game
//      registry, activates it, and forwards the packet.
//   3. Once active, periodically broadcasts MSG_TOTEM_BEACON (0xF0)
//      as a game-typed packet so only players in the same session
//      receive it (used for RSSI proximity detection).
//   4. Forwards every subsequent game-type-matching packet to
//      runner->onMessage().  Calls runner->update() every tick.
//   5. On MSG_ROSTER (universal): calls runner->onRoster(), then
//      runner->reset() and returns to IDLE (typeId = UNIVERSAL).
//
// Lifecycle:
//   LightAir_TotemDriver driver(radio, manager, ui);
//   driver.begin();
//   loop() { driver.loop(); }
//
// The same physical firmware image handles every game role
// (base, flag, CP): the correct runner is looked up at runtime.
// ----------------------------------------------------------------
class LightAir_TotemDriver {
public:
    LightAir_TotemDriver(LightAir_Radio&       radio,
                         LightAir_GameManager& manager,
                         LightAir_TotemUICtrl& ui);

    // Calls radio.begin() and triggers the Idle background animation.
    bool begin();

    // One full loop tick: poll radio, handle events, advance animations.
    // Call from Arduino loop() with no delay.
    void loop();

private:
    LightAir_Radio&       _radio;
    LightAir_GameManager& _manager;
    LightAir_TotemUICtrl& _ui;

    LightAir_TotemRunner* _runner;      // nullptr = IDLE
    uint32_t              _lastBeacon;  // millis() of last beacon broadcast

    // Find the runner for a given game typeId (nullptr if not found).
    LightAir_TotemRunner* findRunner(uint16_t typeId) const;

    // Flush all queued radio and UI commands to the hardware.
    void flushOutput(LightAir_TotemOutput& out);
};
