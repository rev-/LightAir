#pragma once
#include "Enlight.h"
#include "../ui/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

// ---------------------------------------------------------------
// EnlightCalibRoutine — two-step hardware calibration sequence.
//
// Entered when 'B' is held at power-on (parallel to '^' for DM mode).
// After completion the device proceeds with normal game setup.
//
// Step 1 — Phase offset (clear target in view):
//   Collect 50 valid Enlight runs (100 ms apart).  A run is discarded
//   (and not counted) if its saturation rate exceeds SAT_THRESH (10 %).
//   Display average and standard deviation of rout/gout/bout.
//   Scan all goertzPeriod phase offsets (3 quick runs each) to find the
//   phase that maximises rout+gout+bout.  Save it to NVS and apply it.
//   Press TRIG2 to continue.
//
// Step 2 — Baseline ("void", no target):
//   Collect 50 Enlight runs (100 ms apart, no saturation rejection).
//   Display average and stdev of all 6 channels (far + near).
//   Save the median of each channel as the far/near baseline to NVS.
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

    // Run one Enlight measurement (REPS repetitions) and block until done.
    // Returns false if saturation rate exceeds SAT_THRESH (run should be
    // discarded and not counted toward the total).
    bool runOne(EnlightRawMeasure& out);

    // Scan all phase offsets 0..(goertzPeriod-1), running SCAN_REPS
    // measurements at each.  Returns the offset with the highest total
    // far correlator output (rout+gout+bout).
    uint32_t scanBestPhase();

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
    static constexpr uint32_t SCAN_REPS  = 3;     // measurements per phase during scan
    static constexpr float    SAT_THRESH = 0.10f; // discard if > 10 % saturated
};
