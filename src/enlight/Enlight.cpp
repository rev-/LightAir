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
    heap_caps_free(_goertzTab);
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
    float acc_far = 0.0f, acc_near = 0.0f;
    for (uint32_t i = 0; i < _periodClocks; i += PDM_CLKS_PER_BYTE) {
        uint8_t byte = 0;
        for (uint32_t j = 0; j < PDM_CLKS_PER_BYTE; j++) {
            const float theta = twoPiOverT * (float)(i + j);
            const float d_far  = base + swing * PDM_AMPLITUDE * cosf(theta);
            const float d_near = base + swing * PDM_AMPLITUDE * sinf(theta);
            const uint8_t b_far  = (acc_far  >= 0.5f) ? 1u : 0u;
            const uint8_t b_near = (acc_near >= 0.5f) ? 1u : 0u;
            acc_far  += d_far  - (float)b_far;
            acc_near += d_near - (float)b_near;
            const uint8_t sh = (uint8_t)(6u - j*2u);
            byte |= (uint8_t)(b_far << (sh+1u)); byte |= (uint8_t)(b_near << sh);
        }
        _ledTxBuf[i / PDM_CLKS_PER_BYTE] = byte;
    }
    for (uint32_t r = 1; r < _periodsPerCycle; r++)
        memcpy(_ledTxBuf + r*_waveformBytes, _ledTxBuf, _waveformBytes);

    buildAdcTxBuffer();
    return true;
}

/* ============================================================
 *   buildGoertzTab()
 *   FAR kernel: goertzTab[idx] = KERN_MAG * cos(2π(idx+phase)/GP)
 *   NEAR kernel: goertzTab[(idx + _nearOffset) % GP],  _nearOffset = 3*GP/4
 *   cos(x + 3π/2) = sin(x), so the near kernel is sine — in phase with NEAR LED.
 *   REQUIREMENT: _goertzPeriod % 4 == 0 for orthogonality.
 * ============================================================ */
int32_t Enlight::kernelEntry(uint32_t gp, uint32_t phaseIdx) {
    return (int32_t)roundf((float)KERN_MAG *
                           cosf(2.0f * (float)M_PI * (float)phaseIdx / (float)gp));
}

void Enlight::buildGoertzTab(uint32_t phase) {
    heap_caps_free(_goertzTab);
    if (_goertzPeriod % 4 != 0)
        ESP_LOGE(TAG, "GOERTZ_PERIOD=%lu not divisible by 4 -- near/far broken",
                 (unsigned long)_goertzPeriod);
    _nearOffset = 3 * _goertzPeriod / 4;
    _goertzTab = (int32_t*)heap_caps_malloc(_goertzPeriod*sizeof(int32_t),
                                             MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!_goertzTab) { ESP_LOGE(TAG, "goertzTab alloc failed"); return; }
    for (uint32_t i = 0; i < _goertzPeriod; i++)
        _goertzTab[i] = kernelEntry(_goertzPeriod, i + phase);
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
    buildGoertzTab(_cal.phaseOff); if (!_goertzTab) return false;
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
    _arrayiter=_satCount=_activePeriods=0;
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
 *   Far  kernel: goertzTab[idx]
 *   Near kernel: goertzTab[(idx+_nearOffset)%GP]
 *
 *   First-period skip: period 0 of every DMA cycle is skipped (photodiode
 *   settling after the inter-cycle gap). _arrayiter is still advanced to keep
 *   phase alignment; _activePeriods is not incremented.
 *
 *   Per-period saturation handling — single combined pass:
 *     No saturation (common): Goertzel accumulators receive every sample.
 *     Any saturation: analytic sinusoidal fitting applied to all channels.
 *       Signal model: s(x) = k + A·sin(2πx/GP + α)
 *       The non-saturated arc is symmetric around x0 (argmax) with half-width
 *       deltaXs samples = (GP − w) / 2, where w = i_last − i_first + 1.
 *       Amplitude: A = (2·deltaX_rad·s(x0) − SUM·twoPiOverGP)
 *                      / (2·(deltaX_rad − sin(deltaX_rad)))
 *       Analytic Goertzel: far  += KERN_MAG·GP/2·A·cos(2π·x0/GP)
 *                          near += KERN_MAG·GP/2·A·sin(2π·x0/GP)
 *       Formula is exact at W_sat=0 (deltaX_rad=π, SUM=GP·k → A exact).
 *
 *   Ditch: period discarded if any channel has > SAT_DITCH_FRAC saturated samples.
 * ============================================================ */
void Enlight::processAdcCycle() {
    auto r12 = [&](uint32_t s) -> uint16_t {
        return (((uint16_t)_adcRxBuf[s*2] << 8) | _adcRxBuf[s*2+1]) & 0x0FFF;
    };

    long long rout_c=0, gout_c=0, bout_c=0;
    long long rnear_c=0, gnear_c=0, bnear_c=0;

    const float twoPiOverGP = 2.0f * (float)M_PI / (float)_goertzPeriod;
    const float halfGP_f    = 0.5f * (float)_goertzPeriod;
    const float kern_f      = (float)KERN_MAG;

    for (uint32_t p = 0; p < _periodsPerCycle; p++) {
        const uint32_t t_base = p * _goertzPeriod;

        if (p == 0) {
            _arrayiter += _goertzPeriod;
            continue;
        }

        // Per-channel state for saturation tracking and non-sat accumulation
        uint32_t  w_sat[ADC_CHANNELS]       = {};
        uint32_t  i_first[ADC_CHANNELS]     = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
        uint32_t  i_last[ADC_CHANNELS]      = {};
        uint16_t  argmax_val[ADC_CHANNELS]  = {};
        uint32_t  argmax_idx[ADC_CHANNELS]  = {};
        long long sum_nonsat[ADC_CHANNELS]  = {};
        long long rout_p=0, gout_p=0, bout_p=0;
        long long rnear_p=0, gnear_p=0, bnear_p=0;

        for (uint32_t t = t_base; t < t_base + _goertzPeriod; t++) {
            const uint32_t base = t * ADC_CHANNELS + ADC_PIPELINE_DELAY;
            const uint16_t rv = r12(base), gv = r12(base+1), bv = r12(base+2);
            const uint32_t lt = t - t_base;

            const uint32_t idx = _arrayiter % _goertzPeriod;
            const int32_t  ks  = _goertzTab[idx];
            const int32_t  kc  = _goertzTab[(idx + _nearOffset) % _goertzPeriod];

            const bool sat_r = (rv >= EnlightDefaults::SAT_HIGH || rv <= EnlightDefaults::SAT_LOW);
            const bool sat_g = (gv >= EnlightDefaults::SAT_HIGH || gv <= EnlightDefaults::SAT_LOW);
            const bool sat_b = (bv >= EnlightDefaults::SAT_HIGH || bv <= EnlightDefaults::SAT_LOW);

            if (sat_r) {
                if (i_first[0] == UINT32_MAX) i_first[0] = lt;
                i_last[0] = lt; w_sat[0]++;
            } else {
                rout_p  += (long long)rv * ks;
                rnear_p += (long long)rv * kc;
                sum_nonsat[0] += rv;
                if (rv > argmax_val[0]) { argmax_val[0] = rv; argmax_idx[0] = lt; }
            }
            if (sat_g) {
                if (i_first[1] == UINT32_MAX) i_first[1] = lt;
                i_last[1] = lt; w_sat[1]++;
            } else {
                gout_p  += (long long)gv * ks;
                gnear_p += (long long)gv * kc;
                sum_nonsat[1] += gv;
                if (gv > argmax_val[1]) { argmax_val[1] = gv; argmax_idx[1] = lt; }
            }
            if (sat_b) {
                if (i_first[2] == UINT32_MAX) i_first[2] = lt;
                i_last[2] = lt; w_sat[2]++;
            } else {
                bout_p  += (long long)bv * ks;
                bnear_p += (long long)bv * kc;
                sum_nonsat[2] += bv;
                if (bv > argmax_val[2]) { argmax_val[2] = bv; argmax_idx[2] = lt; }
            }

            if (!sat_r && !sat_g && !sat_b) _rawsum += rv + gv + bv;
            if (sat_r || sat_g || sat_b)    _satCount++;
            _arrayiter++;
        }

        // Ditch period if any channel is nearly fully saturated
        bool ditch = false;
        for (int ch = 0; ch < ADC_CHANNELS; ch++) {
            if ((float)w_sat[ch] > EnlightDefaults::SAT_DITCH_FRAC * (float)_goertzPeriod) {
                ditch = true; break;
            }
        }
        if (ditch) continue;

        _activePeriods++;

        const bool any_sat = (w_sat[0] > 0 || w_sat[1] > 0 || w_sat[2] > 0);

        if (!any_sat) {
            // Goertzel path: optimal SNR for weak signals, no saturation
            rout_c  += rout_p;  gout_c  += gout_p;  bout_c  += bout_p;
            rnear_c += rnear_p; gnear_c += gnear_p; bnear_c += bnear_p;
        } else {
            // Analytic sinusoidal fitting for all channels (signal strong, noise negligible)
            long long* const far_acc[ADC_CHANNELS]  = {&rout_c,  &gout_c,  &bout_c};
            long long* const near_acc[ADC_CHANNELS] = {&rnear_c, &gnear_c, &bnear_c};

            for (int ch = 0; ch < ADC_CHANNELS; ch++) {
                float    deltaX_rad;
                float    sum_ch;
                uint32_t x0;

                if (w_sat[ch] == 0) {
                    // Non-saturated channel: use full-period formula (exact at W_sat=0)
                    x0         = argmax_idx[ch];
                    deltaX_rad = (float)M_PI;
                    sum_ch     = (float)sum_nonsat[ch];
                } else {
                    const uint32_t w       = i_last[ch] - i_first[ch] + 1;
                    const uint32_t deltaXs = (_goertzPeriod - w) / 2;
                    deltaX_rad = (float)deltaXs * twoPiOverGP;
                    // x0: centre of non-saturated arc (one sample past the saturated zone end)
                    x0 = (i_last[ch] + 1 + deltaXs) % _goertzPeriod;

                    // SUM over the symmetric non-saturated interval [x0-deltaXs, x0+deltaXs]
                    sum_ch = 0.0f;
                    for (uint32_t d = 0; d <= 2 * deltaXs; d++) {
                        const uint32_t lt2 = (x0 + _goertzPeriod - deltaXs + d) % _goertzPeriod;
                        sum_ch += (float)r12((t_base + lt2) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch);
                    }
                }

                const float sx0   = (float)r12((t_base + x0) * ADC_CHANNELS + ADC_PIPELINE_DELAY + ch);
                const float num   = 2.0f * deltaX_rad * sx0 - sum_ch * twoPiOverGP;
                const float denom = 2.0f * (deltaX_rad - sinf(deltaX_rad));
                const float A     = (fabsf(denom) > 1e-6f) ? (num / denom) : 0.0f;
                const float phi   = twoPiOverGP * (float)x0;

                *far_acc[ch]  += (long long)(kern_f * halfGP_f * A * cosf(phi));
                *near_acc[ch] += (long long)(kern_f * halfGP_f * A * sinf(phi));
            }
        }
    }

    _rout  += rout_c;  _gout  += gout_c;  _bout  += bout_c;
    _rnear += rnear_c; _gnear += gnear_c; _bnear += bnear_c;
}

/* ============================================================
 *   classify()
 * ============================================================ */
EnlightResult Enlight::classify() {
    // Calibration baselines were measured with (_periodsPerCycle-1) active periods
    // per DMA cycle.  Scale by _activePeriods / (expected active periods this run)
    // so that ditched periods do not cause over-subtraction.
    const long long nCycles = (long long)(_arrayiter / (_goertzPeriod * _periodsPerCycle));
    const float nd_f  = (float)(_periodsPerCycle - 1) * (float)nCycles;
    const float scale = (nd_f > 0.0f) ? ((float)_activePeriods / nd_f) : 1.0f;
    const long long eff_rcal     = (long long)roundf((float)_cal.rcal     * scale);
    const long long eff_gcal     = (long long)roundf((float)_cal.gcal     * scale);
    const long long eff_bcal     = (long long)roundf((float)_cal.bcal     * scale);
    const long long eff_rcalNear = (long long)roundf((float)_cal.rcalNear * scale);
    const long long eff_gcalNear = (long long)roundf((float)_cal.gcalNear * scale);
    const long long eff_bcalNear = (long long)roundf((float)_cal.bcalNear * scale);

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
