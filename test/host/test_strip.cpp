// Host test for the totem LED strip's one-shot queue.
//
// One-shots used to replace one another: two players respawning at the same
// base in the same cycle showed one animation, and a role that flashes twice
// in one rule (FLAG's missing-then-taken pair) showed only the second.  They
// now queue and play in arrival order, which is what this pins down.
#include <cstdio>

#include "Arduino.h"
uint32_t g_millis = 1000;

#include "FastLED.h"
CFastLED FastLED;

#include "ui/totem/strip/LightAir_LEDStrip_HW.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

// A flat one-cycle fill in a recognisable colour: whatever is on screen,
// pixel 0 names it.
static StripAnimation fill(uint8_t r, uint16_t ms) {
    return StripAnimation(r, 0, 0, StripEffect::Fill, ms,
                          0, 0, 0, StripZone::All, /*pulseCount*/ 1);
}

// Advance time and render one frame, the way the driver's loop does.
static uint8_t frameAt(LightAir_LEDStrip_HW& s, uint32_t atMs) {
    g_millis = atMs;
    s.update();
    return FastLED.pixels ? FastLED.pixels[0].r : 0;
}

int main() {
    LightAir_LEDStrip_HW strip;
    strip.begin(13, 13);

    // ---- 1. Two one-shots queued in one tick both play, in order ----
    {
        printf("queue order:\n");
        g_millis = 1000;
        strip.loop(fill(9, 1000));            // background marker
        strip.play(fill(100, 500));           // first  one-shot
        strip.play(fill(200, 500));           // second one-shot

        CHECK(frameAt(strip, 1100) == 100, "first one-shot on screen");
        CHECK(frameAt(strip, 1400) == 100, "still the first at 400ms");
        CHECK(frameAt(strip, 1600) == 200, "second takes over when the first ends");
        CHECK(frameAt(strip, 1900) == 200, "second still running at 900ms");
        CHECK(frameAt(strip, 2100) == 9,   "background resumes once the queue drains");
    }

    // ---- 2. The queue is bounded; overflow is dropped, not wrapped ----
    {
        printf("queue bound:\n");
        g_millis = 5000;
        strip.loop(fill(9, 1000));
        for (uint8_t i = 0; i < LightAir_LEDStrip::MAX_ONESHOTS; i++)
            strip.play(fill((uint8_t)(10 + i), 100));
        strip.play(fill(250, 100));           // one too many

        // Walk the whole queue: it must be the first MAX_ONESHOTS, in order,
        // with the overflow entry absent rather than displacing anyone.
        for (uint8_t i = 0; i < LightAir_LEDStrip::MAX_ONESHOTS; i++) {
            uint8_t got = frameAt(strip, 5000 + (uint32_t)i * 100 + 50);
            CHECK(got == (uint8_t)(10 + i), "queued one-shots play in order");
        }
        CHECK(frameAt(strip, 5000 + 100u * LightAir_LEDStrip::MAX_ONESHOTS + 50) == 9,
              "dropped overflow did not extend the queue");
    }

    // ---- 3. A one-shot arriving mid-play waits its turn ----
    {
        printf("late arrival:\n");
        g_millis = 9000;
        strip.loop(fill(9, 1000));
        strip.play(fill(100, 500));
        CHECK(frameAt(strip, 9200) == 100, "first playing");
        strip.play(fill(200, 500));            // arrives while the first runs
        CHECK(frameAt(strip, 9300) == 100, "late arrival does not interrupt");
        CHECK(frameAt(strip, 9600) == 200, "late arrival plays after it");
    }

    printf("\n%s\n", failures ? "STRIP TESTS FAILED" : "STRIP TESTS PASS");
    return failures ? 1 : 0;
}
