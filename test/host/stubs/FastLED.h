// Host-test stub of FastLED: enough of the API for LightAir_LEDStrip_HW.
//
// addLeds() keeps the pixel buffer pointer the way the real library does,
// which is also how a test reads back what a frame actually rendered.
#pragma once
#include <stdint.h>

struct CRGB {
    uint8_t r, g, b;
    // Trivially default-constructible, like the real CRGB — the strip
    // memsets its pixel buffer.
    CRGB() = default;
    CRGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
};

enum EOrder { RGB = 0, RBG, GRB, GBR, BRG, BGR };

template <uint8_t DATA_PIN, EOrder O> class WS2812B {};

class CFastLED {
public:
    template <template <uint8_t, EOrder> class CHIPSET, uint8_t DATA_PIN, EOrder O>
    void addLeds(CRGB* leds, int n) { pixels = leds; count = n; }
    void setBrightness(uint8_t) {}
    void show() { shows++; }

    CRGB* pixels = nullptr;   // the strip's own buffer, as handed to addLeds
    int   count  = 0;
    int   shows  = 0;
};

extern CFastLED FastLED;
