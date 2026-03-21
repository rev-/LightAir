#pragma once
#include "LightAir_LEDStrip.h"
#include <Arduino.h>
#include <FastLED.h>

// ----------------------------------------------------------------
// LightAir_LEDStrip_HW — WS2812B animation engine.
//
// Effects:
//   Off        — all LEDs off.
//   Fill       — all LEDs set to colour instantly.
//   Wipe       — sequential per-LED advance (~stepMs per LED).
//   Pulse      — fade full→dim→full over durationMs; loops if background.
//   Blink      — on/off toggle every durationMs/2; loops if background.
//   BlinkFast  — on/off toggle every 150 ms (fixed); loops if background.
//   Chase      — single lit LED advances along strip.
//   Alternate  — odd/even LEDs swap between two colours every durationMs/2.
//
// Usage:
//   LightAir_LEDStrip_HW strip;
//   strip.begin(13, 13);             // DATA_PIN=13, NUM_LEDS=13
//   strip.loop({ 255,0,0, StripEffect::Blink, 1000 });   // red blink forever
//   strip.play({ 0,255,0, StripEffect::Wipe,  500  });   // green wipe once
//   // in game loop:
//   strip.update();
// ----------------------------------------------------------------
class LightAir_LEDStrip_HW : public LightAir_LEDStrip {
public:
    // dataPin   : GPIO pin connected to strip data input
    // numLeds   : number of LEDs in the strip
    void begin(int dataPin, uint8_t numLeds);

    void play(const StripAnimation& anim) override;
    void loop(const StripAnimation& anim) override;
    void stopLoop() override;
    void update() override;

private:
    CRGB    _leds[MAX_LEDS];
    uint8_t _numLeds  = 0;

    // Foreground (one-shot)
    StripAnimation _fg         = {};
    bool           _fgActive   = false;
    uint32_t       _fgStartMs  = 0;
    uint8_t        _fgStep     = 0;    // for Wipe / Chase current LED index

    // Background (looping)
    StripAnimation _bg         = {};
    bool           _bgActive   = false;
    uint32_t       _bgCycleMs  = 0;   // millis() when current bg cycle started
    uint8_t        _bgStep     = 0;
    bool           _bgPhase    = false;

    void renderAnim(const StripAnimation& a,
                    uint32_t elapsed,
                    uint8_t& step,
                    bool&  phase,
                    bool   looping);
    void setAll(uint8_t r, uint8_t g, uint8_t b);
    void setAlternate(uint8_t r1, uint8_t g1, uint8_t b1,
                      uint8_t r2, uint8_t g2, uint8_t b2,
                      bool phase);
};
