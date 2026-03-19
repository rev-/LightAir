#pragma once
#include <Arduino.h>

// ----------------------------------------------------------------
// LightAir_TotemRGB — simple 4-GPIO RGB LED driver for totems.
//
// Hardware: one common-cathode (or common-anode) RGB LED driven
// by four digital output pins.  No PWM — each channel is ON/OFF.
// This matches the circuit in TOTEM_Team0_LED.ino (BUTTON_COMM,
// BUTTON_R, BUTTON_G, BUTTON_B).
//
// Usage:
//   LightAir_TotemRGB rgb;
//   rgb.begin(pinComm, pinR, pinG, pinB, commonActive);
//   rgb.set(true, false, false);   // red on
//   rgb.off();
// ----------------------------------------------------------------
class LightAir_TotemRGB {
public:
    // pinComm      : shared common pin (driven to commonActive to enable LED)
    // pinR/G/B     : individual colour pins
    // commonActive : HIGH for common-anode, LOW for common-cathode
    void begin(int pinComm, int pinR, int pinG, int pinB,
               bool commonActive = LOW);

    // Set colour; each argument true = channel on.
    void set(bool r, bool g, bool b);

    // Turn all channels off.
    void off();

private:
    int  _pinComm, _pinR, _pinG, _pinB;
    bool _commonActive;
};
