#include "LightAir_TotemRGB_HW.h"

void LightAir_TotemRGB_HW::begin(int pinComm, int pinR, int pinG, int pinB,
                                  bool commonActive) {
    _pinComm      = pinComm;
    _pinR         = pinR;
    _pinG         = pinG;
    _pinB         = pinB;
    _commonActive = commonActive;

    pinMode(pinComm, OUTPUT);
    pinMode(pinR,    OUTPUT);
    pinMode(pinG,    OUTPUT);
    pinMode(pinB,    OUTPUT);
    off();
}

void LightAir_TotemRGB_HW::set(uint8_t r, uint8_t g, uint8_t b) {
    if (r == 0 && g == 0 && b == 0) { off(); return; }
    // Drive PWM channels first, then enable common to avoid colour flicker.
    analogWrite(_pinR, r);
    analogWrite(_pinG, g);
    analogWrite(_pinB, b);
    digitalWrite(_pinComm, _commonActive);
}

void LightAir_TotemRGB_HW::off() {
    // Disable common first to avoid colour flicker on channel change.
    digitalWrite(_pinComm, !_commonActive);
    digitalWrite(_pinR, LOW);
    digitalWrite(_pinG, LOW);
    digitalWrite(_pinB, LOW);
}
