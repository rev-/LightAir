#pragma once
#include "../enlight/Enlight.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"
#include "../game/LightAir_GameHold.h"

// ---------------------------------------------------------------
// EnlightCalibRoutine — three-step hardware calibration sequence, plus a
// summary.
//
// Started from Settings -> Calibration (run()), or from the in-game tools
// menu while a match is on (runHeld(), see LightAir_GameHold.h: the game
// keeps being serviced from every wait loop, the player stays targettable).
// When it ends — saved or aborted — it returns to where it was started
// from; nothing reboots.
//
// All-or-nothing
//   The steps work on a RAM copy of the calibration (_work), seeded from the
//   one Enlight is using.  NVS is written exactly once, when the operator
//   holds TRIG2 on the summary, and the new values are applied to the live
//   Enlight at that same moment.  Until then the device keeps its old
//   calibration on flash: an abort, or a battery that dies mid-routine,
//   leaves it exactly as it was.  (Step 1's phase IS applied live, because
//   steps 2 and 3 have to be measured through it; an abort puts the old
//   phase back.)
//
// Abort
//   Holding B for ABORT_HOLD_MS, at any point — including the automatic
//   50-reading loops and the summary — discards everything measured so far
//   and returns.  The routine then waits for B to be released, so the key
//   cannot leak into whatever screen comes next.
//
// Borrowing the optics
//   The routine owns Enlight while it runs.  On entry it saves the
//   repetitions and cooldown the device was set to, brings it to rest
//   (Enlight::settle(): any run in flight is finished and dropped, the
//   cooldown is skipped) and runs with cooldown 0; on exit it restores both.
//
// Step 1 — Phase offset (clear target in view, at CAL_REF_DIST_M):
//   For each of N_RUNS measurements the user presses TRIG1 to trigger a
//   single Enlight run.  Runs with saturation > SAT_THRESH are discarded
//   (user must press TRIG1 again).  For each valid run the raw ADC buffer
//   is correlated against a shifted kernel at all offsets 0…goertzPeriod-1
//   (full 360° scan), skipping the first period to match processAdcCycle();
//   the offset yielding the maximum sum is recorded as the bestPhase for
//   that shot.  After all shots the median bestPhase is kept and applied.
//   Press TRIG2 to continue.
//
// Step 2 — Baseline ("void", no target):
//   Collect 50 Enlight runs (100 ms apart, no saturation rejection).
//   Display average and stdev of all 6 channels (far + near).
//   The median of each channel becomes the far/near baseline.
//   Press TRIG2 to continue.
//
// Step 3 — White diffusing surface (contact … ~5 m):
//   Illuminate a white diffusing wall or target from varying distances
//   (contact to ~5 m).  Collect 50 runs (100 ms apart, no saturation
//   rejection) and keep the per-channel maxima, near and far, as the
//   "white" thresholds that let the classifier tell a reflective
//   (legitimate) target from a diffusing surface.
//   Press TRIG2 to continue.
//
// Step 4 — Summary:
//   Paged view of every value about to be saved (^ / V).  Hold TRIG2 to
//   save and apply; hold B to discard.
//
// NOTE — the distance estimate is still flawed.  Step 1 takes its shots
// through the phase in use BEFORE calibration, and only then computes the
// new one; step 2 turns those same shots into the reference return
// (refFar*) that anchors Enlight::estimateRangeM().  On a device whose phase
// has drifted — exactly the one being recalibrated — that reference is
// correlated off-phase and comes out low, so the metres reported are biased.
// Fix (deferred): re-correlate the stored step-1 samples with the new phase,
// or take the reference shots after the phase is fixed.
// ---------------------------------------------------------------
class EnlightCalibRoutine : public LightAir_HoldTool {
public:
    EnlightCalibRoutine(Enlight&            e,
                        LightAir_Display&   disp,
                        LightAir_InputCtrl& input,
                        uint8_t             keypadId);

    // Blocking.  Returns true when a new calibration was saved and applied,
    // false when the operator aborted (nothing saved, old values restored).
    bool run();

    // LightAir_HoldTool: the same routine, from inside a running game.
    const char* holdName() const override { return "Calibration"; }
    void runHeld(LightAir_HoldHost& host) override;

    // How long B must be held to abort.
    static constexpr uint32_t ABORT_HOLD_MS = 1500;

private:
    // Called from every wait loop: keeps a running game serviced while the
    // routine blocks.  Nothing to do off-game.
    void idle() { if (_host) _host->service(); }

    // Whole routine: borrow the optics, run the steps, save or restore,
    // give the optics back.
    bool session();

    // --- step implementations; each returns false when aborted ---
    bool step1();
    bool step2();
    bool step3();
    bool step4();  // summary — paged view of the values about to be saved

    // One pass of every wait loop: poll input, call idle(), track the abort
    // hold.  Returns false once the operator has aborted.
    bool tick();

    // Run one Enlight measurement (exactly REPS repetitions) and block until
    // done.  Sets repetitions = REPS before each run so the result is
    // independent of whatever the game had configured.
    // Returns false if saturation rate exceeds SAT_THRESH (run should be
    // discarded and not counted toward the total) — or if the operator
    // aborted, which callers tell apart through _aborted.
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
    // down.  Returns false if the operator aborted meanwhile.
    bool waitTrig(uint8_t trigId);

    // delay() that keeps ticking.  Returns false if aborted meanwhile.
    bool pause(uint32_t ms);

    bool keyDown(char key) const;
    bool buttonDown(uint8_t id) const;

    static void settleIdle(void* self) { static_cast<EnlightCalibRoutine*>(self)->idle(); }

    static constexpr uint32_t N_RUNS     = 50;    // valid runs to collect per step
    static constexpr uint32_t REPS       = 5;     // run() repetitions per measurement
    static constexpr uint32_t DELAY_MS   = 100;   // ms between measurements
    static constexpr float    SAT_THRESH = 0.00f; // discard if any sample is saturated

    Enlight&            _e;
    LightAir_Display&   _disp;
    LightAir_InputCtrl& _input;
    uint8_t             _keypadId;

    LightAir_HoldHost*  _host    = nullptr;  // the game, when run in-game
    const InputReport*  _rep     = nullptr;  // report from the last tick()
    uint32_t            _bDownAt = 0;        // millis() B went down; 0 = up
    bool                _aborted = false;

    EnlightCalib        _orig;   // what Enlight was using when the routine began
    EnlightCalib        _work;   // what the steps are building

    long long           _step1_r[N_RUNS];  // RGB values from step1 for each run
    long long           _step1_g[N_RUNS];
    long long           _step1_b[N_RUNS];
    uint32_t            _step1_n;  // number of runs collected in step1
};
