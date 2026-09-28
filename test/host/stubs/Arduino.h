// Host-test stub of the Arduino core.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
extern uint32_t g_millis;
// Opt-in auto-advance: GameRunner::update() busy-waits on millis() for the
// rest of its 10 ms loop, which never ends on a frozen clock.  A test that
// drives update() sets this to 1 for the duration; 0 keeps time frozen.
__attribute__((weak)) uint32_t g_millisStep = 0;
static inline uint32_t millis() { return g_millis += g_millisStep; }
static inline void delay(uint32_t) {}
#define PROGMEM

// ESP-class stub (restart is a no-op on host).
struct HostEsp { void restart() {} };
static HostEsp ESP;
