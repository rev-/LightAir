#pragma once
#include "LightAir_TotemRGB.h"
#include <Arduino.h>

// ----------------------------------------------------------------
// LightAir_TotemRGB_HW — 4-GPIO RGB LED driver for totems.
//
// Hardware: one common-cathode (or common-anode) RGB LED driven
// by four digital output pins.  No PWM — each channel is ON/OFF.
// This matches the circuit in TOTEM_Team0_LED.ino (BUTTON_COMM,
// BUTTON_R, BUTTON_G, BUTTON_B).
//
// Usage:
//   LightAir_TotemRGB_HW rgb;
//   rgb.begin(pinComm, pinR, pinG, pinB, commonActive);
//   rgb.set(true, false, false);   // red on
//   rgb.off();
// ----------------------------------------------------------------
class LightAir_TotemRGB_HW : public LightAir_TotemRGB {
public:
    // pinComm      : shared common pin (driven to commonActive to enable LED)
    // pinR/G/B     : individual colour pins
    // commonActive : HIGH for common-anode, LOW for common-cathode
    void begin(int pinComm, int pinR, int pinG, int pinB,
               bool commonActive = LOW);

    void set(bool r, bool g, bool b) override;
    void off() override;

private:
    int  _pinComm, _pinR, _pinG, _pinB;
    bool _commonActive;
};
