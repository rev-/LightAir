#include "LightAir_LEDStrip_HW.h"
#include "../../../config.h"
#include <string.h>

namespace {
// Stations for the VerticalScan effect (cross-rectangle "rungs", spine end
// to spine end).  Indexed via the flat tables in TotemLedLayout.
const uint8_t* kStations[TotemLedLayout::kStationCount] = {
    TotemLedLayout::kStation0, TotemLedLayout::kStation1,
    TotemLedLayout::kStation2, TotemLedLayout::kStation3,
    TotemLedLayout::kStation4,
};
const uint8_t kStationSizes[TotemLedLayout::kStationCount] = { 2, 3, 3, 3, 2 };

// Scale an 8-bit colour channel by an 8-bit brightness (0..255).
inline uint8_t scale8c(uint8_t c, uint8_t b) {
    return (uint8_t)(((uint16_t)c * b) / 255);
}
}  // namespace

void LightAir_LEDStrip_HW::begin(int dataPin, uint8_t numLeds) {
    (void)dataPin;
    _numLeds = (numLeds > MAX_LEDS) ? MAX_LEDS : numLeds;
    // FastLED requires a compile-time pin; we default to the reference hardware
    // pin (13) here.  Override by subclassing or adjusting for your hardware.
    FastLED.addLeds<WS2812B, 13, GRB>(_leds, _numLeds);
    FastLED.setBrightness(255);
    memset(_leds, 0, sizeof(_leds));
    FastLED.show();
}

void LightAir_LEDStrip_HW::play(const StripAnimation& anim) {
    _fg        = anim;
    _fgActive  = true;
    _fgStartMs = millis();
}

void LightAir_LEDStrip_HW::loop(const StripAnimation& anim) {
    _bg        = anim;
    _bgActive  = true;
    _bgStartMs = millis();
}

void LightAir_LEDStrip_HW::stopLoop() {
    _bgActive = false;
    if (!_fgActive) {
        setAll(0, 0, 0);
        FastLED.show();
    }
}

// ----------------------------------------------------------------
void LightAir_LEDStrip_HW::update() {
    uint32_t now = millis();

    if (_fgActive) {
        uint32_t elapsed = now - _fgStartMs;
        renderAnim(_fg, elapsed);
        FastLED.show();

        // One-shot completion.  durationMs is one motion cycle; a one-shot
        // plays pulseCount cycles (or a single cycle when pulseCount == 0),
        // then yields back to the background.  Off is instant.
        uint16_t period = _fg.durationMs ? _fg.durationMs : 1000;
        uint32_t total  = (_fg.pulseCount > 0)
                              ? (uint32_t)_fg.pulseCount * period
                              : period;
        bool done = (_fg.effect == StripEffect::Off) || (elapsed >= total);
        if (done) {
            _fgActive  = false;
            _bgStartMs = now;  // restart the background cleanly
        }
        return;
    }

    if (_bgActive) {
        renderAnim(_bg, now - _bgStartMs);
        FastLED.show();
    }
}

// ----------------------------------------------------------------
// Zone helpers.
uint8_t LightAir_LEDStrip_HW::zoneCount(StripZone zone) const {
    switch (zone) {
        case StripZone::Perimeter:
            return (TotemLedLayout::kPerimeterCount < _numLeds)
                       ? TotemLedLayout::kPerimeterCount : _numLeds;
        case StripZone::CenterLine:
            return TotemLedLayout::kCenterLineCount;
        case StripZone::Center:
            return 1;
        case StripZone::All:
        default:
            return _numLeds;
    }
}

uint8_t LightAir_LEDStrip_HW::zoneLed(StripZone zone, uint8_t i) const {
    switch (zone) {
        case StripZone::Perimeter:  return TotemLedLayout::kPerimeter[i];
        case StripZone::CenterLine: return TotemLedLayout::kCenterLine[i];
        case StripZone::Center:     return TotemLedLayout::kCenter;
        case StripZone::All:
        default:                    return i;
    }
}

void LightAir_LEDStrip_HW::setZone(StripZone zone, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t n = zoneCount(zone);
    for (uint8_t i = 0; i < n; i++) {
        uint8_t led = zoneLed(zone, i);
        if (led < _numLeds) _leds[led] = CRGB(r, g, b);
    }
}

void LightAir_LEDStrip_HW::setAll(uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < _numLeds; i++)
        _leds[i] = CRGB(r, g, b);
}

// ----------------------------------------------------------------
void LightAir_LEDStrip_HW::renderAnim(const StripAnimation& a, uint32_t elapsed) {
    setAll(0, 0, 0);

    if (a.effect == StripEffect::Off) return;

    uint16_t period = a.durationMs ? a.durationMs : 1000;

    // ---- Beat-group bookkeeping (pulseCount cycles, then one silent cycle) ----
    // pulseCount == 0 → continuous: a single, ever-repeating cycle.
    uint32_t cyclePhase;  // ms within the current motion cycle [0, period)
    if (a.pulseCount == 0) {
        cyclePhase = elapsed % period;
    } else {
        uint32_t group   = (uint32_t)(a.pulseCount + 1) * period;
        uint32_t inGroup = elapsed % group;
        if (inGroup >= (uint32_t)a.pulseCount * period)
            return;                       // silent beat → leave all off
        cyclePhase = inGroup % period;
    }

    switch (a.effect) {
        case StripEffect::Off:
            break;

        case StripEffect::Fill:
            setZone(a.zone, a.r, a.g, a.b);
            break;

        case StripEffect::Pulse: {
            // Triangle 0→255→0 across the cycle, floored so it never fully dies.
            uint8_t p      = (uint8_t)((cyclePhase * 255) / period);
            uint8_t tri    = (p < 128) ? (uint8_t)(p * 2) : (uint8_t)((255 - p) * 2);
            uint8_t minB   = 20;
            uint8_t bright = minB + (uint8_t)(((uint16_t)tri * (255 - minB)) / 255);
            setZone(a.zone, scale8c(a.r, bright), scale8c(a.g, bright), scale8c(a.b, bright));
            break;
        }

        case StripEffect::Blink:
            // On for the first half of the cycle, off for the second.
            if (cyclePhase < (uint32_t)(period / 2))
                setZone(a.zone, a.r, a.g, a.b);
            break;

        case StripEffect::BlinkFast: {
            // Fixed fast toggle, independent of period.
            bool on = ((elapsed / 150) & 1) == 0;
            if (on) setZone(a.zone, a.r, a.g, a.b);
            break;
        }

        case StripEffect::Wipe: {
            // Run grows along the zone, holds full, then resets next cycle.
            uint8_t n = zoneCount(a.zone);
            if (n == 0) break;
            uint8_t target = (uint8_t)(((uint32_t)cyclePhase * n) / period) + 1;
            if (target > n) target = n;
            for (uint8_t i = 0; i < target; i++) {
                uint8_t led = zoneLed(a.zone, i);
                if (led < _numLeds) _leds[led] = CRGB(a.r, a.g, a.b);
            }
            break;
        }

        case StripEffect::Chase: {
            uint8_t n = zoneCount(a.zone);
            if (n == 0) break;
            uint8_t pos = (uint8_t)(((uint32_t)cyclePhase * n) / period) % n;
            uint8_t led = zoneLed(a.zone, pos);
            if (led < _numLeds) _leds[led] = CRGB(a.r, a.g, a.b);
            break;
        }

        case StripEffect::Alternate: {
            // Interleave two colours within the zone, swapping each half-cycle.
            bool phase = cyclePhase >= (uint32_t)(period / 2);
            uint8_t n  = zoneCount(a.zone);
            for (uint8_t i = 0; i < n; i++) {
                uint8_t led = zoneLed(a.zone, i);
                if (led >= _numLeds) continue;
                bool even = (i & 1) == 0;
                if (even ^ phase) _leds[led] = CRGB(a.r,  a.g,  a.b);
                else              _leds[led] = CRGB(a.r2, a.g2, a.b2);
            }
            break;
        }

        case StripEffect::Sparse: {
            // Every `density`-th LED in the zone, brightness-modulated.
            uint8_t stride = a.density ? a.density : 3;
            uint8_t bright;
            if (a.pulseStyle == StripPulseStyle::Hard) {
                bright = (cyclePhase < (uint32_t)(period / 2)) ? 255 : 40;
            } else {
                uint8_t p   = (uint8_t)((cyclePhase * 255) / period);
                uint8_t tri = (p < 128) ? (uint8_t)(p * 2) : (uint8_t)((255 - p) * 2);
                bright = 40 + (uint8_t)(((uint16_t)tri * (255 - 40)) / 255);
            }
            uint8_t n = zoneCount(a.zone);
            for (uint8_t i = 0; i < n; i += stride) {
                uint8_t led = zoneLed(a.zone, i);
                if (led < _numLeds)
                    _leds[led] = CRGB(scale8c(a.r, bright),
                                      scale8c(a.g, bright),
                                      scale8c(a.b, bright));
            }
            break;
        }

        case StripEffect::VerticalScan: {
            // One "rung" lit at a time, ping-ponging end-to-end along the
            // length of the rectangle.  Ignores a.zone (spans the geometry).
            const uint8_t n = TotemLedLayout::kStationCount;          // 5 stations
            const uint8_t span = (n > 1) ? (2 * (n - 1)) : 1;        // 0..n-1..1 = 8
            uint8_t pos = (uint8_t)(((uint32_t)cyclePhase * span) / period) % span;
            uint8_t st  = (pos < n) ? pos : (uint8_t)(span - pos);   // ping-pong
            const uint8_t* station = kStations[st];
            for (uint8_t i = 0; i < kStationSizes[st]; i++) {
                uint8_t led = station[i];
                if (led < _numLeds) _leds[led] = CRGB(a.r, a.g, a.b);
            }
            break;
        }
    }
}
