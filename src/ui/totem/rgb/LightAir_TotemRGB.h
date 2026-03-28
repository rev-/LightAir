#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_TotemRGB — abstract interface for a totem RGB LED.
//
// Each channel is driven with 8-bit PWM (0 = off, 255 = full).
// set(0,0,0) is equivalent to off().
// Concrete implementation: LightAir_TotemRGB_HW
// ----------------------------------------------------------------
class LightAir_TotemRGB {
public:
    virtual ~LightAir_TotemRGB() {}

    // Set colour via PWM; 0 = off, 255 = full brightness per channel.
    virtual void set(uint8_t r, uint8_t g, uint8_t b) = 0;

    // Turn all channels off.
    virtual void off() = 0;
};
