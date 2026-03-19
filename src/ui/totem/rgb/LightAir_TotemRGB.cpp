#include "LightAir_TotemRGB.h"

void LightAir_TotemRGB::begin(int pinComm, int pinR, int pinG, int pinB,
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

void LightAir_TotemRGB::set(bool r, bool g, bool b) {
    // Drive colour channels first, then enable common.
    digitalWrite(_pinR, r ? HIGH : LOW);
    digitalWrite(_pinG, g ? HIGH : LOW);
    digitalWrite(_pinB, b ? HIGH : LOW);
    digitalWrite(_pinComm, _commonActive);
}

void LightAir_TotemRGB::off() {
    // Disable common first to avoid colour flicker on channel change.
    digitalWrite(_pinComm, !_commonActive);
    digitalWrite(_pinR, LOW);
    digitalWrite(_pinG, LOW);
    digitalWrite(_pinB, LOW);
}
