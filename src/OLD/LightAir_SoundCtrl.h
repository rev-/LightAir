#ifndef LIGHTAIR_SOUNDCTRL_H
#define LIGHTAIR_SOUNDCTRL_H

#include <Ticker.h>
#include "LightAir_Audio.h"
#include "LightAir_BuzzerAudio.h"
#include <Arduino.h>

enum class SoundType : uint8_t {
    Enlight = 0,
    Lit,
    Down,
    Up,
    EndGame,
    FlagGain,
    FlagTaken,
    FlagReturn,
    ControlGain,
    ControlLoss,
    RoleChange,
    Stop,
    Bonus,
    Malus,
    Special1,
    Special2,
    Custom1,
    Custom2,
    Custom3,
    Custom4,
    Count
};

// ---------------- Sound Sequence ----------------
struct SoundSequence {
    uint16_t freqs[4];
    uint16_t durations[4];
    uint8_t stepCount;
};

class LightAir_SoundCtrl {
public:
    explicit LightAir_SoundCtrl(LightAir_Audio& audio);

    // Play arbitrary sequence (UIAction style)
    void playSound(
        const uint16_t freqs[4],
        const uint16_t durations[4],
        uint8_t stepCount,
        uint32_t totalTimeMs
    );

    void stop();

    // Define custom sounds
    void defineCustomSound(
        SoundType slot,
        const uint16_t freqs[4],
        const uint16_t durations[4],
        uint8_t stepCount
    );

private:
    LightAir_Audio* _audio;
    Ticker _noteTicker;

    uint32_t _playStartMs;
    uint32_t _totalPlayTimeMs;
    uint8_t  _selectedSound;
    uint8_t  _currentNote;
    bool     _isPlaying;

    SoundSequence _customSounds[4];     // Custom1–4
    static const SoundSequence _soundTable[static_cast<uint8_t>(SoundType::Custom1)];

    void playNextNote();

    static void timer_triggered_playNextNote(LightAir_SoundCtrl* self);

    // Internal buffer for UIAction-style play
    const uint16_t* _currentFreqs;
    const uint16_t* _currentDurations;
    uint8_t         _currentStepCount;
};

#endif
