#ifndef LIGHTAIR_VIBRATIONCTRL_H
#define LIGHTAIR_VIBRATIONCTRL_H

#include <Arduino.h>
#include <Ticker.h>
#include "LightAir_Vibration.h"

class LightAir_VibrationCtrl {
public:
    explicit LightAir_VibrationCtrl(LightAir_Vibration& vib);

    // Play a sequence of vibrations
    void playSequence(
        const uint8_t intensities[4],
        const uint16_t durations[4],
        uint8_t stepCount,
        uint32_t totalTimeMs
    );

    void stop();

private:
    LightAir_Vibration* _vib;
    Ticker _vibTicker;

    const uint8_t*  _currentIntensities;
    const uint16_t* _currentDurations;
    uint8_t         _currentStepCount;

    uint32_t _playStartMs;
    uint32_t _totalPlayTimeMs;
    uint8_t  _currentStep;
    bool     _isPlaying;

    void playNextStep();
    static void timer_triggered_playNextStep(LightAir_VibrationCtrl* self);
};

#endif
