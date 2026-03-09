#include "LightAir_RGBCtrl.h"
#include "LightAir_RGB.h"
#include "LightAir_RGB_HW.h"
#include <Arduino.h>

// ----------------------------------------------------
// Constructor
// ----------------------------------------------------
LightAir_RGBCtrl::LightAir_RGBCtrl(LightAir_RGB& rgb) :
_rgb(&rgb),
_currentColors(nullptr),
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
void LightAir_RGBCtrl::timer_triggered_playNextStep(LightAir_RGBCtrl* self) {
    self->playNextStep();
}

// ----------------------------------------------------
// Play RGB sequence
// ----------------------------------------------------
void LightAir_RGBCtrl::playSequence(
    const uint8_t colors[4][3],
    const uint16_t durations[4],
    uint8_t stepCount,
    uint32_t totalTimeMs
) {
    if (stepCount == 0 || stepCount > 4) return;

    stop();

    _currentColors     = colors;
    _currentDurations  = durations;
    _currentStepCount  = stepCount;
    _playStartMs       = millis();
    _totalPlayTimeMs   = totalTimeMs;
    _currentStep       = 0;
    _isPlaying         = true;

    playNextStep();
}

// ----------------------------------------------------
// Stop playback
// ----------------------------------------------------
void LightAir_RGBCtrl::stop() {
    _rgbTicker.detach();
    if (_rgb) _rgb->off();
    _isPlaying = false;
}

// ----------------------------------------------------
// Playback engine
// ----------------------------------------------------
void LightAir_RGBCtrl::playNextStep() {
    if (!_isPlaying) return;

    uint32_t elapsed = millis() - _playStartMs;
    if (elapsed >= _totalPlayTimeMs) {
        stop();
        return;
    }

    if (!_currentColors || !_currentDurations || _currentStepCount == 0) {
        stop();
        return;
    }

    if (_currentStep >= _currentStepCount) {
        _currentStep = 0; // Loop sequence
    }

    // Set current RGB
    if (_rgb) {
        _rgb->setColor(
            _currentColors[_currentStep][0],
            _currentColors[_currentStep][1],
            _currentColors[_currentStep][2]
        );
    }

    // Schedule next step
    _rgbTicker.attach_ms(
        _currentDurations[_currentStep],
        &timer_triggered_playNextStep,
        this
    );

    _currentStep++;
}
