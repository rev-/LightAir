#pragma once
#include "LightAir_TotemRGB.h"
#include <Arduino.h>

// ----------------------------------------------------------------
// LightAir_TotemRGB_HW — 4-GPIO RGB LED driver for totems.
//
// Hardware: one common-cathode (or common-anode) RGB LED with the
// three colour channels on PWM-capable GPIO pins (4, 19, 21).
// analogWrite() provides 8-bit PWM per channel so any RGB colour
// can be represented, not just on/off.
//
// Usage:
//   LightAir_TotemRGB_HW rgb;
//   rgb.begin(pinComm, pinR, pinG, pinB, commonActive);
//   rgb.set(255, 0, 0);    // red at full brightness
//   rgb.set(0, 128, 255);  // mix
//   rgb.off();
// ----------------------------------------------------------------
class LightAir_TotemRGB_HW : public LightAir_TotemRGB {
public:
    // pinComm      : shared common pin (driven to commonActive to enable LED)
    // pinR/G/B     : individual colour pins (must be PWM-capable)
    // commonActive : HIGH for common-anode, LOW for common-cathode
    void begin(int pinComm, int pinR, int pinG, int pinB,
               bool commonActive = LOW);

    void set(uint8_t r, uint8_t g, uint8_t b) override;
    void off() override;

private:
    int  _pinComm, _pinR, _pinG, _pinB;
    bool _commonActive;
};
