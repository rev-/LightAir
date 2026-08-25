#pragma once
#include "SpiAdcSensor.h"

// Resistive voltage divider connected to a 12-bit ADC channel.
//
// Circuit:
//   V_in ── [r_top] ── ADC_pin ── [r_bottom] ── GND
//
// Conversion:
//   V_in = raw * vref * (r_top + r_bottom) / (r_bottom * 4095)
class VDivSensor : public SpiAdcSensor {
public:
    VDivSensor(float r_top, float r_bottom, float vref = 3.3f)
        : _scale(vref * (r_top + r_bottom) / (r_bottom * 4095.0f)) {}

protected:
    float convert(uint16_t raw) const override {
        return raw * _scale;
    }

private:
    float _scale;
};
