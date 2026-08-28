#pragma once
#include "LightAir_LEDStrip.h"
#include <Arduino.h>
#include <FastLED.h>

// ----------------------------------------------------------------
// LightAir_LEDStrip_HW — WS2812B animation engine.
//
// Renders the StripAnimation building blocks (zone + effect + timing)
// from LightAir_LEDStrip.h.  Each frame is computed purely from the time
// elapsed since play()/loop() was called, so there is no per-effect step
// state to keep in sync.
//
// Beat groups (pulseCount):
//   pulseCount == 0 → the effect runs continuously (one cycle = durationMs,
//                     repeating forever); used for roaming idles (Chase).
//   pulseCount >= 1 → that many motion cycles play back-to-back, then one
//                     silent cycle (all off), then the group repeats; used
//                     to give per-team "1 slow pulse" vs "2 fast blinks".
//
// Usage:
//   LightAir_LEDStrip_HW strip;
//   strip.begin(13, 13);                                  // DATA_PIN, NUM_LEDS
//   strip.loop({ 255,0,0, StripEffect::Blink, 1000 });    // red blink forever
//   strip.play({ 0,255,0, StripEffect::Wipe,  500  });    // green wipe once
//   // one-shots queue: two play() calls in one tick show one after the other
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

    // Foreground (one-shots), played in arrival order.  _fgCount is the
    // number still waiting including the one on screen, which sits at _fgHead.
    StripAnimation _fg[MAX_ONESHOTS] = {};
    uint8_t        _fgHead    = 0;
    uint8_t        _fgCount   = 0;
    uint32_t       _fgStartMs = 0;

    // Background (looping)
    StripAnimation _bg        = {};
    bool           _bgActive  = false;
    uint32_t       _bgStartMs = 0;

    // Total play time of one queued one-shot (pulseCount cycles, or one).
    static uint32_t oneShotTotal(const StripAnimation& a);

    // Render `a` into _leds for the given elapsed time (ms since start).
    void renderAnim(const StripAnimation& a, uint32_t elapsed);

    // Zone helpers: number of LEDs in a zone, and the physical LED index of
    // the i-th member of that zone (so effects iterate a zone uniformly).
    uint8_t zoneCount(StripZone zone) const;
    uint8_t zoneLed(StripZone zone, uint8_t i) const;
    void    setZone(StripZone zone, uint8_t r, uint8_t g, uint8_t b);

    void setAll(uint8_t r, uint8_t g, uint8_t b);
};
