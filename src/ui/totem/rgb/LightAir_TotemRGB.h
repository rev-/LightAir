#pragma once

// ----------------------------------------------------------------
// LightAir_TotemRGB — abstract interface for a totem RGB LED.
//
// Each channel is ON/OFF only (no PWM).
// Concrete implementation: LightAir_TotemRGB_HW
// ----------------------------------------------------------------
class LightAir_TotemRGB {
public:
    virtual ~LightAir_TotemRGB() {}

    // Set colour; each argument true = channel on.
    virtual void set(bool r, bool g, bool b) = 0;

    // Turn all channels off.
    virtual void off() = 0;
};
