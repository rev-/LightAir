// Host test for the optics LED buffer (EnlightLedWave).
//
// The full- and low-power LED waveforms used to sit in two DMA buffers of a
// whole cycle each.  There is now one, rewritten from a stored period when a
// run changes power.  The DMA must still see exactly what it saw before: a
// whole cycle of the requested power, every period identical.  This pins that
// down, along with the two things the rewrite must not do — run when the
// power did not change, or leave anything of the other power behind.
#include <cstdio>
#include <cstring>
#include <vector>

#include "enlight/EnlightLedWave.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

// Every period of the DMA buffer equals `period`.
static bool holdsEverywhere(const EnlightLedWave& w, const uint8_t* period) {
    const uint32_t pb = w.periodBytes();
    for (size_t off = 0; off < w.bytes(); off += pb)
        if (memcmp(w.dmaBuf() + off, period, pb) != 0) return false;
    return true;
}

// Fraction of clocks the LEDs are lit, both channels together.  The SPI
// output is inverted on its way to the LEDs: a 0 bit is a lit LED.
static double ledOnFraction(const uint8_t* p, uint32_t bytes) {
    uint32_t on = 0;
    for (uint32_t i = 0; i < bytes; i++) on += 8 - __builtin_popcount(p[i]);
    return (double)on / (bytes * 8.0);
}

// The whole contract, on one geometry.
static void exercise(uint32_t clocks, uint32_t periods) {
    EnlightLedWave w;
    CHECK(w.begin(clocks, periods, EnlightDefaults::LOW_POWER_FACTOR), "begin allocates");
    CHECK(w.periodBytes() == clocks / PDM_CLKS_PER_BYTE, "one period is clocks / 4 bytes");
    CHECK(w.bytes() == (size_t)w.periodBytes() * periods, "the buffer is exactly one cycle");

    // ---- Starts at full power, in every period ----
    std::vector<uint8_t> full(w.periodBytes()), low(w.periodBytes());
    EnlightLedWave::generatePeriod(full.data(), clocks, 1.0f);
    EnlightLedWave::generatePeriod(low.data(),  clocks, EnlightDefaults::LOW_POWER_FACTOR);
    CHECK(memcmp(w.period(false), full.data(), full.size()) == 0, "stored full period is the generator's");
    CHECK(memcmp(w.period(true),  low.data(),  low.size())  == 0, "stored low period is the generator's");
    CHECK(!w.holdsLow(), "begin leaves full power in the buffer");
    CHECK(holdsEverywhere(w, full.data()), "every period of the cycle is the full-power one");
    const std::vector<uint8_t> initial(w.dmaBuf(), w.dmaBuf() + w.bytes());

    // ---- Asking for what it already holds writes nothing ----
    CHECK(!w.select(false), "full -> full is not a rewrite");

    // ---- Switch to low power: once, and completely ----
    CHECK(w.select(true), "full -> low rewrites");
    CHECK(w.holdsLow(), "and says so");
    CHECK(holdsEverywhere(w, low.data()), "every period is now the low-power one, the last included");
    CHECK(!w.select(true), "low -> low is not a rewrite");

    // ---- Back to full power: the bytes the run started with ----
    CHECK(w.select(false), "low -> full rewrites");
    CHECK(!w.holdsLow(), "and says so");
    CHECK(memcmp(w.dmaBuf(), initial.data(), initial.size()) == 0,
          "full power after a low-power run is byte-for-byte the first fill");

    // ---- The low period really is the dim one ----
    const double onFull = ledOnFraction(full.data(), w.periodBytes());
    const double onLow  = ledOnFraction(low.data(),  w.periodBytes());
    const double ratio  = onLow / onFull;
    CHECK(memcmp(full.data(), low.data(), full.size()) != 0, "the two powers differ");
    CHECK(ratio > EnlightDefaults::LOW_POWER_FACTOR * 0.8 &&
          ratio < EnlightDefaults::LOW_POWER_FACTOR * 1.2,
          "low power lights the LEDs LOW_POWER_FACTOR as long as full power");
    printf("  %u clocks x %u periods: LEDs lit %.1f%% at full, %.1f%% at low (x%.3f)\n",
           (unsigned)clocks, (unsigned)periods, onFull * 100.0, onLow * 100.0, ratio);
}

int main() {
    // V6R2 geometry, as Enlight::generateWaveform derives it: 16 MHz / 1667 Hz
    // rounded to the 48-clock grain is 9600 clocks (2400 bytes) a period, and
    // 32767 / 2400 = 13 periods fit one DMA transfer.
    printf("V6R2 geometry:\n");
    exercise(9600, 13);

    // Nothing may assume 13: a different LED frequency changes both numbers.
    printf("other geometry:\n");
    exercise(4800, 27);

    printf("\n%s\n", failures ? "LEDWAVE TESTS FAILED" : "LEDWAVE TESTS PASS");
    return failures ? 1 : 0;
}
