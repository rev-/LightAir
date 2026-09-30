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

// Deterministic integer hash (a murmur3-style finaliser): the twinkle's
// "random" choices are a pure function of LED index and cycle number, so a
// frame is reproducible — which is what lets the host test pin the look.
inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

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
    if (_fgCount >= MAX_ONESHOTS) return;          // queue full — drop
    uint8_t tail = (uint8_t)((_fgHead + _fgCount) % MAX_ONESHOTS);
    _fg[tail] = anim;
    if (_fgCount == 0) _fgStartMs = millis();      // nothing playing: start now
    _fgCount++;
}

// durationMs is one motion cycle; a one-shot plays pulseCount of them (or a
// single cycle when pulseCount == 0).  Off is instant.
uint32_t LightAir_LEDStrip_HW::oneShotTotal(const StripAnimation& a) {
    if (a.effect == StripEffect::Off) return 0;
    uint16_t period = a.durationMs ? a.durationMs : 1000;
    return (a.pulseCount > 0) ? (uint32_t)a.pulseCount * period : period;
}

void LightAir_LEDStrip_HW::loop(const StripAnimation& anim) {
    _bg        = anim;
    _bgActive  = true;
    _bgStartMs = millis();
}

void LightAir_LEDStrip_HW::stopLoop() {
    _bgActive = false;
    if (_fgCount == 0) {
        setAll(0, 0, 0);
        FastLED.show();
    }
}

// ----------------------------------------------------------------
void LightAir_LEDStrip_HW::update() {
    uint32_t now = millis();

    // Drain any finished one-shots first, so a zero-length or already-elapsed
    // entry hands over within the same tick instead of costing a frame.
    while (_fgCount > 0 && (now - _fgStartMs) >= oneShotTotal(_fg[_fgHead])) {
        _fgHead = (uint8_t)((_fgHead + 1) % MAX_ONESHOTS);
        _fgCount--;
        _fgStartMs = now;      // the next one starts here; background too
        _bgStartMs = now;
    }

    if (_fgCount > 0) {
        renderAnim(_fg[_fgHead], now - _fgStartMs);
        FastLED.show();
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
            // A twinkle.  Every LED runs its own cycle of `period`, offset
            // from the others, and in each of its cycles lights with a
            // 1-in-`density` chance — so which LEDs shine changes all the
            // time and no two pulse in step.  The envelope carries the
            // meaning without relying on colour:
            //   Smooth — fades up and back down over the whole cycle;
            //   Hard   — full on for the first third, then dark.
            // density 1 keeps every LED in the same cycle: a synchronous
            // flash, which is what a claim burst wants.
            const uint8_t chance = a.density ? a.density : 3;
            const uint8_t n      = zoneCount(a.zone);
            for (uint8_t i = 0; i < n; i++) {
                const uint8_t led = zoneLed(a.zone, i);
                if (led >= _numLeds) continue;

                uint32_t ph = cyclePhase;
                if (chance > 1) {
                    const uint32_t local = elapsed + mix32(led * 2654435761U) % period;
                    const uint32_t cycle = local / period;
                    ph = local % period;
                    if (mix32(led ^ (cycle * 0x9e3779b9U)) % chance != 0) continue;
                }

                uint8_t bright;
                if (a.pulseStyle == StripPulseStyle::Hard) {
                    if (ph >= (uint32_t)(period / 3)) continue;
                    bright = 255;
                } else {
                    const uint8_t p = (uint8_t)((ph * 255) / period);
                    bright = (p < 128) ? (uint8_t)(p * 2) : (uint8_t)((255 - p) * 2);
                }
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
