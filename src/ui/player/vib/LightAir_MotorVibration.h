#ifndef LIGHTAIR_MOTOR_VIBRATION_H
#define LIGHTAIR_MOTOR_VIBRATION_H

#include <Arduino.h>
#include "LightAir_Vibration.h"

class LightAir_MotorVibration : public LightAir_Vibration {
public:
    LightAir_MotorVibration(
        int pin = 12
    );

    void vibrate(int intensity = 255) override;
    void stop() override;

private:
    int _pin;
};

#endif
