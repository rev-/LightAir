#pragma once
#include "../config.h"
#include <stdint.h>
#include <stddef.h>

// PDM geometry: the two LED channels (FAR and NEAR) ride the two DIO lines,
// two bits per clock, so a byte carries four clocks.
static constexpr uint32_t PDM_CLKS_PER_BYTE = 4;
static constexpr float    PDM_AMPLITUDE     = 0.95f;

// ----------------------------------------------------------------
// EnlightLedWave — the LED modulation the DMA plays, in ONE buffer.
//
// A DMA cycle is a whole number of identical PDM periods, so its buffer is
// fully described by one period.  A run plays it at full power, or drops to
// LOW_POWER_FACTOR once a cycle saturates.  Keeping a whole cycle of each
// power cost a second 31 KB of DMA RAM for the dim copy — on boards with no
// PSRAM, the largest single allocation in the firmware.  Instead one period
// of each power is kept, and the buffer is rewritten from the other period
// when a run changes power.
//
// What the DMA sends is byte-for-byte what the two-buffer layout sent; only
// the moment the bytes are written changes.  select() must therefore run
// while no transfer is reading the buffer.  Enlight calls it from its cycle
// task between cycles, which is the only place an LED transfer is queued.
// ----------------------------------------------------------------
class EnlightLedWave {
public:
    EnlightLedWave() = default;
    ~EnlightLedWave();
    EnlightLedWave(const EnlightLedWave&) = delete;
    EnlightLedWave& operator=(const EnlightLedWave&) = delete;

    // Allocate the DMA buffer (`periods` periods) and the two stored
    // periods, generate both, and fill the buffer at full power.
    bool begin(uint32_t periodClocks, uint32_t periods, float lowScale);

    // Make the buffer hold the requested power.  True when it had to be
    // rewritten (one memcpy per period), false when it already held it.
    bool select(bool low);

    const uint8_t* dmaBuf()          const { return _dma; }
    size_t         bytes()           const { return (size_t)_periodBytes * _periods; }
    uint32_t       periodBytes()     const { return _periodBytes; }
    bool           holdsLow()        const { return _holdsLow; }
    const uint8_t* period(bool low)  const { return low ? _low : _full; }

    // One period of the sigma-delta PDM at the given amplitude scale, both
    // channels interleaved (FAR = cos in the high bit, NEAR = sin in the low
    // bit).  Writes periodClocks / PDM_CLKS_PER_BYTE bytes.
    static void generatePeriod(uint8_t* buf, uint32_t periodClocks, float ampScale);

private:
    uint8_t* _dma         = nullptr;   // the only buffer a transfer reads
    uint8_t* _full        = nullptr;   // one period at full power
    uint8_t* _low         = nullptr;   // one period at lowScale
    uint32_t _periodBytes = 0;
    uint32_t _periods     = 0;
    bool     _holdsLow    = false;

    void fill(const uint8_t* period);
};
