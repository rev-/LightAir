// ================================================================
// LightAir_TotemProvisioning.ino
//
// One-time NVS setup sketch for a LightAir totem device.
//
// Flash this sketch onto the totem ESP32 to assign its logical
// totem number and hardware type.  After the sketch runs, reflash
// the device with the unified firmware (LightAir.ino at the repo root).
//
// Usage:
//   1. Open Serial Monitor at 115200 baud.
//   2. Edit TOTEM_NUM below to match the desired totem (1–16,
//      matching the "Totem 01".."Totem 16" labels used elsewhere).
//   3. Flash.  The sketch writes to NVS and prints confirmation.
//
// NVS namespace: "calibration"
//   "id" uint8  — logical totem ID used as the beacon senderId
//   "hw" uint8  — DeviceHardware::TOTEM (1)
//
// Totem ID conventions (see TotemDefs in config.h):
//   Player IDs 1–16 are reserved for player blasters.
//   Totem numbers 1–16 map to logical IDs 254 down to 239
//   (TOTEM_NUM 1 = id 254 = "Totem 01", TOTEM_NUM 16 = id 239 =
//   "Totem 16"), via TotemDefs::idFromIndex(TOTEM_NUM - 1).
//
//   Totem role (BASE_O, BASE_X, CP, FLAG, …) and team are NOT
//   fixed in firmware — they are assigned per-game at game setup,
//   so this sketch only provisions the totem's identity, not its
//   in-game role.
// ================================================================

#include <LightAir.h>

// ---- Edit this before flashing ----
static constexpr uint8_t TOTEM_NUM = 1;   // totem number, 1–16 ("Totem 01".."Totem 16")

static constexpr uint8_t TOTEM_ID = TotemDefs::idFromIndex(TOTEM_NUM - 1);  // logical id, 254..239

// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=== LightAir Totem Provisioning ===");
    Serial.printf("Writing: totem=%u  id=%u  hw=TOTEM\n", TOTEM_NUM, TOTEM_ID);

    // Write ID via existing calibration helpers.
    if (!player_config_save_id(TOTEM_ID)) {
        Serial.println("ERROR: failed to save ID");
    }
    if (!player_config_save_hardware(DeviceHardware::TOTEM)) {
        Serial.println("ERROR: failed to save hardware type");
    }

    // Verify by reading back.
    PlayerConfig cfg;
    player_config_load(cfg);
    Serial.printf("Verified:  id=%u  hw=%u\n", cfg.id, (uint8_t)cfg.hardware);

    if (cfg.id       == TOTEM_ID &&
        cfg.hardware == DeviceHardware::TOTEM) {
        Serial.println("Provisioning OK.  Reflash with the unified LightAir.ino.");
    } else {
        Serial.println("MISMATCH — check NVS write permissions.");
    }
}

// ----------------------------------------------------------------
void loop() {
    // Nothing to do.
}
