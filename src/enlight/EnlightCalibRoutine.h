#pragma once
#include "Enlight.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

// ---------------------------------------------------------------
// EnlightCalibRoutine — three-step hardware calibration sequence.
//
// Entered when 'B' is held at power-on (parallel to '^' for DM mode).
// After completion the device proceeds with normal game setup.
//
// Step 1 — Phase offset (clear target in view):
//   For each of N_RUNS measurements the user presses TRIG1 to trigger a
//   single Enlight run.  Runs with saturation > SAT_THRESH are discarded
//   (user must press TRIG1 again).  For each valid run the raw ADC buffer
//   is correlated against a shifted sine table at offsets 0…goertzPeriod/4
//   (0°…90°); the offset yielding the maximum sum is recorded as the
//   bestPhase for that shot.  After all shots the bestPhase values are
//   sorted and the median is saved to NVS and applied.
//   Press TRIG2 to continue.
//
// Step 2 — Baseline ("void", no target):
//   Collect 50 Enlight runs (100 ms apart, no saturation rejection).
//   Display average and stdev of all 6 channels (far + near).
//   Save the median of each channel as the far/near baseline to NVS.
//   Press TRIG2 to continue.
//
// Step 3 — White diffusing surface (contact … ~5 m):
//   Illuminate a white diffusing wall or target from varying distances
//   (contact to ~5 m).  Collect 50 runs (100 ms apart, no saturation
//   rejection).  For each reading compute:
//     near_pow = |rnear| + |gnear| + |bnear|
//     far_pow  = |rout|  + |gout|  + |bout|
//   Track the maximum near_pow and maximum far_pow seen across all 50
//   readings.  Save them to NVS as "Max Near White" and "Max Far White".
//   These thresholds allow the classifier to distinguish reflective
//   (legitimate) targets from diffusing surfaces.
//   Press TRIG2 to finish.
// ---------------------------------------------------------------
class EnlightCalibRoutine {
public:
    EnlightCalibRoutine(Enlight&            e,
                        LightAir_Display&   disp,
                        LightAir_InputCtrl& input,
                        uint8_t             keypadId);

    void run();  // blocking; executes step 1 then step 2

private:
    // --- step implementations ---
    void step1();
    void step2();
    void step3();
    void step4();  // calibration summary — paged view of all NVS values

    // Run one Enlight measurement (REPS repetitions) and block until done.
    // Returns false if saturation rate exceeds SAT_THRESH (run should be
    // discarded and not counted toward the total).
    bool runOne(EnlightRawMeasure& out);

    // From the raw ADC buffer of the last completed run, correlate each
    // triple against a sine table shifted by p = 0…goertzPeriod/4 and
    // return the offset p that yields the maximum sum.
    uint32_t computeBestPhase();

    // Display up to 6 text rows (y = 0, 10, 20, 30, 40, 50).
    void showLines(const char* l0 = nullptr, const char* l1 = nullptr,
                   const char* l2 = nullptr, const char* l3 = nullptr,
                   const char* l4 = nullptr, const char* l5 = nullptr);

    // Block until the specified trigger button (TRIG_1_ID / TRIG_2_ID) is
    // released.  Polls at ~10 ms intervals.
    void waitTrig(uint8_t trigId);

    Enlight&            _e;
    LightAir_Display&   _disp;
    LightAir_InputCtrl& _input;
    uint8_t             _keypadId;

    static constexpr uint32_t N_RUNS     = 50;    // valid runs to collect per step
    static constexpr uint32_t REPS       = 5;     // run() repetitions per measurement
    static constexpr uint32_t DELAY_MS   = 100;   // ms between measurements
    static constexpr float    SAT_THRESH = 0.10f; // discard if > 10 % saturated
};
