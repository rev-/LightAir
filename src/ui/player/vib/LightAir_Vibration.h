#ifndef LIGHTAIR_VIBRATION_H
#define LIGHTAIR_VIBRATION_H

class LightAir_Vibration {
public:
    virtual ~LightAir_Vibration() {}

    // Start vibration at a given intensity (0-255)
    virtual void vibrate(int intensity) = 0;

    // Stop vibration
    virtual void stop() = 0;
};

#endif
