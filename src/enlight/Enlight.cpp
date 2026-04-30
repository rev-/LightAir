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

Enlight::Enlight(const EnlightConfig& cfg, const EnlightCalib& cal) : _cfg(cfg), _cal(cal) {}
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
    if (_cfg.ledFreqHz == 0 || _cfg.ledClockHz == 0) return false;

    const float    idealClocks = (float)_cfg.ledClockHz / (float)_cfg.ledFreqHz;
    const uint32_t grains      = (uint32_t)fmaxf(1.0f, roundf(idealClocks / GOERTZ_GRAIN));
    _periodClocks  = grains * GOERTZ_GRAIN;
    _waveformBytes = _periodClocks / PDM_CLKS_PER_BYTE;
    _goertzPeriod  = _periodClocks / GOERTZ_GRAIN;
    _actualFreqHz  = (float)_cfg.ledClockHz / (float)_periodClocks;

    _periodsPerCycle  = (uint32_t)(ENLIGHT_SPI_MAX_DMA_LEN / _waveformBytes);
    if (_periodsPerCycle == 0) {
        ESP_LOGE(TAG, "Waveform (%lu bytes) exceeds DMA limit", (unsigned long)_waveformBytes);
        return false;
    }
    _ledBufBytes      = _periodsPerCycle * _waveformBytes;
    _adcConvsPerCycle = _periodsPerCycle * (_periodClocks / ADC_CLKS_PER_CONV);
    _adcBufBytes      = (_adcConvsPerCycle + ADC_PIPELINE_DELAY) * ADC_BYTES_PER_CONV;

    const float cycleMs = (float)(_periodsPerCycle * _periodClocks) / (float)_cfg.ledClockHz * 1000.0f;
    ESP_LOGI(TAG, "PDM: req=%.1fHz actual=%.4fHz period=%lu clocks/%lu bytes "
             "GP=%lu perCycle=%lu cycleMs=%.3f pdmOff=%.3f",
             (double)_cfg.ledFreqHz, (double)_actualFreqHz,
             (unsigned long)_periodClocks, (unsigned long)_waveformBytes,
             (unsigned long)_goertzPeriod, (unsigned long)_periodsPerCycle,
             (double)cycleMs, (double)_cfg.pdmAmpOffset);

    _ledTxBuf = (uint8_t*)heap_caps_malloc(_ledBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    _adcTxBuf = (uint8_t*)heap_caps_malloc(_adcBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    _adcRxBuf = (uint8_t*)heap_caps_malloc(_adcBufBytes, MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if (!_ledTxBuf || !_adcTxBuf || !_adcRxBuf) {
        ESP_LOGE(TAG, "DMA alloc failed"); return false;
    }
    memset(_adcRxBuf, 0, _adcBufBytes);

    const float base = 0.5f + _cfg.pdmAmpOffset, swing = 0.5f - _cfg.pdmAmpOffset;
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
    gc.pin_bit_mask=1ULL<<_cfg.afeOn;
    gc.mode=GPIO_MODE_OUTPUT;
    gpio_config(&gc);
    gpio_set_level((gpio_num_t)_cfg.afeOn, 0);

    spi_bus_config_t lb={};
    lb.mosi_io_num=_cfg.ledSdo;
    lb.miso_io_num=_cfg.ledSdiOut;
    lb.sclk_io_num=-1;
    lb.quadwp_io_num=-1;
    lb.quadhd_io_num=-1;
    lb.max_transfer_sz=_ledBufBytes;

    if (spi_bus_initialize((spi_host_device_t)_cfg.ledHost,&lb,SPI_DMA_CH_AUTO)!=ESP_OK) {
        ESP_LOGE(TAG,"LED bus init failed"); return false; }
    spi_device_interface_config_t ld={};
    ld.clock_speed_hz=(int)_cfg.ledClockHz;
    ld.mode=0;
    ld.spics_io_num=-1;
    ld.queue_size=1;
    ld.flags=SPI_DEVICE_HALFDUPLEX;

    if (spi_bus_add_device((spi_host_device_t)_cfg.ledHost,&ld,&_ledDevice)!=ESP_OK) {
        ESP_LOGE(TAG,"LED dev add failed"); return false; }
    const int lp[2]={_cfg.ledSdo,_cfg.ledSdiOut};
    const int ls[2]={(int)spi_periph_signal[(spi_host_device_t)_cfg.ledHost].spid_out,
                     (int)spi_periph_signal[(spi_host_device_t)_cfg.ledHost].spiq_out};
    for (int n=0;n<2;n++) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[lp[n]],PIN_FUNC_GPIO);
        gpio_set_direction((gpio_num_t)lp[n],GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(lp[n],ls[n],true,false);
    }

    spi_bus_config_t ab={};
    ab.mosi_io_num=_cfg.adcSdo;
    ab.miso_io_num=_cfg.adcSdi;
    ab.sclk_io_num=_cfg.adcClk;
    ab.quadwp_io_num=-1;
    ab.quadhd_io_num=-1;
    ab.max_transfer_sz=_adcBufBytes;

    if (spi_bus_initialize((spi_host_device_t)_cfg.adcHost,&ab,SPI_DMA_CH_AUTO)!=ESP_OK) {
        ESP_LOGE(TAG,"ADC bus init failed"); return false; }
    spi_device_interface_config_t ad={};
    ad.clock_speed_hz=(int)_cfg.adcClockHz; ad.mode=0; ad.spics_io_num=_cfg.adcCs; ad.queue_size=1;
    if (spi_bus_add_device((spi_host_device_t)_cfg.adcHost,&ad,&_adcDevice)!=ESP_OK) {
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
    _resultDelivered = false;
    _active=true;
    _firstCycle=true;
    gpio_set_level((gpio_num_t)_cfg.afeOn,1);
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
    const uint8_t cmds[ADC_CHANNELS]={_cfg.adcCmdR,_cfg.adcCmdG,_cfg.adcCmdB};
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
 *   classify() later weights this per-phase count by sin²[j] to correct
 *   the far baseline and by cos²[j] to correct the near baseline.
 *   The squared-kernel weighting provides automatic attribution: a saturation
 *   at the cosine peak (sin[j] = 0) contributes nothing to the far correction.
 * ============================================================ */
void Enlight::processAdcCycle() {
    const uint32_t triples = _adcConvsPerCycle / ADC_CHANNELS;
    for (uint32_t t = 0; t < triples; t++) {
        if (t < _goertzPeriod) { _arrayiter++; continue; }

        const uint32_t base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY;
        auto r12 = [&](uint32_t s) -> uint16_t {
            return (((uint16_t)_adcRxBuf[s*2] << 8) | _adcRxBuf[s*2+1]) & 0x0FFF;
        };
        const uint16_t rv = r12(base), gv = r12(base+1), bv = r12(base+2);
        _rawsum += rv + gv + bv;

        const uint32_t idx = _arrayiter % _goertzPeriod;
        const int32_t  ks  = _sintab[idx];
        const int32_t  kc  = _sintab[(idx + _cosOffset) % _goertzPeriod];

        if (rv >= _cfg.satHigh || rv <= _cfg.satLow ||
            gv >= _cfg.satHigh || gv <= _cfg.satLow ||
            bv >= _cfg.satHigh || bv <= _cfg.satLow) {
            // Record which phase bucket was lost; classify() uses this for baseline correction.
            _satPhaseCount[idx]++;
            _satCount++;
        } else {
            _rout  += (long long)rv*ks; _gout  += (long long)gv*ks; _bout  += (long long)bv*ks;
            _rnear += (long long)rv*kc; _gnear += (long long)gv*kc; _bnear += (long long)bv*kc;
        }
        _arrayiter++;
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
 *
 *   Correction principle
 *   Each sample at phase j contributes to rcal proportionally to sintab[j]²
 *   (kernel power ≈ sin²).  The fraction of baseline energy lost through
 *   excluded samples is therefore:
 *
 *     frac = Σ_j satPhaseCount[j] × sin²[j]  /  (N_per_phase × Σ_j sin²[j])
 *
 *   where N_per_phase = _arrayiter / GP is the number of times each phase
 *   was visited (exact, since each period contains exactly one of each phase).
 *   The corrected baseline is  cal × (1 − frac) = cal − cal × frac.
 *
 *   The SAME _satPhaseCount array is used for both far and near channels,
 *   weighted by sin²[j] and cos²[j] respectively.  Because Σ sin²[j] = Σ cos²[j]
 *   over a full period, the denominator is identical and is precomputed as
 *   _sin2total in buildSintab().
 *
 *   Integer arithmetic (Q16 fixed-point, no float)
 *   -----------------------------------------------
 *   frac_q16 = (sin2_sat << 16) / denom     ∈ [0, 65536]
 *   corr     = (cal × frac_q16 + (1<<15)) >> 16   (rounded)
 *
 *   Overflow check (worst case, all phases always saturated):
 *     sin2_sat   ≤ N_per_phase × _sin2total  →  sin2_sat << 16 fits in int64
 *     cal × frac_q16  ≤ UINT32_MAX × 65536 ≈ 2.8 × 10¹⁴  →  fits in int64
 * ============================================================ */
EnlightResult Enlight::classify() {
    // Accumulate sin²-weighted and cos²-weighted saturation counts.
    long long sin2_sat = 0, cos2_sat = 0;
    if (_satPhaseCount && _sin2total > 0) {
        for (uint32_t j = 0; j < _goertzPeriod; j++) {
            if (!_satPhaseCount[j]) continue;
            const long long sj = _sintab[j];
            const long long cj = _sintab[(j + _cosOffset) % _goertzPeriod];
            sin2_sat += (long long)_satPhaseCount[j] * sj * sj;
            cos2_sat += (long long)_satPhaseCount[j] * cj * cj;
        }
    }

    // denom = N_per_phase × Σ sin²[j]  (total expected sin²-weighted sample count).
    // The first period of every DMA cycle is skipped (photodiode re-settling), so
    // only (_periodsPerCycle - 1) periods per cycle contributed to the accumulators.
    const long long cycles      = (long long)(_arrayiter / (_goertzPeriod * _periodsPerCycle));
    const long long n_per_phase = (long long)(_periodsPerCycle - 1) * cycles;
    const long long denom       = n_per_phase * _sin2total;

    // Q16 correction fractions: 0 = no saturation, 65536 = all samples excluded.
    const long long frac_far_q16  = (denom > 0) ? (sin2_sat << 16) / denom : 0LL;
    const long long frac_near_q16 = (denom > 0) ? (cos2_sat << 16) / denom : 0LL;

    // Corrected baselines: cal - round(cal × frac).  Clamped so they can't go negative.
    // The + (1<<15) term rounds to nearest rather than truncating.
#define CORR(cal, frac_q16) \
    ((long long)(cal) - (((long long)(cal) * (frac_q16) + (1LL<<15)) >> 16))
#define EFF(cal, frac_q16) \
    (CORR(cal, frac_q16) > 0 ? CORR(cal, frac_q16) : 0LL)

    // Scale the single-cycle calibration baselines by the number of cycles so
    // they match the accumulated correlator sums, then apply saturation correction.
    const long long eff_rcal     = EFF(_cal.rcal     * cycles, frac_far_q16);
    const long long eff_gcal     = EFF(_cal.gcal     * cycles, frac_far_q16);
    const long long eff_bcal     = EFF(_cal.bcal     * cycles, frac_far_q16);
    const long long eff_rcalNear = EFF(_cal.rcalNear * cycles, frac_near_q16);
    const long long eff_gcalNear = EFF(_cal.gcalNear * cycles, frac_near_q16);
    const long long eff_bcalNear = EFF(_cal.bcalNear * cycles, frac_near_q16);

#undef EFF
#undef CORR

    const long long rout = (_rout  > eff_rcal)     ? _rout  - eff_rcal     : 0LL;
    const long long gout = (_gout  > eff_gcal)     ? _gout  - eff_gcal     : 0LL;
    const long long bout = (_bout  > eff_bcal)     ? _bout  - eff_bcal     : 0LL;
    _rnear = (_rnear > eff_rcalNear) ? _rnear - eff_rcalNear : 0LL;
    _gnear = (_gnear > eff_gcalNear) ? _gnear - eff_gcalNear : 0LL;
    _bnear = (_bnear > eff_bcalNear) ? _bnear - eff_bcalNear : 0LL;

    ESP_LOGD(TAG, "rawsum=%lld far=(%lld,%lld,%lld) near=(%lld,%lld,%lld) "
                  "sat=%lu frac_far_q16=%lld frac_near_q16=%lld",
             _rawsum, rout, gout, bout, _rnear, _gnear, _bnear,
             (unsigned long)_satCount, frac_far_q16, frac_near_q16);

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
                            configMAX_PRIORITIES-1,&_taskHandle,_cfg.taskCore);
}

void Enlight::onCycleDone() {
    processAdcCycle();
    _repsRemaining--;
    if (_repsRemaining==0) {
        gpio_set_level((gpio_num_t)_cfg.afeOn,0);
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
        while (esp_timer_get_time()-t0 < (int64_t)s->_cfg.afeStartupUs) {}
    }
    spi_device_queue_trans(s->_ledDevice,&s->_ledTrans,portMAX_DELAY);
    spi_device_queue_trans(s->_adcDevice,&s->_adcTrans,portMAX_DELAY);
    spi_transaction_t* r;
    spi_device_get_trans_result(s->_ledDevice,&r,portMAX_DELAY);
    spi_device_get_trans_result(s->_adcDevice,&r,portMAX_DELAY);
    s->onCycleDone();
    vTaskDelete(NULL);
}
