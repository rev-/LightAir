// LightAir_Audio.h
#ifndef LIGHTAIR_AUDIO_H
#define LIGHTAIR_AUDIO_H

class LightAir_Audio {
public:
    virtual ~LightAir_Audio() {}     // Virtual destructor for safety
    virtual void play(int freq) = 0;
    virtual void stop() = 0;
};

#endif
