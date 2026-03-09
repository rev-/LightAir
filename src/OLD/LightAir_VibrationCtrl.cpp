#include "LightAir_VibrationCtrl.h"

// ----------------------------------------------------
// Constructor
// ----------------------------------------------------
LightAir_VibrationCtrl::LightAir_VibrationCtrl(LightAir_Vibration& vib) :
_vib(&vib),
_currentIntensities(nullptr),
_currentDurations(nullptr),
_currentStepCount(0),
_playStartMs(0),
_totalPlayTimeMs(0),
_currentStep(0),
_isPlaying(false)
{}

// ----------------------------------------------------
// Static Ticker trampoline
// ----------------------------------------------------
void LightAir_VibrationCtrl::timer_triggered_playNextStep(LightAir_VibrationCtrl* self) {
    self->playNextStep();
}

// ----------------------------------------------------
// Play arbitrary vibration sequence
// ----------------------------------------------------
void LightAir_VibrationCtrl::playSequence(
    const uint8_t intensities[4],
    const uint16_t durations[4],
    uint8_t stepCount,
    uint32_t totalTimeMs
) {
    if (stepCount == 0 || stepCount > 4) return;

    stop();

    _currentIntensities  = intensities;
    _currentDurations    = durations;
    _currentStepCount    = stepCount;
    _playStartMs         = millis();
    _totalPlayTimeMs     = totalTimeMs;
    _currentStep         = 0;
    _isPlaying           = true;

    playNextStep();
}

// ----------------------------------------------------
// Stop playback
// ----------------------------------------------------
void LightAir_VibrationCtrl::stop() {
    _vibTicker.detach();
    if (_vib) _vib->stop();
    _isPlaying = false;
}

// ----------------------------------------------------
// Playback engine
// ----------------------------------------------------
void LightAir_VibrationCtrl::playNextStep() {
    if (!_isPlaying) return;

    uint32_t elapsed = millis() - _playStartMs;
    if (elapsed >= _totalPlayTimeMs) {
        stop();
        return;
    }

    if (!_currentIntensities || !_currentDurations || _currentStepCount == 0) {
        stop();
        return;
    }

    if (_currentStep >= _currentStepCount) {
        _currentStep = 0; // Loop sequence
    }

    // Start current vibration
    _vib->vibrate(_currentIntensities[_currentStep]);

    // Schedule next step
    _vibTicker.attach_ms(
        _currentDurations[_currentStep],
        &timer_triggered_playNextStep,
        this
    );

    _currentStep++;
}
