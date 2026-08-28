#pragma once
#include <stdint.h>

// Host stub: monotonic and always advancing, so busy-wait loops that spin on
// it (the AFE settling wait) terminate instead of hanging the test binary.
static inline int64_t esp_timer_get_time() {
    static int64_t t = 0;
    t += 1000;
    return t;
}
