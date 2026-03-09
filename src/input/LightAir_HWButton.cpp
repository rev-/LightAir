#include "LightAir_HWButton.h"

LightAir_HWButton::LightAir_HWButton(uint8_t pin, bool activeLow)
    : _pin(pin), _activeLow(activeLow) {}

void LightAir_HWButton::begin() {
    pinMode(_pin, _activeLow ? INPUT_PULLUP : INPUT);
}

bool LightAir_HWButton::isPressed() const {
    bool pinHigh = (digitalRead(_pin) == HIGH);
    return _activeLow ? !pinHigh : pinHigh;
}
