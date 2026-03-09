#include "SoundCtrl.h"


SoundCtrl::SoundCtrl() {
    _noteCount = 0;
    _repeatCount = 0;
    _currentNote = 0;
    _isPlaying = false;
    Buzzer_HW _Buzzer;
    Ticker _noteTicker;
}


void SoundCtrl::timer_triggered_playNextNote(SoundCtrl* sc) {
    //needs to be public and static to work with Ticker over ESP!
    sc->playNextNote();
}


void SoundCtrl::playLit() {
    _freqs[0] = 4000;
    _durations[0] = 2000;
    play(_freqs,_durations,1,0);
}

void SoundCtrl::playDown() {
    _freqs[0]=4000;
    _freqs[1]=3174;
    _freqs[2]=2376;
    _freqs[3]=1782;
    _durations[0]=150;
    _durations[1]=120;
    _durations[2]=100;
    _durations[3]=500;
    play(_freqs,_durations,4,10);
}

void SoundCtrl::playUp() {
    _freqs[0]=1782;
    _freqs[1]=2376;
    _freqs[2]=3174;
    _freqs[3]=4000;
    _durations[0]=100;
    _durations[1]=100;
    _durations[2]=120;
    _durations[3]=500;
    play(_freqs,_durations,4,5);
}

void SoundCtrl::playEndGame() {
    _freqs[0] = 5000;
    _freqs[1] = 0;
    _durations[0] = 3000;
    _durations[0]= 0;
    play(_freqs,_durations,2,3);
}

void SoundCtrl::play(int freqs[], int durations[], uint8_t count, int repeat) {
    if (count == 0 || count > 4) return;

    // Copy sequence
    for (uint8_t i = 0; i < count; i++) {
        _freqs[i] = freqs[i];
        _durations[i] = durations[i];
    }

    _noteCount = count;
    _repeatCount = repeat;
    _currentNote = 0;
    _isPlaying = true;

    playNextNote();
}


void SoundCtrl::playNextNote() {
    if (!_isPlaying) return;

    if (_currentNote >= _noteCount) {
        if (_repeatCount >= 1) {
            _repeatCount--;
            _currentNote = 0;
        } else {
            stop();
            return;
        }
    }

    // Play current note
    _Buzzer.play(_freqs[_currentNote]);
    _noteTicker.attach_ms(_durations[_currentNote], &timer_triggered_playNextNote, this);

    // Iterate for next note
    _currentNote++;

}

void SoundCtrl::stop() {
    _noteTicker.detach();
    _Buzzer.stop();   // Stop buzzer
    _isPlaying = false;
}
