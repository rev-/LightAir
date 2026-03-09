#ifndef LIGHTAIR_RGBCONTROL_H
#define LIGHTAIR_RGBCONTROL_H

#include <Arduino.h>
#include <Ticker.h>
#include "LightAir_RGB.h"

class LightAir_RGBCtrl {
public:
    explicit LightAir_RGBCtrl(LightAir_RGB& rgb);

    // Play RGB sequence
    void playSequence(
        const uint8_t colors[4][3],  // R,G,B for up to 4 steps
        const uint16_t durations[4],
        uint8_t stepCount,
        uint32_t totalTimeMs
    );

    void stop();

private:
    LightAir_RGB* _rgb;
    Ticker _rgbTicker;

    const uint8_t (*_currentColors)[3]; // pointer to array of RGB triplets
    const uint16_t* _currentDurations;
    uint8_t _currentStepCount;

    uint32_t _playStartMs;
    uint32_t _totalPlayTimeMs;
    uint8_t  _currentStep;
    bool     _isPlaying;

    void playNextStep();
    static void timer_triggered_playNextStep(LightAir_RGBCtrl* self);
};

#endif
