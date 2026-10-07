#pragma once
#include <stdint.h>

// Host stub for the ESP Ticker.
//
// The real one schedules a callback after N ms.  Nothing here runs the
// callback — the host tests are not driving real time — but the INTERVAL is
// recorded, because that is the observable part of a UI action: what a
// LightAir_UICtrl step schedules is what the player hears.  Asserting on it
// is how the burst-length arithmetic is checked through the code that uses
// it, rather than only in isolation.
//
// A function-local static rather than an extern, so every translation unit
// that includes this header links without a definition of its own.
struct HostTickerLog {
    uint32_t lastMs;   // interval of the most recent schedule
    uint32_t calls;    // how many schedules have happened
};

inline HostTickerLog& hostTicker() {
    static HostTickerLog t = { 0, 0 };
    return t;
}

struct Ticker {
    template <typename F, typename A> void attach_ms(uint32_t ms, F, A) {
        hostTicker().lastMs = ms;
        hostTicker().calls++;
    }
    template <typename F, typename A> void once_ms(uint32_t ms, F, A) {
        hostTicker().lastMs = ms;
        hostTicker().calls++;
    }
    void detach() {}
};
