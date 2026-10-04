#include "EnlightLedWave.h"
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"

EnlightLedWave::~EnlightLedWave() {
    heap_caps_free(_dma);
    heap_caps_free(_full);
    heap_caps_free(_low);
}

bool EnlightLedWave::begin(uint32_t periodClocks, uint32_t periods, float lowScale) {
    _periodBytes = periodClocks / PDM_CLKS_PER_BYTE;
    _periods     = periods;
    // DMA-capable and word-aligned, as heap_caps_malloc returns it.  A buffer
    // that is neither makes the SPI driver copy it into a bounce buffer of
    // the same size on every transfer — the RAM this class exists to save,
    // asked for again at every cycle.
    _dma  = (uint8_t*)heap_caps_malloc(bytes(), MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    // The stored periods are only ever read by the CPU.
    _full = (uint8_t*)heap_caps_malloc(_periodBytes, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    _low  = (uint8_t*)heap_caps_malloc(_periodBytes, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!_dma || !_full || !_low) return false;

    generatePeriod(_full, periodClocks, 1.0f);
    generatePeriod(_low,  periodClocks, lowScale);
    fill(_full);
    _holdsLow = false;
    return true;
}

bool EnlightLedWave::select(bool low) {
    if (low == _holdsLow) return false;
    fill(low ? _low : _full);
    _holdsLow = low;
    return true;
}

void EnlightLedWave::fill(const uint8_t* period) {
    for (uint32_t r = 0; r < _periods; r++)
        memcpy(_dma + r * _periodBytes, period, _periodBytes);
}

void EnlightLedWave::generatePeriod(uint8_t* buf, uint32_t periodClocks, float ampScale) {
    const float base = 0.5f + EnlightDefaults::PDM_AMP_OFFSET;
    const float swing = 0.5f - EnlightDefaults::PDM_AMP_OFFSET;
    const float twoPiOverT = 2.0f * (float)M_PI / (float)periodClocks;
    // SPI output is hardware-inverted: bit=1 → LED OFF, bit=0 → LED ON.
    // To scale LED power by ampScale, scale the LED signal (1-SPI), not SPI itself:
    //   SPI = 1 - (1 - base - swing*A*cos) * ampScale
    // At ampScale=1 this reduces to base + swing*A*cos (identical to full-power).
    // At ampScale=0.1, average SPI ≈ 0.96 → LED ON ~4% → dim.
    // Dry-run one full period to find the periodic steady-state accumulator values,
    // so the real pass starts in-phase with no transient.
    float acc_far = 0.0f, acc_near = 0.0f;
    for (uint32_t i = 0; i < periodClocks; i += PDM_CLKS_PER_BYTE) {
        for (uint32_t j = 0; j < PDM_CLKS_PER_BYTE; j++) {
            const float theta = twoPiOverT * (float)(i + j);
            const float d_far  = 1.0f - (1.0f - base - swing * PDM_AMPLITUDE * cosf(theta)) * ampScale;
            const float d_near = 1.0f - (1.0f - base - swing * PDM_AMPLITUDE * sinf(theta)) * ampScale;
            acc_far  += d_far  - (float)((acc_far  >= 0.5f) ? 1u : 0u);
            acc_near += d_near - (float)((acc_near >= 0.5f) ? 1u : 0u);
        }
    }
    for (uint32_t i = 0; i < periodClocks; i += PDM_CLKS_PER_BYTE) {
        uint8_t byte = 0;
        for (uint32_t j = 0; j < PDM_CLKS_PER_BYTE; j++) {
            const float theta = twoPiOverT * (float)(i + j);
            const float d_far  = 1.0f - (1.0f - base - swing * PDM_AMPLITUDE * cosf(theta)) * ampScale;
            const float d_near = 1.0f - (1.0f - base - swing * PDM_AMPLITUDE * sinf(theta)) * ampScale;
            const uint8_t b_far  = (acc_far  >= 0.5f) ? 1u : 0u;
            const uint8_t b_near = (acc_near >= 0.5f) ? 1u : 0u;
            acc_far  += d_far  - (float)b_far;
            acc_near += d_near - (float)b_near;
            const uint8_t sh = (uint8_t)(6u - j*2u);
            byte |= (uint8_t)(b_far << (sh+1u)); byte |= (uint8_t)(b_near << sh);
        }
        buf[i / PDM_CLKS_PER_BYTE] = byte;
    }
}
