// ================================================================
// LightAir_Totem.ino — firmware for a LightAir totem device.
//
// A totem is a passive game object (respawn base, flag, control
// point) that runs on dedicated hardware — no photodiode, display,
// audio, or vibration required.
//
// Hardware assumed (same PCB as TOTEM_Team0_LED.ino):
//   DATA_PIN  13   WS2812B LED strip data
//   NUM_LEDS  13   strip length
//   PIN_COMM  18   RGB common-cathode/anode enable pin
//   PIN_R      4   RGB red channel
//   PIN_G     19   RGB green channel
//   PIN_B     21   RGB blue channel
//
// NVS keys (written by LightAir_TotemProvisioning.ino):
//   "id"   uint8   — totem logical player-ID (used as beacon senderId)
//   "team" uint8   — 0=O, 1=X (for FLAG and BASE role colour coding)
//   "hw"   uint8   — DeviceHardware::TOTEM (1)
//
// The sketch:
//   1. Loads NVS config.
//   2. Registers all totem roles from AllTotems.cpp.
//   3. Starts LightAir_TotemDriver.
//   4. Calls driver.loop() on every Arduino loop() tick.
//
// No Enlight / display / UI-player / input hardware is used.
// The enlightPtr extern is satisfied with a nullptr stub since
// player-side ruleset code is never called on a totem.
// ================================================================

#include <LightAir.h>
#include "../../src/totem/AllTotems.h"

// Enlight* stub — required by ruleset translation units;
// never dereferenced on a totem (player-side behavior code is
// compiled in but never invoked).
#include "../../src/enlight/Enlight.h"

// ---- Pin definitions ----
static constexpr int PIN_DATA =  13;  // WS2812B data
static constexpr int NUM_LEDS =  13;
static constexpr int PIN_COMM =  18;  // RGB common
static constexpr int PIN_R    =   4;
static constexpr int PIN_G    =  19;
static constexpr int PIN_B    =  21;

// ---- Enlight stub (required by rulesets; never called on totem) ----
Enlight* enlightPtr = nullptr;

// ---- Hardware objects ----
static LightAir_RadioESPNow      transport;
static LightAir_TotemUICtrl      ui;
static LightAir_TotemRoleManager roleMgr;

// Radio is constructed after NVS load (needs playerId).
static LightAir_Radio*           radio  = nullptr;
static LightAir_TotemDriver*     driver = nullptr;

// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // Load NVS calibration (id, team, hardware).
    PlayerConfig cfg;
    player_config_load(cfg);
    Serial.printf("Totem id=%u team=%u\n", cfg.id, cfg.team);

    // Register all totem roles before constructing the driver.
    registerAllTotems(roleMgr);

    // Initialise radio with totem ID.
    static RadioConfig radioCfg;
    radio  = new LightAir_Radio(transport, cfg.id,
                                RadioToken::UNSET, 0, cfg.team, radioCfg);
    driver = new LightAir_TotemDriver(*radio, ui, roleMgr);

    // Initialise totem UI hardware.
    ui.begin(PIN_COMM, PIN_R, PIN_G, PIN_B, PIN_DATA, NUM_LEDS);

    // Start radio and kick off Idle animation.
    if (!driver->begin()) {
        Serial.println("Totem radio init FAILED");
        while (true) delay(1000);
    }

    Serial.println("Totem ready.");
}

// ----------------------------------------------------------------
void loop() {
    driver->loop();
}
