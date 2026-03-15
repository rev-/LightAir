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
static constexpr size_t   SPI_MAX_DMA_LEN    = 32767;
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
static constexpr int32_t  SIN_MAG       = 512;
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
};

struct EnlightResult {
    EnlightStatus status = EnlightStatus::IDLE;
    uint8_t       id     = 0;  // player index (PLAYER_HIT),
                                // colour id (NEAR, 0 until near grid defined)
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

    // Non-blocking start. repetitions = number of DMA cycles before classify.
    // 1 cycle = _periodsPerCycle sine periods (13 at V6R2 defaults = 7.8 ms).
    // Returns false if already running.
    bool run(uint32_t repetitions);

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

    // Correlator kernel. Cosine = sintab[(idx+_cosOffset)%_goertzPeriod]; no second array.
    int32_t*    _sintab    = nullptr;
    uint32_t    _cosOffset = 0;

    // Per-phase saturation counters, one per kernel source (far = sine LED, near = cosine LED).
    // Kept separate because saturation from one source does not necessarily contaminate the other:
    // e.g. a very close reflector that clips the ADC at the cosine peak leaves the sine kernel
    // (ks ≈ 0 at that phase) unaffected, so we can still accumulate the sample for the far channel.
    //
    //   _satCountFar[j]   indexed by sine  phase  idx   = _arrayiter % GP
    //   _satCountNear[j]  indexed by cosine phase  idx_c = (idx + _cosOffset) % GP
    //
    // Both use the same sin²-weighted correction formula in classify() because each array is
    // indexed by the respective kernel's own phase coordinate.
    uint32_t*   _satCountFar  = nullptr;
    uint32_t*   _satCountNear = nullptr;
    long long   _sin2total    = 0;  // Σ_j sintab[j]²; precomputed in buildSintab()

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
    portMUX_TYPE    _mux          = portMUX_INITIALIZER_UNLOCKED;
    EnlightResult   _latestResult = {};
    volatile bool   _complete     = false;
    bool            _active       = false;  // set by run(), cleared by poll() after result consumed

    uint32_t        _repsRemaining = 0;
    TaskHandle_t    _taskHandle    = nullptr;
    struct TaskArgs { Enlight* self; };
    TaskArgs        _taskArgs      = {};

    bool          generateWaveform();
    void          buildSintab(uint32_t phase);
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
