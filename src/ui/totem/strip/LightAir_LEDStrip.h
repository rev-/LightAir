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
// An animation is described by a small, orthogonal set of building blocks
// (see StripAnimation): a colour, a ZONE (which LEDs are eligible), an
// EFFECT (footprint + motion), and timing/shape parameters.  Roles bind
// these into recognizable, colour-independent signatures via the totem UI
// controller.
//
// Concrete implementation: LightAir_LEDStrip_HW
// ----------------------------------------------------------------

// Which LEDs an effect is allowed to touch.  See config.h TotemLedLayout.
enum class StripZone : uint8_t {
    All,         // every LED on the strip
    Perimeter,   // the rectangle outline (indices 0–9)
    CenterLine,  // the spine through the middle (indices 10–12)
    Center,      // the single center LED (index 11)
};

// Footprint + motion primitive.
enum class StripEffect : uint8_t {
    Off,          // all LEDs off.
    Fill,         // every LED in the zone lit (steady, or pulses if pulseCount>0).
    Wipe,         // run grows LED-by-LED along the zone, holds full, then resets.
    Pulse,        // every LED in the zone, smooth brightness fade (breathing).
    Blink,        // every LED in the zone, hard on/off toggle.
    BlinkFast,    // Blink at a fixed fast period (~150 ms half-cycle).
    Chase,        // a single lit LED roams around the zone's index list.
    Alternate,    // zone split into two interleaved colours, swapping each half-period.
    Sparse,       // every Nth LED (stride = `density`) lit; twinkles per pulseStyle.
    VerticalScan, // one cross-rectangle "rung" lit, ping-ponging along the length.
};

// How a Sparse (or any brightness-modulated) effect varies over time.
enum class StripPulseStyle : uint8_t {
    Smooth,  // gentle sinusoid/triangle fade — "soft, good" feel.
    Hard,    // sharp on/off flicker — "pointy, bad" feel.
};

struct StripAnimation {
    uint8_t         r, g, b;
    StripEffect     effect;
    uint16_t        durationMs;   // total duration for one-shot; one motion cycle for loops
    // Optional secondary colour for Alternate effect (defaults to off)
    uint8_t         r2, g2, b2;
    // Shape/timing modifiers (all defaulted so existing positional literals
    // keep compiling and behave exactly as before):
    StripZone       zone;         // which LEDs the effect may light
    uint8_t         pulseCount;   // looping background: motion cycles per "beat
                                  //   group" — that many cycles play, then one
                                  //   silent cycle, then repeat (per-team beat).
                                  //   one-shot: total number of cycles to play
                                  //   before yielding to the background.
                                  //   0 = continuous loop / single one-shot cycle.
    uint8_t         density;      // Sparse stride (every Nth LED); smaller = denser.
    StripPulseStyle pulseStyle;   // Sparse / brightness envelope shape.

    StripAnimation()
        : r(0), g(0), b(0), effect(StripEffect::Off), durationMs(0),
          r2(0), g2(0), b2(0),
          zone(StripZone::All), pulseCount(0), density(3),
          pulseStyle(StripPulseStyle::Smooth) {}

    StripAnimation(uint8_t r, uint8_t g, uint8_t b,
                   StripEffect effect, uint16_t durationMs,
                   uint8_t r2 = 0, uint8_t g2 = 0, uint8_t b2 = 0,
                   StripZone zone = StripZone::All,
                   uint8_t pulseCount = 0,
                   uint8_t density = 3,
                   StripPulseStyle pulseStyle = StripPulseStyle::Smooth)
        : r(r), g(g), b(b), effect(effect), durationMs(durationMs),
          r2(r2), g2(g2), b2(b2),
          zone(zone), pulseCount(pulseCount), density(density),
          pulseStyle(pulseStyle) {}
};

class LightAir_LEDStrip {
public:
    static constexpr uint8_t MAX_LEDS = 30;

    // One-shot animations QUEUE rather than replace one another: two players
    // respawning at the same base in the same cycle must each get their own
    // run of the strip, and a role that flashes twice in one rule (FLAG's
    // missing-then-taken pair) must show both.  Overflow past this depth is
    // dropped — a base still animating for a player who left long ago is
    // worse than a missed frame.
    static constexpr uint8_t MAX_ONESHOTS = 10;

    virtual ~LightAir_LEDStrip() {}

    // Queue a one-shot animation.  It plays as soon as the ones already
    // queued finish, overriding the background for its duration.
    virtual void play(const StripAnimation& anim) = 0;

    // Set a looping background animation.  Plays whenever no foreground is active.
    virtual void loop(const StripAnimation& anim) = 0;

    // Stop the looping background (turn strip off).
    virtual void stopLoop() = 0;

    // Advance animation state.  Call every loop tick.
    virtual void update() = 0;
};
