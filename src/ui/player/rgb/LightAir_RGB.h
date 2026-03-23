#ifndef LIGHTAIR_RGB_H
#define LIGHTAIR_RGB_H

#include <stdint.h>

class LightAir_RGB {
public:
    virtual ~LightAir_RGB() {}

    // Play a single RGB color
    virtual void setColor(uint8_t r, uint8_t g, uint8_t b) = 0;

    // Turn off RGB output
    virtual void off() = 0;
};
