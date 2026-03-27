#include "LightAir_MotorVibration.h"
#include "LightAir_Vibration.h"
#include <Arduino.h>

LightAir_MotorVibration::LightAir_MotorVibration(int pin)
: _pin(pin)
{
    pinMode(_pin, OUTPUT);
    analogWrite(_pin, 0);  // Motor off
}

void LightAir_MotorVibration::vibrate(int intensity)
{
    // Clamp intensity to valid PWM range
    if (intensity < 0) intensity = 0;
    if (intensity > 255) intensity = 255;

    analogWrite(_pin, intensity);
}

void LightAir_MotorVibration::stop()
{
    analogWrite(_pin, 0);
}
