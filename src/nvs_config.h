#pragma once
#include "nvs_flash.h"
#include "nvs.h"
#include <stdint.h>

#define CALIB_NVS_NAMESPACE     "calibration"

// Calibration keys (namespace: "calibration")

// Identification keys
#define CAL_KEY_ID              "id"
#define CAL_KEY_TEAM            "team"

// Calibration for Enlight: near/far channel baselines and white balance factors
#define CAL_KEY_RCAL            "rcal"         // far  channel baselines
#define CAL_KEY_GCAL            "gcal"
#define CAL_KEY_BCAL            "bcal"
#define CAL_KEY_RCAL_NEAR       "rcal_near"    // near channel baselines
#define CAL_KEY_GCAL_NEAR       "gcal_near"
#define CAL_KEY_BCAL_NEAR       "bcal_near"
#define CAL_KEY_LIMPOW          "limpow"       // min rawsum for classification
#define CAL_KEY_RFACT           "rfact"        // white-balance float blob
#define CAL_KEY_BFACT           "bfact"
// nearRatio = (|rnear|+|gnear|+|bnear|) / (|rout|+|gout|+|bout|)
// Far targets  -> ratio ~ constant (optical gain ratio of the two LEDs).
// Near objects -> ratio >> constant (wide-cone LED disproportionately bright).
// If nearRatio > nearRatioMax -> status = NEAR.
// Calibrate at minimum acceptable working distance.
#define CAL_KEY_NEAR_RATIO_MAX  "near_ratio"   // float blob
#define CAL_KEY_PHASE_OFF       "phase_off"    // LED excitation delay in samples
// Step 3 — white diffusing surface (contact … 5 m):
// maximum near and far correlator power seen during the sweep.
// Used to distinguish reflective targets from diffusing surfaces.
#define CAL_KEY_MAX_NEAR_WHITE  "max_near_w"   // Max Near White (uint32)
#define CAL_KEY_MAX_FAR_WHITE   "max_far_w"    // Max Far White  (uint32)
// Far hit-boxes in (outr, outang) space.
// [0]=outr_max [1]=outr_min [2]=outang_max [3]=outang_min
// Key "hb_N_K": N=player(1-based), K=corner 0..3. Sentinel -10 = inactive.
#define CAL_KEY_HITBOX_FMT      "hb_%d_%d"
#define CALIB_MAX_PLAYERS       16

// ---------------------------------------------------------------
// Player identity — stored alongside calibration because both are
// device-specific constants.  id is permanent (set once, never changes
// across games).  team is mutable at runtime but persists across power
// cycles so it doesn't have to be re-entered every session.
//
// Default for both fields when the NVS key is absent: 0xFF (unset).
// ---------------------------------------------------------------
struct PlayerConfig {
    uint8_t id;    // logical player ID (= mycolor); maps to LightAir_Radio playerId
    uint8_t team;  // team assignment; game-defined meaning
};

bool player_config_load(PlayerConfig& cfg);
bool player_config_save(const PlayerConfig& cfg);
bool player_config_save_team(uint8_t team);    // update only team without touching id

struct EnlightCalib {
    uint32_t    rcal, gcal, bcal;             // far  channel baselines
    uint32_t    rcalNear, gcalNear, bcalNear; // near channel baselines
    uint32_t    limpow;
    uint32_t    phaseOff;                     // sintab phase offset (LED excitation delay, in samples)
    float       rfact, bfact;
    float       nearRatioMax;
    float       hitBox[CALIB_MAX_PLAYERS][4]; // sentinel -10 = inactive
    // Step 3 — white diffusing surface calibration
    uint32_t    maxNearWhite; // Max Near White: peak near-channel power over white-wall sweep
    uint32_t    maxFarWhite;  // Max Far White:  peak far-channel  power over white-wall sweep
};

bool enlight_calib_load(EnlightCalib& cal);
bool enlight_calib_save(const EnlightCalib& cal);
