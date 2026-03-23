#include "LightAir_RGB_HW.h"
#include "LightAir_RGB.h"
#include <Arduino.h>

// ----------------------------------------------------
// Constructor
// ----------------------------------------------------
LightAir_RGB_HW::LightAir_RGB_HW(int8_t pinR, int8_t pinG, int8_t pinB)
: _pinR(pinR), _pinG(pinG), _pinB(pinB)
{
    if (_pinR >= 0) pinMode(_pinR, OUTPUT);
    if (_pinG >= 0) pinMode(_pinG, OUTPUT);
    if (_pinB >= 0) pinMode(_pinB, OUTPUT);

    off(); // start with LED off
}

// ----------------------------------------------------
// Set RGB color
// ----------------------------------------------------
void LightAir_RGB_HW::setColor(uint8_t r, uint8_t g, uint8_t b) {
    if (_pinR >= 0) writePWM(_pinR, r);
    if (_pinG >= 0) writePWM(_pinG, g);
    if (_pinB >= 0) writePWM(_pinB, b);
}

// ----------------------------------------------------
// Turn off LED
// ----------------------------------------------------
void LightAir_RGB_HW::off() {
    if (_pinR >= 0) writePWM(_pinR, 0);
    if (_pinG >= 0) writePWM(_pinG, 0);
    if (_pinB >= 0) writePWM(_pinB, 0);
}

// ----------------------------------------------------
// Write PWM value (0-255)
// ----------------------------------------------------
void LightAir_RGB_HW::writePWM(int8_t pin, uint8_t value) {
    if (pin < 0) return; // ignore invalid pin
    analogWrite(pin, value);
}
