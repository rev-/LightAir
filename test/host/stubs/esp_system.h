#pragma once
// Counts instead of rebooting, so a test can assert that a restart was (or
// was not) asked for.  Weak, like g_millisStep: one definition wins across
// every translation unit that includes this.
__attribute__((weak)) int g_restartCalls = 0;
static inline void esp_restart() { g_restartCalls++; }
