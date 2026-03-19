// ================================================================
// LightAir_TotemProvisioning.ino
//
// One-time NVS setup sketch for a LightAir totem device.
//
// Flash this sketch onto the totem ESP32 to assign its logical
// player-ID, team, and hardware type.  After the sketch runs,
// reflash the device with LightAir_Totem.ino.
//
// Usage:
//   1. Open Serial Monitor at 115200 baud.
//   2. Edit TOTEM_ID and TOTEM_TEAM below to match the desired
//      totem assignment.
//   3. Flash.  The sketch writes to NVS and prints confirmation.
//
// NVS namespace: "lightair"
//   "id"   uint8  — logical player ID used as the beacon senderId
//   "team" uint8  — 0 = team-O, 1 = team-X
//   "hw"   uint8  — DeviceHardware::TOTEM (1)
//
// Totem ID conventions:
//   Player IDs 1–9 are reserved for player blasters.
//   Totem IDs 10–20 avoid collisions with players.
//   ID 10 = Base O1, 11 = Base O2, 12 = Base X1, 13 = Base X2,
//   14 = Flag O,  15 = Flag X,
//   16 = CP1, 17 = CP2, 18 = CP3, 19 = CP4, 20 = CP5
// ================================================================

#include <LightAir.h>

// ---- Edit these before flashing ----
static constexpr uint8_t TOTEM_ID   = 10;   // logical player-ID for this totem
static constexpr uint8_t TOTEM_TEAM =  0;   // 0 = team-O, 1 = team-X

// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=== LightAir Totem Provisioning ===");
    Serial.printf("Writing: id=%u  team=%u  hw=TOTEM\n", TOTEM_ID, TOTEM_TEAM);

    // Write ID and team via existing calibration helpers.
    if (!player_config_save_id(TOTEM_ID)) {
        Serial.println("ERROR: failed to save ID");
    }
    if (!player_config_save_team(TOTEM_TEAM)) {
        Serial.println("ERROR: failed to save team");
    }
    if (!player_config_save_hardware(DeviceHardware::TOTEM)) {
        Serial.println("ERROR: failed to save hardware type");
    }

    // Verify by reading back.
    PlayerConfig cfg;
    player_config_load(cfg);
    Serial.printf("Verified:  id=%u  team=%u  hw=%u\n",
                  cfg.id, cfg.team, (uint8_t)cfg.hardware);

    if (cfg.id       == TOTEM_ID   &&
        cfg.team     == TOTEM_TEAM &&
        cfg.hardware == DeviceHardware::TOTEM) {
        Serial.println("Provisioning OK.  Reflash with LightAir_Totem.ino.");
    } else {
        Serial.println("MISMATCH — check NVS write permissions.");
    }
}

// ----------------------------------------------------------------
void loop() {
    // Nothing to do.
}
