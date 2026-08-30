#pragma once
#include "nvs_flash.h"
#include "nvs.h"
#include <stdint.h>
#include "config.h"

#define CALIB_NVS_NAMESPACE     "calibration"

// Calibration keys (namespace: "calibration")

// Identification keys
#define CAL_KEY_ID              "id"
#define CAL_KEY_HARDWARE        "hw"    // DeviceHardware enum stored as uint8

// Calibration for Enlight: near/far channel baselines and white balance factors
#define CAL_KEY_RCAL            "rcal"         // far  channel baselines
#define CAL_KEY_GCAL            "gcal"
#define CAL_KEY_BCAL            "bcal"
#define CAL_KEY_RCAL_NEAR       "rcal_near"    // near channel baselines
#define CAL_KEY_GCAL_NEAR       "gcal_near"
#define CAL_KEY_BCAL_NEAR       "bcal_near"
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
#define CAL_KEY_THRESH_NEAR_R  "thresh_near_r"   // Max Near Red (uint32)
#define CAL_KEY_THRESH_NEAR_G  "thresh_near_g"   // Max Near Green (uint32)
#define CAL_KEY_THRESH_NEAR_B  "thresh_near_b"   // Max Near Blue (uint32)
#define CAL_KEY_THRESH_FAR_R   "thresh_far_r"    // Max Far Red  (uint32)
#define CAL_KEY_THRESH_FAR_G   "thresh_far_g"    // Max Far Green  (uint32)
#define CAL_KEY_THRESH_FAR_B   "thresh_far_b"    // Max Far Blue  (uint32)
// Step 1 — clear-target reference return, per channel, baseline-subtracted and
// normalised per DMA cycle (exactly as thresh_far_* are), captured at
// CAL_KEY_REF_DIST metres.  Retroreflector return falls as 1/x^n, so this one
// measurement fixes the whole curve and lets classify() report an estimated
// distance.  refDist travels with the values, so changing
// EnlightDefaults::CAL_REF_DIST_M does not invalidate an existing calibration.
// All zero = not calibrated: no distance estimate is reported.
#define CAL_KEY_REF_FAR_R      "ref_far_r"       // reference far Red   (uint32)
#define CAL_KEY_REF_FAR_G      "ref_far_g"       // reference far Green (uint32)
#define CAL_KEY_REF_FAR_B      "ref_far_b"       // reference far Blue  (uint32)
#define CAL_KEY_REF_DIST       "ref_dist"        // metres at which the above were taken
#define CALIB_MAX_PLAYERS       16

// ---------------------------------------------------------------
// Player identity — stored alongside calibration because both are
// device-specific constants.  id is permanent (set once, never changes
// across games).  Team is NOT stored in NVS; it is assigned at game-start
// time via the config packet and applied live through LightAir_Radio.
//
// Default for id when the NVS key is absent: 0xFF (unset).
// ---------------------------------------------------------------
struct PlayerConfig {
    uint8_t        id;        // logical player ID (= mycolor); maps to LightAir_Radio playerId
    DeviceHardware hardware;  // PLAYER or TOTEM; default = PLAYER when key absent
};

bool player_config_load(PlayerConfig& cfg);
bool player_config_save(const PlayerConfig& cfg);
bool player_config_save_id(uint8_t id);               // update only id
bool player_config_save_hardware(DeviceHardware hw);  // update only hardware type

struct EnlightCalib {
    uint32_t    rcal, gcal, bcal;             // far  channel baselines
    uint32_t    rcalNear, gcalNear, bcalNear; // near channel baselines
    uint32_t    phaseOff;                     // goertzTab phase offset (LED excitation delay, in samples)
    float       rfact, bfact;
    float       nearRatioMax;
    // Step 3 — per-channel low-power thresholds (white-wall sweep)
    uint32_t    thresh_near_r, thresh_near_g, thresh_near_b;
    uint32_t    thresh_far_r,  thresh_far_g,  thresh_far_b;
    // Step 1 — clear-target reference return at refDistM metres, per DMA cycle.
    // Zero = not calibrated; classify() then reports no distance estimate.
    uint32_t    refFarR, refFarG, refFarB;
    uint8_t     refDistM;
};

bool enlight_calib_load(EnlightCalib& cal);
bool enlight_calib_save(const EnlightCalib& cal);
