#include "Enlight.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "hal/gpio_hal.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "Enlight";

Enlight::Enlight(const EnlightCalib& cal) : _cal(cal) {}
Enlight::~Enlight() {
    heap_caps_free(_ledTxBuf);
    heap_caps_free(_adcTxBuf);
    heap_caps_free(_adcRxBuf);
    heap_caps_free(_sintab);
    heap_caps_free(_satPhaseCount);
}

/* ============================================================
 *   generateWaveform()
 *   1. Round frequency to nearest GOERTZ_GRAIN-multiple period.
 *   2. _periodsPerCycle = floor(ENLIGHT_SPI_MAX_DMA_LEN / waveformBytes).
 *      All buffers are sized once. Cycle duration logged at INFO.
 *   3. Sigma-delta PDM for one period; replicate across DMA buffer.
 *      desired = (0.5+off) + (0.5-off)*PDM_AMP*sin/cos(theta)
 *   4. Fill ADC TX buffer (fixed command stream).
 * ============================================================ */
bool Enlight::generateWaveform() {
    if (EnlightDefaults::LED_FREQ_HZ == 0 || EnlightDefaults::LED_CLOCK_HZ == 0) return false;

    const float    idealClocks = (float)EnlightDefaults::LED_CLOCK_HZ / (float)EnlightDefaults::LED_FREQ_HZ;
    const uint32_t grains      = (uint32_t)fmaxf(1.0f, roundf(idealClocks / GOERTZ_GRAIN));
    _periodClocks  = grains * GOERTZ_GRAIN;
    _waveformBytes = _periodClocks / PDM_CLKS_PER_BYTE;
    _goertzPeriod  = _periodClocks / GOERTZ_GRAIN;
    _actualFreqHz  = (float)EnlightDefaults::LED_CLOCK_HZ / (float)_periodClocks;

    _periodsPerCycle  = (uint32_t)(ENLIGHT_SPI_MAX_DMA_LEN / _waveformBytes);
    if (_periodsPerCycle == 0) {
        ESP_LOGE(TAG, "Waveform (%lu bytes) exceeds DMA limit", (unsigned long)_waveformBytes);
        return false;
    }
    _ledBufBytes      = _periodsPerCycle * _waveformBytes;
    _adcConvsPerCycle = _periodsPerCycle * (_periodClocks / ADC_CLKS_PER_CONV);
    _adcBufBytes      = (_adcConvsPerCycle + ADC_PIPELINE_DELAY) * ADC_BYTES_PER_CONV;

    const float cycleMs = (float)(_periodsPerCycle * _periodClocks) / (float)EnlightDefaults::LED_CLOCK_HZ * 1000.0f;
    ESP_LOGI(TAG, "PDM: req=%.1fHz actual=%.4fHz period=%lu clocks/%lu bytes "
             "GP=%lu perCycle=%lu cycleMs=%.3f pdmOff=%.3f",
             (double)EnlightDefaults::LED_FREQ_HZ, (double)_actualFreqHz,
             (unsigned long)_periodClocks, (unsigned long)_waveformBytes,
             (unsigned long)_goertzPeriod, (unsigned long)_periodsPerCycle,
             (double)cycleMs, (double)EnlightDefaults::PDM_AMP_OFFSET);

    _ledTxBuf = (uint8_t*)heap_caps_malloc(_ledBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    _adcTxBuf = (uint8_t*)heap_caps_malloc(_adcBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    _adcRxBuf = (uint8_t*)heap_caps_malloc(_adcBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if (!_ledTxBuf || !_adcTxBuf || !_adcRxBuf) {
        ESP_LOGE(TAG, "DMA alloc failed"); return false;
    }
    memset(_adcRxBuf, 0, _adcBufBytes);

    const float base = 0.5f + EnlightDefaults::PDM_AMP_OFFSET, swing = 0.5f - EnlightDefaults::PDM_AMP_OFFSET;
    const float twoPiOverT = 2.0f * (float)M_PI / (float)_periodClocks;
    float acc_sin = 0.0f, acc_cos = 0.0f;
    for (uint32_t i = 0; i < _periodClocks; i += PDM_CLKS_PER_BYTE) {
        uint8_t byte = 0;
        for (uint32_t j = 0; j < PDM_CLKS_PER_BYTE; j++) {
            const float theta = twoPiOverT * (float)(i + j);
            const float ds = base + swing * PDM_AMPLITUDE * sinf(theta);
            const float dc = base + swing * PDM_AMPLITUDE * cosf(theta);
            const uint8_t bs = (acc_sin >= 0.5f) ? 1u : 0u;
            const uint8_t bc = (acc_cos >= 0.5f) ? 1u : 0u;
            acc_sin += ds - (float)bs; acc_cos += dc - (float)bc;
            const uint8_t sh = (uint8_t)(6u - j*2u);
            byte |= (uint8_t)(bs << (sh+1u)); byte |= (uint8_t)(bc << sh);
        }
        _ledTxBuf[i / PDM_CLKS_PER_BYTE] = byte;
    }
    for (uint32_t r = 1; r < _periodsPerCycle; r++)
        memcpy(_ledTxBuf + r*_waveformBytes, _ledTxBuf, _waveformBytes);

    buildAdcTxBuffer();
    return true;
}

/* ============================================================
 *   buildSintab()
 *   Cosine kernel uses sintab with offset _cosOffset = GP/4.
 *   REQUIREMENT: _goertzPeriod % 4 == 0 for orthogonality.
 * ============================================================ */
void Enlight::buildSintab(uint32_t phase) {
    heap_caps_free(_sintab);
    if (_goertzPeriod % 4 != 0)
        ESP_LOGE(TAG, "GOERTZ_PERIOD=%lu not divisible by 4 -- near/far broken",
                 (unsigned long)_goertzPeriod);
    _cosOffset = _goertzPeriod / 4;
    _sintab = (int32_t*)heap_caps_malloc(_goertzPeriod*sizeof(int32_t),
                                          MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!_sintab) { ESP_LOGE(TAG, "sintab alloc failed"); return; }
    const float k = 2.0f * (float)M_PI / (float)_goertzPeriod;
    for (uint32_t i = 0; i < _goertzPeriod; i++)
        _sintab[i] = (int32_t)roundf((float)SIN_MAG * sinf(k * (float)(i + phase)));

    // Precompute Σ sintab[j]² = Σ cos[j]² (they are equal over a full period).
    // Used once in classify() as the denominator for both far and near corrections.
    _sin2total = 0;
    for (uint32_t i = 0; i < _goertzPeriod; i++)
        _sin2total += (long long)_sintab[i] * _sintab[i];

    // Allocate the per-phase saturation counter (one array, GP entries, uint16_t).
    // Zeroed here by calloc; zeroed again at the start of every run() via memset.
    heap_caps_free(_satPhaseCount);
    _satPhaseCount = (uint16_t*)heap_caps_calloc(_goertzPeriod, sizeof(uint16_t),
                                                  MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!_satPhaseCount)
        ESP_LOGE(TAG, "satPhaseCount alloc failed");
}

/* ============================================================
 *   buildGrid() + gridLookup()
 * ============================================================ */
static void sort_unique(float* a, int& n) {
    for (int i=1;i<n;i++){float k=a[i];int j=i-1;while(j>=0&&a[j]>k){a[j+1]=a[j];j--;}a[j+1]=k;}
    int o=0; for(int i=0;i<n;i++) if(o==0||a[i]!=a[o-1]) a[o++]=a[i]; n=o;
}
static int upper_bound_f(const float* a, int n, float v) {
    int lo=0,hi=n; while(lo<hi){int m=(lo+hi)/2; if(a[m]<=v)lo=m+1; else hi=m;} return lo;
}

void Enlight::buildGrid() {
    memset(&_grid, 0, sizeof(_grid));
    float xb[GRID_MAX_THRESH], yb[GRID_MAX_THRESH]; int nx=0,ny=0;
    for (int p=1;p<CALIB_MAX_PLAYERS;p++) {
        const float* b=colorBox::colorBox[p]; if(b[0]<=-5.0f) continue;
        if(nx+2<=GRID_MAX_THRESH){xb[nx++]=b[1];xb[nx++]=b[0];}
        if(ny+2<=GRID_MAX_THRESH){yb[ny++]=b[3];yb[ny++]=b[2];}
    }
    sort_unique(xb,nx); sort_unique(yb,ny);
    _grid.nX=nx; _grid.nY=ny;
    memcpy(_grid.xThresh,xb,nx*sizeof(float));
    memcpy(_grid.yThresh,yb,ny*sizeof(float));
    for (int p=1;p<CALIB_MAX_PLAYERS;p++) {
        const float* b=colorBox::colorBox[p]; if(b[0]<=-5.0f) continue;
        _grid.table[upper_bound_f(_grid.xThresh,nx,(b[0]+b[1])*0.5f)]
                   [upper_bound_f(_grid.yThresh,ny,(b[2]+b[3])*0.5f)] = (uint8_t)p;
    }
    ESP_LOGI(TAG, "Grid: %d X thresholds, %d Y thresholds", nx, ny);
}

int Enlight::gridLookup(float outr, float outang) const {
    if (_grid.nX==0||outr<_grid.xThresh[0]||outr>_grid.xThresh[_grid.nX-1]
      ||_grid.nY==0||outang<_grid.yThresh[0]||outang>_grid.yThresh[_grid.nY-1])
        return -1;
    const uint8_t p = _grid.table
        [upper_bound_f(_grid.xThresh,_grid.nX,outr)]
        [upper_bound_f(_grid.yThresh,_grid.nY,outang)];
    return (p>0)?(int)p:-1;
}

/* ============================================================
 *   begin()
 * ============================================================ */
bool Enlight::begin() {
    if (!generateWaveform()) return false;
    buildSintab(_cal.phaseOff); if (!_sintab) return false;
    buildGrid();

    gpio_config_t gc={};
    gc.pin_bit_mask=1ULL<<EnlightDefaults::AFE_ON;
    gc.mode=GPIO_MODE_OUTPUT;
    gpio_config(&gc);
    gpio_set_level((gpio_num_t)EnlightDefaults::AFE_ON, 0);

    spi_bus_config_t lb={};
    lb.mosi_io_num=EnlightDefaults::LED_SDO;
    lb.miso_io_num=EnlightDefaults::LED_SDI_OUT;
    lb.sclk_io_num=-1;
    lb.quadwp_io_num=-1;
    lb.quadhd_io_num=-1;
    lb.max_transfer_sz=_ledBufBytes;

    if (spi_bus_initialize((spi_host_device_t)EnlightDefaults::LED_HOST,&lb,SPI_DMA_CH_AUTO)!=ESP_OK) {
        ESP_LOGE(TAG,"LED bus init failed"); return false; }
    spi_device_interface_config_t ld={};
    ld.clock_speed_hz=(int)EnlightDefaults::LED_CLOCK_HZ;
    ld.mode=0;
    ld.spics_io_num=-1;
    ld.queue_size=1;
    ld.flags=SPI_DEVICE_HALFDUPLEX;

    if (spi_bus_add_device((spi_host_device_t)EnlightDefaults::LED_HOST,&ld,&_ledDevice)!=ESP_OK) {
        ESP_LOGE(TAG,"LED dev add failed"); return false; }
    const int lp[2]={EnlightDefaults::LED_SDO,EnlightDefaults::LED_SDI_OUT};
    const int ls[2]={(int)spi_periph_signal[(spi_host_device_t)EnlightDefaults::LED_HOST].spid_out,
                     (int)spi_periph_signal[(spi_host_device_t)EnlightDefaults::LED_HOST].spiq_out};
    for (int n=0;n<2;n++) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[lp[n]],PIN_FUNC_GPIO);
        gpio_set_direction((gpio_num_t)lp[n],GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(lp[n],ls[n],true,false);
    }

    spi_bus_config_t ab={};
    ab.mosi_io_num=EnlightDefaults::ADC_SDO;
    ab.miso_io_num=EnlightDefaults::ADC_SDI;
    ab.sclk_io_num=EnlightDefaults::ADC_CLK;
    ab.quadwp_io_num=-1;
    ab.quadhd_io_num=-1;
    ab.max_transfer_sz=_adcBufBytes;

    if (spi_bus_initialize((spi_host_device_t)EnlightDefaults::ADC_HOST,&ab,SPI_DMA_CH_AUTO)!=ESP_OK) {
        ESP_LOGE(TAG,"ADC bus init failed"); return false; }
    spi_device_interface_config_t ad={};
    ad.clock_speed_hz=(int)EnlightDefaults::ADC_CLOCK_HZ; ad.mode=0; ad.spics_io_num=EnlightDefaults::ADC_CS; ad.queue_size=1;
    if (spi_bus_add_device((spi_host_device_t)EnlightDefaults::ADC_HOST,&ad,&_adcDevice)!=ESP_OK) {
        ESP_LOGE(TAG,"ADC dev add failed"); return false; }

    memset(&_ledTrans,0,sizeof(_ledTrans));
    _ledTrans.tx_buffer=_ledTxBuf;
    _ledTrans.flags=SPI_TRANS_MODE_DIO;
    _ledTrans.length=_ledBufBytes*8;
    memset(&_adcTrans,0,sizeof(_adcTrans));
    _adcTrans.tx_buffer=_adcTxBuf;
    _adcTrans.rx_buffer=_adcRxBuf;
    _adcTrans.length=_adcBufBytes*8;
    _adcTrans.rxlength=_adcBufBytes*8;

    ESP_LOGI(TAG,"begin() OK  perCycle=%lu  adcConvs=%lu",
             (unsigned long)_periodsPerCycle,(unsigned long)_adcConvsPerCycle);
    return true;
}

/* ============================================================
 *   run()  +  poll()
 * ============================================================ */
bool Enlight::run() {
    if (_active) return false;
    _repsRemaining=_repetitions;
    taskENTER_CRITICAL(&_mux);
    _complete=false;
    _latestResult={};
    taskEXIT_CRITICAL(&_mux);
    _rout=_gout=_bout=_rnear=_gnear=_bnear=_rawsum=0;
    _arrayiter=_satCount=0;
    if (_satPhaseCount) memset(_satPhaseCount, 0, _goertzPeriod * sizeof(uint16_t));
    memset(_satKSum, 0, sizeof(_satKSum));
    memset(_satKValidCount, 0, sizeof(_satKValidCount));
    _resultDelivered = false;
    _active=true;
    _firstCycle=true;
    gpio_set_level((gpio_num_t)EnlightDefaults::AFE_ON,1);
    spawnCycle();
    return true;
}

EnlightRawMeasure Enlight::rawMeasure() const {
    return { _rout, _gout, _bout, _rnear, _gnear, _bnear, _satCount, _arrayiter };
}

EnlightResult Enlight::poll() {
    if (!_active) return {EnlightStatus::IDLE,0};
    if (!_complete) return {EnlightStatus::RUNNING,0};

    if (!_resultDelivered) {
        EnlightResult r;
        taskENTER_CRITICAL(&_mux);
        r=_latestResult;
        _latestResult={};
        taskEXIT_CRITICAL(&_mux);
        if (_cooldown == 0) {
            _active=false;
        } else {
            _resultDelivered=true;
            _cooldownStart=esp_timer_get_time();
        }
        return r;
    }

    if (esp_timer_get_time()-_cooldownStart < _cooldown) return {EnlightStatus::COOLDOWN,0};

    _active=false;
    _resultDelivered=false;
    return {EnlightStatus::IDLE,0};
}

/* ============================================================
 *   buildAdcTxBuffer() -- filled once at begin()
 * ============================================================ */
void Enlight::buildAdcTxBuffer() {
    const uint8_t cmds[ADC_CHANNELS]={EnlightDefaults::ADC_CMD_R,EnlightDefaults::ADC_CMD_G,EnlightDefaults::ADC_CMD_B};
    const uint32_t slots=_adcConvsPerCycle+ADC_PIPELINE_DELAY;
    for (uint32_t i=0;i<slots;i++) {
        _adcTxBuf[i*2]=(i<_adcConvsPerCycle)?cmds[i%ADC_CHANNELS]:0x00;
        _adcTxBuf[i*2+1]=0x00;
    }
}

/* ============================================================
 *   processAdcCycle()
 *   Far  kernel: sintab[idx]
 *   Near kernel: sintab[(idx+_cosOffset)%GP]
 *
 *   First-period skip (per DMA cycle)
 *   ------------------------------------
 *   The ~300-500 us gap between consecutive DMA cycles is long enough for the
 *   photodiode circuit to partially re-settle.  The first sine period of every
 *   DMA cycle is therefore excluded from all accumulators (_rawsum, correlators,
 *   _satPhaseCount).  _arrayiter is still incremented for those triples so that
 *   the phase index (idx = _arrayiter % GP) stays aligned for the remaining
 *   (_periodsPerCycle - 1) periods, which still form an integer multiple of GP,
 *   guaranteeing zero mean for the correlator sums.
 *
 *   Saturation control
 *   ------------------
 *   When any R/G/B channel clips, the triple is excluded from both
 *   accumulators and its phase bucket is recorded in _satPhaseCount[idx].
 *   classify() later weights this per-phase count to correct the near baseline.
 *   
 * ============================================================ */
void Enlight::processAdcCycle() {
    auto r12 = [&](uint32_t s) -> uint16_t {
        return (((uint16_t)_adcRxBuf[s*2] << 8) | _adcRxBuf[s*2+1]) & 0x0FFF;
    };

    const uint32_t triples = _adcConvsPerCycle / ADC_CHANNELS;

    // ── Goertzel correlator ──────────────────────────────────────────────────
    uint32_t cycle_sat = 0;
    for (uint32_t t = 0; t < triples; t++) {
        if (t < _goertzPeriod) { _arrayiter++; continue; }

        const uint32_t base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY;
        const uint16_t rv = r12(base), gv = r12(base+1), bv = r12(base+2);
        _rawsum += rv + gv + bv;

        const uint32_t idx = _arrayiter % _goertzPeriod;
        const int32_t  ks  = _sintab[idx];
        const int32_t  kc  = _sintab[(idx + _cosOffset) % _goertzPeriod];

        if (rv >= EnlightDefaults::SAT_HIGH || rv <= EnlightDefaults::SAT_LOW ||
            gv >= EnlightDefaults::SAT_HIGH || gv <= EnlightDefaults::SAT_LOW ||
            bv >= EnlightDefaults::SAT_HIGH || bv <= EnlightDefaults::SAT_LOW) {
            _satPhaseCount[idx]++;
            _satCount++;
            cycle_sat++;
        } else {
            _rout  += (long long)rv*ks; _gout  += (long long)gv*ks; _bout  += (long long)bv*ks;
            _rnear += (long long)rv*kc; _gnear += (long long)gv*kc; _bnear += (long long)bv*kc;
        }
        _arrayiter++;
    }

    // ── Per-period DC baseline (k) estimation ────────────────────────────────
    // Only runs when this cycle had at least one saturated triple; cycles without
    // saturation do not contribute to (or clear) the accumulators.
    //
    // Signal model: s(x) = k − A·[1−cos(2πx/T)] − B·[1+sin(2πx/T)]
    // where x is measured from the ADC maximum (sintab peak, phaseOff-corrected).
    // At x=0: FAR term = 0, NEAR term = B  →  s(0) = k − B.
    // NEAR is odd about x=0, so:
    //   k = s(0) + [s(−x₀) − s(x₀)] / (2·sin(2π·x₀/GP))
    //
    // x₀: furthest unsaturated symmetric pair within ±GP/4 of the peak.
    // Discard conditions (whole period skipped for all channels):
    //   • any channel saturated at s(0)
    //   • x₀ < 10  (also catches settling period p=0: t0p≈8 < max_x0=50, fails buffer guard)
    if (cycle_sat > 0) {
        const uint32_t t0p    = (_goertzPeriod / 4 + _goertzPeriod - _cal.phaseOff)
                                % _goertzPeriod;
        const uint32_t max_x0 = _goertzPeriod / 4;

        auto ch_sat = [](uint16_t v) -> bool {
            return v >= EnlightDefaults::SAT_HIGH || v <= EnlightDefaults::SAT_LOW;
        };

        for (uint32_t p = 0; p < _periodsPerCycle; p++) {
            const uint32_t tc = p * _goertzPeriod + t0p;
            if (tc < max_x0 || tc + max_x0 >= triples) continue;

            uint16_t s0[ADC_CHANNELS];
            bool bad = false;
            for (int ch = 0; ch < ADC_CHANNELS; ch++) {
                s0[ch] = r12(tc * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch);
                if (ch_sat(s0[ch])) { bad = true; break; }
            }
            if (bad) continue;

            uint32_t x0 = 0;
            for (uint32_t x = 1; x <= max_x0; x++) {
                bool any_sat = false;
                for (int ch = 0; ch < ADC_CHANNELS && !any_sat; ch++) {
                    if (ch_sat(r12((tc - x) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch)) ||
                        ch_sat(r12((tc + x) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch)))
                        any_sat = true;
                }
                if (any_sat) break;
                x0 = x;
            }
            if (x0 < 10) continue;

            const float inv2sin = 1.0f / (2.0f * sinf(2.0f * (float)M_PI
                                                       * (float)x0 / (float)_goertzPeriod));
            for (int ch = 0; ch < ADC_CHANNELS; ch++) {
                const float sm = (float)r12((tc - x0) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch);
                const float sp = (float)r12((tc + x0) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch);
                _satKSum[ch]  += (float)s0[ch] + (sm - sp) * inv2sin;
                _satKValidCount[ch]++;
            }
        }
    }
}

/* ============================================================
 *   classify()
 *
 *   Baseline correction for saturation-excluded samples
 *   ----------------------------------------------------
 *   The stored calibration values (rcal, gcal, …) were measured with ALL
 *   samples present.  When saturated triples are excluded in processAdcCycle()
 *   the accumulators are smaller, so subtracting the full baseline over-
 *   subtracts and produces false zeroes.
 *   Also, the baseline offset is supposed to be canceled in an int number
 *   of periods of the modulator, but the saturation factually removes
 *   some samples, falsifying this hypotesis. A baseline correction is required.
 *
 *   Correction principle
 *   Removal of saturated samples may be modeled as a subtraction from the unsaturated output value:
 *     
 *      R_SAT = R - integral_SAT{[s(x)+v(x)+k]*m(x)}
 *
 *     where:
 *     R_SAT = calculated value for channel R considering saturation (removed samples)
 *     R = calculated value for channel R if saturation wouldn't happen
 *     integral_SAT = sum over the samples that are saturated
 *     s(x) = efficient signal function (cos(x/T*2*pi)-1)*A
 *     v(x) = void signal function (cos(x/T*2*pi)-1)*v
 *     k = baseline channel value, supposed constant
 *     m(x) = modulation signal (cos(x/T*2*pi))*M
 *     
 *   after a bit of calculus and manipulation starting from this equation we get to:
 *
 *   R = [ R_SAT + k* integral_SAT{m(x)} ]/[ 1+ (2/(T*(M^2)))* integral_SAT{m(x)*[M-m(x)]} ] 
 *
 *     where:
 *     T = total number of samples in one sine period
 *     other symbols have the same meaning as above
 *
 *   This basically allows us to estimate the R value starting from the R_SAT information,
 *   that is the actual output value after removal of SAT samples and accumulation with modulation function.
 *   Logically, it is a 2 step compensation:
 *   STEP1 = correct for missing baseline samples : add k* integral_SAT{m(x)}
 *   STEP2 = multiplication factor to cosider missing samples in signal and void : divide by 1+ (2/(T*(M^2)))* integral_SAT{m(x)*[M-m(x)]}
 *
 *   we name the correction addend : c_(R,G,B) = k_(R,G,B) * integral_SAT{m(x)}
 *   we name the correction dividend: gamma = 1+ (2/(T*(M^2)))* integral_SAT{m(x)*[M-m(x)]}
 *
 *   it's worth noting k is channel-dependent, so in general we have separate k_r , k_g , k_b and separate c_R, c_G, c_B
 *   on the other hand, m(x) does not depend on the channel, so gamma is the same value for all the channels.
 *
 *
 * ============================================================ */
EnlightResult Enlight::classify() {
    // Accumulate correction factors.
    // gammaSatCorr = Σ_j satPhaseCount[j] * sintab[j] * (SIN_MAG - sintab[j])
    // cSatCorr     = Σ_j satPhaseCount[j] * sintab[j]
    //
    // gammaF = 1 + (2/(T·M²))·gammaSatCorr  is an exact result (not an approximation).
    // It equals R·gammaF = R_SAT + k·cSatCorr for any saturation pattern.
    // gammaF can legitimately be negative for heavy saturation at negative-sintab phases
    // (> T/4 worst-phase samples saturated); the numerator has matching sign so the
    // ratio still recovers R.  Guard is |gammaF| > threshold to avoid amplifying noise
    // near the singularity (gammaF = 0 ↔ exactly T/4 worst-phase samples saturated).
    long long gammaSatCorr_far = 0, cSatCorr_far = 0;
    long long gammaSatCorr_near = 0, cSatCorr_near = 0;
    if (_satPhaseCount && _sin2total > 0) {
        for (uint32_t j = 0; j < _goertzPeriod; j++) {
            if (!_satPhaseCount[j]) continue;
            const int32_t sj = _sintab[j];
            const int32_t cj = _sintab[(j + _cosOffset) % _goertzPeriod];
            gammaSatCorr_far  += (long long)_satPhaseCount[j] * sj * (SIN_MAG - sj);
            cSatCorr_far      += (long long)_satPhaseCount[j] * sj;
            gammaSatCorr_near += (long long)_satPhaseCount[j] * cj * (SIN_MAG - cj);
            cSatCorr_near     += (long long)_satPhaseCount[j] * cj;
        }
        // Average all valid per-period k estimates into per-channel baselines.
        const float k_R = _satKValidCount[0] ? _satKSum[0] / (float)_satKValidCount[0] : 0.0f;
        const float k_G = _satKValidCount[1] ? _satKSum[1] / (float)_satKValidCount[1] : 0.0f;
        const float k_B = _satKValidCount[2] ? _satKSum[2] / (float)_satKValidCount[2] : 0.0f;
        const float invGammaDenom = 2.0f / ((float)SIN_MAG * (float)SIN_MAG * (float)_goertzPeriod);
        // STEP1: k correction — always apply when saturation exists.
        // Restores the missing DC contribution from saturated samples.
        _rout  = (long long)((float)_rout  + k_R * (float)cSatCorr_far);
        _gout  = (long long)((float)_gout  + k_G * (float)cSatCorr_far);
        _bout  = (long long)((float)_bout  + k_B * (float)cSatCorr_far);
        _rnear = (long long)((float)_rnear + k_R * (float)cSatCorr_near);
        _gnear = (long long)((float)_gnear + k_G * (float)cSatCorr_near);
        _bnear = (long long)((float)_bnear + k_B * (float)cSatCorr_near);
        // STEP2: gamma correction — skip only near the singularity (gammaF ≈ 0).
        // Negative gammaF is valid: numerator and denominator have matching sign.
        const float gammaF_far  = 1.0f + invGammaDenom * (float)gammaSatCorr_far;
        const float gammaF_near = 1.0f + invGammaDenom * (float)gammaSatCorr_near;
        if (fabsf(gammaF_far) > 0.05f) {
            _rout  = (long long)((float)_rout  / gammaF_far);
            _gout  = (long long)((float)_gout  / gammaF_far);
            _bout  = (long long)((float)_bout  / gammaF_far);
        }
        if (fabsf(gammaF_near) > 0.05f) {
            _rnear = (long long)((float)_rnear / gammaF_near);
            _gnear = (long long)((float)_gnear / gammaF_near);
            _bnear = (long long)((float)_bnear / gammaF_near);
        }
    }

    // Scale single-cycle calibration baselines to match the multi-cycle accumulators.
    // The saturation correction above adjusts _rout/_rnear toward the full no-saturation
    // values, so we use the uncorrected (full-cycle) baselines here.
    const long long cycles      = (long long)(_arrayiter / (_goertzPeriod * _periodsPerCycle));
    const long long eff_rcal     = (long long)_cal.rcal     * cycles;
    const long long eff_gcal     = (long long)_cal.gcal     * cycles;
    const long long eff_bcal     = (long long)_cal.bcal     * cycles;
    const long long eff_rcalNear = (long long)_cal.rcalNear * cycles;
    const long long eff_gcalNear = (long long)_cal.gcalNear * cycles;
    const long long eff_bcalNear = (long long)_cal.bcalNear * cycles;

    const long long rout = (_rout  > eff_rcal)     ? _rout  - eff_rcal     : 0LL;
    const long long gout = (_gout  > eff_gcal)     ? _gout  - eff_gcal     : 0LL;
    const long long bout = (_bout  > eff_bcal)     ? _bout  - eff_bcal     : 0LL;
    _rnear = (_rnear > eff_rcalNear) ? _rnear - eff_rcalNear : 0LL;
    _gnear = (_gnear > eff_gcalNear) ? _gnear - eff_gcalNear : 0LL;
    _bnear = (_bnear > eff_bcalNear) ? _bnear - eff_bcalNear : 0LL;

    ESP_LOGD(TAG, "rawsum=%lld far=(%lld,%lld,%lld) near=(%lld,%lld,%lld) sat=%lu",
             _rawsum, rout, gout, bout, _rnear, _gnear, _bnear,
             (unsigned long)_satCount);

    if (_rawsum <= (long long)_cal.limpow) return {EnlightStatus::LOW_POW, 0};

    const float farSum  = (float)((rout)  + (gout)  + (bout));
    const float nearSum = (float)((_rnear) + (_gnear) + (_bnear));
    if (farSum > 0.0f && (nearSum / farSum) > _cal.nearRatioMax) {
        ESP_LOGD(TAG, "NEAR ratio=%.3f", (double)(nearSum / farSum));
        return classifyNear();
    }

    float outr = rout * _cal.rfact, outb = bout * _cal.bfact, outg = (float)gout;
    const float s = outr + outb + outg;
    if (s <= 0.0f) return {EnlightStatus::NO_HIT, 0};
    outr /= s; outg /= s;
    const float outang = (outr < 1.0f) ? (outg / (1.0f - outr)) : 1.0f;
    const int hit = gridLookup(outr, outang);
    if (hit > 0) {
        ESP_LOGI(TAG, "HIT player %d (outr=%.3f outang=%.3f)", hit, outr, outang);
        return {EnlightStatus::PLAYER_HIT, (uint8_t)hit};
    }
    return {EnlightStatus::NO_HIT, 0};
}

/* ============================================================
 *   classifyNear() -- stub
 *   _rnear/_gnear/_bnear are baseline-subtracted and available.
 *   Implement and define the near hit-box grid when needed.
 * ============================================================ */
EnlightResult Enlight::classifyNear() {
    return {EnlightStatus::NEAR,0};
}

/* ============================================================
 *   spawnCycle() / onCycleDone() / dmaTask()
 * ============================================================ */
void Enlight::spawnCycle() {
    _taskArgs={this};
    xTaskCreatePinnedToCore(dmaTask,"EnlightDMA",4096,&_taskArgs,
                            configMAX_PRIORITIES-1,&_taskHandle,EnlightDefaults::TASK_CORE);
}

void Enlight::onCycleDone() {
    processAdcCycle();
    _repsRemaining--;
    if (_repsRemaining==0) {
        gpio_set_level((gpio_num_t)EnlightDefaults::AFE_ON,0);
        EnlightResult r=classify();
        taskENTER_CRITICAL(&_mux);
        _latestResult=r;
        _complete=true;
        taskEXIT_CRITICAL(&_mux);
        return;
    }
    spawnCycle();
}

void Enlight::dmaTask(void* arg) {
    Enlight* s=static_cast<TaskArgs*>(arg)->self;
    if (s->_firstCycle) {
        s->_firstCycle=false;
        const int64_t t0=esp_timer_get_time();
        while (esp_timer_get_time()-t0 < (int64_t)EnlightDefaults::AFE_STARTUP_MICROS) {}
    }
    spi_device_queue_trans(s->_ledDevice,&s->_ledTrans,portMAX_DELAY);
    spi_device_queue_trans(s->_adcDevice,&s->_adcTrans,portMAX_DELAY);
    spi_transaction_t* r;
    spi_device_get_trans_result(s->_ledDevice,&r,portMAX_DELAY);
    spi_device_get_trans_result(s->_adcDevice,&r,portMAX_DELAY);
    s->onCycleDone();
    vTaskDelete(NULL);
}
