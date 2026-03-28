#pragma once
#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_LEDStrip — abstract interface for a totem LED strip.
//
// Provides one-shot and looping animations over a LED strip.
// update() must be called every game loop tick (~10 ms).
//
// Animation model:
//   • A looping "background" animation runs continuously.
//   • A one-shot "foreground" animation temporarily overrides it.
//   • When the foreground finishes, the background resumes.
//
// Concrete implementation: LightAir_LEDStrip_HW
// ----------------------------------------------------------------

enum class StripEffect : uint8_t {
    Off,
    Fill,
    Wipe,
    Pulse,
    Blink,
    BlinkFast,
    Chase,
    Alternate,
};

struct StripAnimation {
    uint8_t     r, g, b;
    StripEffect effect;
    uint16_t    durationMs;  // total duration for one-shot; period for loops
    // Optional secondary colour for Alternate effect (defaults to off)
    uint8_t     r2, g2, b2;
    StripAnimation()
        : r(0), g(0), b(0), effect(StripEffect::Off), durationMs(0), r2(0), g2(0), b2(0) {}
    StripAnimation(uint8_t r, uint8_t g, uint8_t b,
                   StripEffect effect, uint16_t durationMs,
                   uint8_t r2 = 0, uint8_t g2 = 0, uint8_t b2 = 0)
        : r(r), g(g), b(b), effect(effect), durationMs(durationMs), r2(r2), g2(g2), b2(b2) {}
};

class LightAir_LEDStrip {
public:
    static constexpr uint8_t MAX_LEDS = 30;

    virtual ~LightAir_LEDStrip() {}

    // Play a one-shot animation.  Overrides the background until complete.
    virtual void play(const StripAnimation& anim) = 0;

    // Set a looping background animation.  Plays whenever no foreground is active.
    virtual void loop(const StripAnimation& anim) = 0;

    // Stop the looping background (turn strip off).
    virtual void stopLoop() = 0;

    // Advance animation state.  Call every loop tick.
    virtual void update() = 0;
};
