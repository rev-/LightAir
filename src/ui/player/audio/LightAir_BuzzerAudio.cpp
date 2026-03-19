#include "LightAir_BuzzerAudio.h"
#include <Arduino.h>

LightAir_BuzzerAudio::LightAir_BuzzerAudio(
    int pin,
    int channel,
    int baseFreq,
    int resolution
)
: _pin(pin), _channel(channel), _baseFreq(baseFreq), _resolution(resolution)
{
    pinMode(_pin, OUTPUT);
    ledcSetup(_channel, _baseFreq, _resolution);
    ledcAttachPin(_pin, _channel);
    ledcWrite(_channel, 0); // Ensure buzzer is off initially
}

void LightAir_BuzzerAudio::play(int freq)
{
    ledcWriteTone(_channel, freq);
}

void LightAir_BuzzerAudio::stop()
{
    ledcWrite(_channel, 0);
}
