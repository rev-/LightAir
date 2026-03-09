#pragma once
#include <Arduino.h>
#include "LightAir_Button.h"

// Concrete GPIO button.
// activeLow = true  (default): pin is INPUT_PULLUP; pressed = LOW.
// activeLow = false           : pin is INPUT;        pressed = HIGH.
class LightAir_HWButton : public LightAir_Button {
public:
    LightAir_HWButton(uint8_t pin, bool activeLow = true);

    // Call once in setup() before registering with InputCtrl.
    void begin();

    bool isPressed() const override;

private:
    uint8_t _pin;
    bool    _activeLow;
};
