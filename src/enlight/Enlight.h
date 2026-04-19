#pragma once
#include "../config.h"
#include "../nvs_config.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdint.h>
#include <stddef.h>
#include <math.h>

// Fixed hardware constants
static constexpr size_t   ENLIGHT_SPI_MAX_DMA_LEN = 32767;
static constexpr uint32_t ADC_BYTES_PER_CONV = 2;
static constexpr uint32_t ADC_PIPELINE_DELAY = 1;
static constexpr uint32_t ADC_CHANNELS       = 3;
static constexpr uint32_t ADC_CLKS_PER_CONV  = 16;
static constexpr uint32_t PDM_CLKS_PER_BYTE  = 4;

// GOERTZ_GRAIN: period_clocks must be a multiple of ADC_CLKS_PER_CONV*ADC_CHANNELS=48
// so that every period contains an exact integer number of R/G/B ADC triples.
//
// Additionally GOERTZ_PERIOD (_goertzPeriod) must be divisible by 4 so that
// _cosOffset = GOERTZ_PERIOD/4 is an exact integer and sine/cosine remain
// orthogonal. begin() logs an error if this condition is violated. Ensure
// ledFreqHz and ledClockHz are chosen accordingly.
static constexpr uint32_t GOERTZ_GRAIN = ADC_CLKS_PER_CONV * ADC_CHANNELS; // 48

static constexpr float    PDM_AMPLITUDE = 0.95f;
static constexpr int32_t  SIN_MAG       = 2048;
static constexpr int      GRID_MAX_THRESH = CALIB_MAX_PLAYERS * 2;

// Result type
enum class EnlightStatus : uint8_t {
    IDLE        = 0,  // no run() issued since last poll()
    RUNNING     = 1,  // DMA cycles in progress
    LOW_POW     = 2,  // rawsum below limpow -- no target
    NO_HIT      = 3,  // power OK, no far hit-box matched
    PLAYER_HIT  = 4,  // far target; id = player index (1-based)
    NEAR        = 5,  // near object; id = near-target colour id
                      //   (near hit-box grid not yet defined; id = 0 for now)
    COOLDOWN    = 6,  // cooldown period after a hit
};

struct EnlightResult {
    EnlightStatus status;
    uint8_t       id;  // player index (PLAYER_HIT),
                        // colour id (NEAR, 0 until near grid defined)
    EnlightResult() : status(EnlightStatus::IDLE), id(0) {}
    EnlightResult(EnlightStatus s, uint8_t i) : status(s), id(i) {}
};

// Raw correlator output after a completed run — exposed for calibration routines.
struct EnlightRawMeasure {
    long long rout, gout, bout;    // far  correlator sums (signed)
    long long rnear, gnear, bnear; // near correlator sums (signed)
    uint32_t  satCount;            // saturated triples in this run
    uint32_t  totalSamples;        // total triples processed (_arrayiter)
};

// Grid classifier: O(log N) lookup in non-overlapping (outr, outang) boxes
struct GridClassifier {
    float   xThresh[GRID_MAX_THRESH];
    float   yThresh[GRID_MAX_THRESH];
    int     nX, nY;
    uint8_t table[GRID_MAX_THRESH + 1][GRID_MAX_THRESH + 1];
};

class Enlight
{
public:
    Enlight(const EnlightConfig& cfg, const EnlightCalib& cal);
    ~Enlight();

    // One-time hardware init, waveform generation, classifier build.
    // Computes _periodsPerCycle = max full periods in one DMA transaction and
    // sizes all buffers accordingly. Logs cycle duration at INFO level.
    bool begin();

    float    actualFreqHz()    const { return _actualFreqHz;    }
    uint32_t goertzPeriod()    const { return _goertzPeriod;    }
    uint32_t periodsPerCycle() const { return _periodsPerCycle; }

    // True while a run() is in progress or its result has not yet been consumed by poll().
    // Non-destructive: does not clear the result. Use this for loop conditions.
    bool isActive() const { return _active; }

    // Set cooldown time, in milliseconds
    void setCooldown(int64_t ms) { _cooldown = ms * 1000; }

    // Set repetitions  = number of DMA cycles before classify.
    // 1 cycle = _periodsPerCycle sine periods (13 at V6R2 defaults = 7.8 ms).
    void setRepetitions(uint32_t reps) { _repetitions = reps; }
    uint32_t cycleTime() const { return _repetitions*EnlightDefaults::MS_PER_REP; }

    // Non-blocking start
    // Returns false if already running.
    bool run();

    // Poll for result. Call from state machine on every tick.
    //
    // Thread-safe: FreeRTOS spinlock serialises the write (dmaTask, core 0)
    // and the read-clear (poll(), any core).
    // When ready, atomically returns result and resets to IDLE --
    // no separate reset() needed before the next run().
    //
    //   { RUNNING,    0 }  in progress
    //   { LOW_POW,    0 }  no target
    //   { NO_HIT,     0 }  power OK, no box matched
    //   { PLAYER_HIT, p }  far target; p = player index (1-based)
    //   { NEAR,       c }  near object; c = colour id (0 until grid defined)
    //   { IDLE,       0 }  before any run()
    EnlightResult poll();

    // Raw correlator accumulators from the last completed run.
    // Valid to call immediately after poll() returns a non-RUNNING status.
    // Values are reset by run(), so call this before the next run().
    EnlightRawMeasure rawMeasure() const;

    // Access to the raw ADC DMA buffer from the last completed DMA cycle.
    // Only the last cycle is retained; call before the next run().
    // Buffer layout: interleaved 16-bit big-endian values, 12-bit ADC in
    // bits [11:0].  Use ADC_PIPELINE_DELAY and ADC_CHANNELS to decode:
    //   base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY  (for triple index t)
    //   rv = ((buf[base*2] << 8) | buf[base*2+1]) & 0x0FFF
    const uint8_t* rawAdcBuf()        const { return _adcRxBuf;        }
    uint32_t       adcConvsPerCycle() const { return _adcConvsPerCycle; }

    // Raw sine lookup table used by the correlator (length = goertzPeriod()).
    // sintab[t % goertzPeriod()] is the far-kernel weight for triple t.
    // Cosine (near) kernel weight: sintab[(t % goertzPeriod() + cosOffset) % goertzPeriod()].
    // Valid after begin() / buildSintab(); nullptr if allocation failed.
    const int32_t* rawSintab() const { return _sintab; }

    // Rebuild the sine/cosine lookup table with the given phase offset.
    // Also precomputes _sin2total and reallocates _satPhaseCount.
    // Safe to call outside of an active run().
    void buildSintab(uint32_t phase);

private:
    EnlightConfig   _cfg;
    EnlightCalib    _cal;

    // Waveform / cycle geometry
    uint32_t    _periodClocks    = 0;
    uint32_t    _waveformBytes   = 0;
    uint32_t    _goertzPeriod    = 0;
    float       _actualFreqHz    = 0.0f;
    uint32_t    _periodsPerCycle  = 0;
    uint32_t    _adcConvsPerCycle = 0;

    //Cooldown
    int64_t    _cooldown           = 0;
    int64_t    _cooldownStart      = 0;

    //Repetitions
    uint32_t    _repetitions        = 10;

    // Correlator kernel. Cosine = sintab[(idx+_cosOffset)%_goertzPeriod]; no second array.
    int32_t*    _sintab    = nullptr;
    uint32_t    _cosOffset = 0;

    // Per-phase saturation counter.
    // _satPhaseCount[j] is incremented whenever a triple at phase j = (_arrayiter % GP)
    // hits SAT_HIGH or SAT_LOW on any channel.  Maximum value per phase per run() call is
    // repetitions × _periodsPerCycle (≤ 13 per DMA cycle at V6R2 defaults); uint16_t is
    // sufficient for up to ~5000 repetitions before overflow.
    //
    // A single array is enough for both far and near corrections because classify() weights
    // it differently for each channel:
    //   far  correction ← Σ_j satPhaseCount[j] × sintab[j]²          (sin² weight)
    //   near correction ← Σ_j satPhaseCount[j] × sintab[(j+cosOffset)%GP]²  (cos² weight)
    // The squared-kernel weighting provides automatic phase attribution: a saturation at the
    // cosine peak (sin[j] = 0) contributes zero to the far correction and maximum to the near.
    uint16_t*   _satPhaseCount = nullptr;
    long long   _sin2total     = 0;  // Σ_j sintab[j]² = Σ_j cos[j]²; precomputed in buildSintab()

    // LED DIO SPI
    spi_device_handle_t _ledDevice   = nullptr;
    uint8_t*            _ledTxBuf    = nullptr;
    size_t              _ledBufBytes = 0;
    spi_transaction_t   _ledTrans    = {};

    // ADC SPI
    spi_device_handle_t _adcDevice   = nullptr;
    uint8_t*            _adcTxBuf    = nullptr;
    uint8_t*            _adcRxBuf    = nullptr;
    size_t              _adcBufBytes = 0;
    spi_transaction_t   _adcTrans    = {};

    // Correlator accumulators (reset in run(), accumulated per cycle)
    long long   _rout = 0, _gout = 0, _bout = 0;
    long long   _rnear = 0, _gnear = 0, _bnear = 0;
    long long   _rawsum    = 0;
    uint32_t    _arrayiter = 0;
    uint32_t    _satCount  = 0;

    GridClassifier  _grid;

    // Result -- written by dmaTask (core 0), read-cleared by poll() (any core).
    portMUX_TYPE    _mux                = portMUX_INITIALIZER_UNLOCKED;
    EnlightResult   _latestResult       = {};
    volatile bool   _complete           = false;
    bool            _active             = false;  // set by run(), cleared by poll() after result consumed
    bool            _resultDelivered    = false;

    uint32_t        _repsRemaining = 0;
    bool            _firstCycle    = false;  // true on the first DMA cycle after AFE power-on
    TaskHandle_t    _taskHandle    = nullptr;
    struct TaskArgs { Enlight* self; };
    TaskArgs        _taskArgs      = {};

    bool          generateWaveform();
    void          buildGrid();
    int           gridLookup(float outr, float outang) const;
    void          buildAdcTxBuffer();
    void          processAdcCycle();
    EnlightResult classify();
    EnlightResult classifyNear();  // stub: {NEAR,0}; extend when near grid defined
    void          spawnCycle();
    void          onCycleDone();
    static void   dmaTask(void* arg);
};
