#ifndef LIGHTAIR_RGB_HW_H
#define LIGHTAIR_RGB_HW_H

#include "LightAir_RGB.h"
#include <Arduino.h>

class LightAir_RGB_HW : public LightAir_RGB {
public:
    // Constructor: optionally provide pins, default -1 disables output
    LightAir_RGB_HW(
        int8_t pinR = -1,
        int8_t pinG = -1,
        int8_t pinB = -1
    );

    void setColor(uint8_t r, uint8_t g, uint8_t b) override;
    void off() override;

private:
    int8_t _pinR;
    int8_t _pinG;
    int8_t _pinB;

    void writePWM(int8_t pin, uint8_t value);
};

#endif
