#pragma once
#include "../enlight/SpiAdcBus.h"
#include <stdint.h>

// Base class for a single ADC channel on the shared SpiAdcBus.
// Subclasses override convert() with the physical formula for their sensor type.
// read() calls the bus for a raw 12-bit value, then applies convert().
// No LUT — conversion runs at read time (called at most once every few seconds).
class SpiAdcSensor {
public:
    virtual ~SpiAdcSensor() = default;

    // bus: shared bus object.  cmd: channel-select byte.  unit: e.g. "V", "C".
    void begin(SpiAdcBus& bus, uint8_t cmd, const char* unit);

    // Read the ADC and apply convert().
    // Returns false on SPI error or if convert() returns NaN.
    bool read(float& out) const;

    const char* unit() const { return _unit; }

protected:
    // Subclass implements the physical conversion from 12-bit raw to output value.
    // Return NAN for physically impossible raw values (e.g. open/short circuit).
    virtual float convert(uint16_t raw) const = 0;

private:
    SpiAdcBus*  _bus  = nullptr;
    uint8_t     _cmd  = 0;
    const char* _unit = "";
};
