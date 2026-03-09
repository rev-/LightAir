#include "LightAir_SoundCtrl.h"
#include <Arduino.h>

// ----------------------------------------------------
// Constructor
// ----------------------------------------------------
LightAir_SoundCtrl::LightAir_SoundCtrl(LightAir_Audio& audio):
_audio(&audio),
_playStartMs(0),
_totalPlayTimeMs(0),
_selectedSound(0),
_currentNote(0),
_isPlaying(false),
_currentFreqs(nullptr),
_currentDurations(nullptr),
_currentStepCount(0)
{
    // Initialize custom sounds as silent
    for (uint8_t i = 0; i < 4; i++) {
        _customSounds[i].stepCount = 0;
    }
}

// ----------------------------------------------------
// Static Ticker Trampoline
// ----------------------------------------------------
void LightAir_SoundCtrl::timer_triggered_playNextNote(LightAir_SoundCtrl* self) {
    self->playNextNote();
}

// ----------------------------------------------------
// Play arbitrary sequence (UIAction style)
// ----------------------------------------------------
void LightAir_SoundCtrl::playSound(
    const uint16_t freqs[4],
    const uint16_t durations[4],
    uint8_t stepCount,
    uint32_t totalTimeMs
) {
    if (stepCount == 0 || stepCount > 4) return;

    stop();

    _currentFreqs      = freqs;
    _currentDurations  = durations;
    _currentStepCount  = stepCount;
    _playStartMs       = millis();
    _totalPlayTimeMs   = totalTimeMs;
    _currentNote       = 0;
    _isPlaying         = true;

    playNextNote();
}

// ----------------------------------------------------
// Stop playback
// ----------------------------------------------------
void LightAir_SoundCtrl::stop() {
    _noteTicker.detach();
    _audio->stop();
    _isPlaying = false;
}

// ----------------------------------------------------
// Define custom sounds
// ----------------------------------------------------
void LightAir_SoundCtrl::defineCustomSound(
    SoundType slot,
    const uint16_t freqs[4],
    const uint16_t durations[4],
    uint8_t stepCount
) {
    if (slot < SoundType::Custom1 || slot > SoundType::Custom4) return;
    if (stepCount == 0 || stepCount > 4) return;

    uint8_t index = static_cast<uint8_t>(slot) - static_cast<uint8_t>(SoundType::Custom1);

    for (uint8_t i = 0; i < stepCount; i++) {
        _customSounds[index].freqs[i]     = freqs[i];
        _customSounds[index].durations[i] = durations[i];
    }

    _customSounds[index].stepCount = stepCount;
}

// ----------------------------------------------------
// Playback Engine
// ----------------------------------------------------
void LightAir_SoundCtrl::playNextNote() {
    if (!_isPlaying) return;

    // Check total elapsed time
    uint32_t elapsed = millis() - _playStartMs;
    if (elapsed >= _totalPlayTimeMs) {
        stop();
        return;
    }

    const uint16_t* freqs = nullptr;
    const uint16_t* durations = nullptr;
    uint8_t stepCount = 0;

    // If UIAction-style sequence
    if (_currentFreqs && _currentDurations && _currentStepCount > 0) {
        freqs = _currentFreqs;
        durations = _currentDurations;
        stepCount = _currentStepCount;
    } else {
        // Pre-defined sound
        const SoundSequence* seq = nullptr;

        if (_selectedSound < static_cast<uint8_t>(SoundType::Custom1)) {
            seq = &_soundTable[_selectedSound];
        } else {
            uint8_t customIndex = _selectedSound - static_cast<uint8_t>(SoundType::Custom1);
            seq = &_customSounds[customIndex];
        }

        if (!seq || seq->stepCount == 0) {
            stop();
            return;
        }

        freqs = seq->freqs;
        durations = seq->durations;
        stepCount = seq->stepCount;
    }

    if (_currentNote >= stepCount) {
        _currentNote = 0; // Loop sequence
    }

    // Play note
    _audio->play(freqs[_currentNote]);

    // Schedule next note
    _noteTicker.attach_ms(
        durations[_currentNote],
        &timer_triggered_playNextNote,
        this
    );

    _currentNote++;
}
