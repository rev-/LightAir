#ifndef SoundCtrl_h
#define SoundCtrl_h

#include "Arduino.h"
#include "Ticker.h"
#include "Buzzer.h"

class SoundCtrl {
public:
    SoundCtrl();
    static void timer_triggered_playNextNote(SoundCtrl *sc);
    void play(int freqs[], int durations[], uint8_t count, int repeat = 1);
    void playLit(void);
    void playDown(void);
    void playUp(void);
    void playEndGame(void);
    void stop(void);
    void playNextNote(void);

private:

    Buzzer_HW _Buzzer;

    int _freqs[4];
    int _durations[4];
    uint8_t _noteCount;
    int _repeatCount;

    int _currentNote;

    Ticker _noteTicker;
    bool _isPlaying;
};

#endif
