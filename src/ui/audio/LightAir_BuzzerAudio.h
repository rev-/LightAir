#ifndef LIGHTAIR_BUZZERAUDIO_H
#define LIGHTAIR_BUZZERAUDIO_H

#include "LightAir_Audio.h"
#include <Arduino.h>

class LightAir_BuzzerAudio : public LightAir_Audio {
public:
    // Constructor with default values
    LightAir_BuzzerAudio(
        int pin = 4,
        int channel = 0,
        int baseFreq = 4000,
        int resolution = 12
    );

    void play(int freq) override;
    void stop() override;

private:
    int _pin;
    int _channel;
    int _baseFreq;
    int _resolution;
};

#endif
