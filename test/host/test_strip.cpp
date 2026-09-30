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
#include "ui/totem/LightAir_TotemUICtrl.h"

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


    // ---- 4. BONUS / MALUS idles: a real twinkle, told apart without colour --
    // They used to light a fixed LED subset (every 4th: one per side, none on
    // the bottom) pulsing in unison.  Driven through the real totem UI
    // controller, so the shipped periods and densities are what is pinned.
    {
        printf("bonus/malus twinkle:\n");
        struct NoRGB : LightAir_TotemRGB {
            void set(uint8_t, uint8_t, uint8_t) override {}
            void off() override {}
        };
        struct Look {
            bool     everLit[13] = {};
            int      distinctSets = 0;   // how many different lit-sets were seen
            int      midLevels    = 0;   // frames with a pixel strictly between 0 and full
            int      onEdges      = 0;   // off->on transitions, all LEDs
            int      maxLit       = 0;
        };
        auto watch = [&](TotemUIEvent ev, uint8_t r, uint8_t g) {
            LightAir_LEDStrip_HW s;
            s.begin(13, 13);
            NoRGB rgb;
            LightAir_TotemUICtrl ui(rgb, s);
            TotemUIOutput out;
            out.trigger(ev, r, g, 0);
            g_millis = 20000;
            ui.apply(out);
            Look L;
            uint16_t seen[64]; int nSeen = 0;
            bool was[13] = {};
            for (uint32_t t = 0; t < 10000; t += 10) {
                g_millis = 20000 + t;
                ui.update();
                uint16_t mask = 0; int lit = 0; bool mid = false;
                for (int i = 0; i < 13; i++) {
                    const uint8_t v = FastLED.pixels[i].r | FastLED.pixels[i].g;
                    const bool on = v > 0;
                    if (on) { mask |= (uint16_t)(1u << i); lit++; L.everLit[i] = true; }
                    if (on && !was[i]) L.onEdges++;
                    was[i] = on;
                    const uint8_t full = r | g;
                    if (v > 0 && v < full) mid = true;
                }
                if (mid) L.midLevels++;
                if (lit > L.maxLit) L.maxLit = lit;
                bool known = false;
                for (int k = 0; k < nSeen; k++) if (seen[k] == mask) known = true;
                if (!known && nSeen < 64) seen[nSeen++] = mask;
            }
            L.distinctSets = nSeen;
            return L;
        };
        const Look B = watch(TotemUIEvent::BonusIdle,   0, 180);
        const Look M = watch(TotemUIEvent::MalusIdle, 200,   0);

        bool allB = true, allM = true;
        for (int i = 0; i < 13; i++) { allB &= B.everLit[i]; allM &= M.everLit[i]; }
        CHECK(allB, "BONUS: every LED takes part, bottom side included");
        CHECK(allM, "MALUS: every LED takes part, bottom side included");
        CHECK(B.distinctSets > 20 && M.distinctSets > 20,
              "the lit LEDs change over time (not one fixed subset)");
        CHECK(B.maxLit < 13 && M.maxLit < 13, "an idle twinkles; it never floods the frame");
        CHECK(B.midLevels > 500, "BONUS is soft: LEDs fade through in-between levels");
        CHECK(M.midLevels == 0,  "MALUS is hard: every LED is fully on or off");
        CHECK(M.onEdges > 3 * B.onEdges, "MALUS blinks several times faster than BONUS");
        printf("  bonus: sets=%d on-edges=%d  malus: sets=%d on-edges=%d\n",
               B.distinctSets, B.onEdges, M.distinctSets, M.onEdges);
    }

    printf("\n%s\n", failures ? "STRIP TESTS FAILED" : "STRIP TESTS PASS");
    return failures ? 1 : 0;
}
