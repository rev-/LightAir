#include "LightAir_LEDStrip_HW.h"
#include <string.h>

void LightAir_LEDStrip_HW::begin(int dataPin, uint8_t numLeds) {
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
    _fgStep    = 0;
}

void LightAir_LEDStrip_HW::loop(const StripAnimation& anim) {
    _bg        = anim;
    _bgActive  = true;
    _bgCycleMs = millis();
    _bgStep    = 0;
    _bgPhase   = false;
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
        renderAnim(_fg, elapsed, _fgStep, _bgPhase /*unused for fg*/, false);
        FastLED.show();

        // Check completion
        bool done = false;
        switch (_fg.effect) {
            case StripEffect::Off:
            case StripEffect::Fill:
                done = true;
                break;
            case StripEffect::Wipe:
            case StripEffect::Chase:
                done = (_fgStep >= _numLeds);
                break;
            default:
                done = (elapsed >= _fg.durationMs);
                break;
        }
        if (done) {
            _fgActive = false;
            // Reset bg cycle so it starts fresh
            _bgCycleMs = now;
            _bgStep    = 0;
            _bgPhase   = false;
        }
        return;
    }

    if (_bgActive) {
        uint32_t elapsed = now - _bgCycleMs;
        uint16_t period  = _bg.durationMs ? _bg.durationMs : 1000;

        // Advance step / phase for per-LED effects
        switch (_bg.effect) {
            case StripEffect::Wipe:
            case StripEffect::Chase:
                _bgStep = (uint8_t)((elapsed / 50) % _numLeds);
                break;
            case StripEffect::Blink:
                _bgPhase = ((elapsed / (period / 2)) & 1) != 0;
                break;
            case StripEffect::BlinkFast:
                _bgPhase = ((elapsed / 150) & 1) != 0;
                break;
            case StripEffect::Alternate:
                _bgPhase = ((elapsed / (period / 2)) & 1) != 0;
                break;
            case StripEffect::Pulse:
                // Use elapsed within one period
                break;
            default:
                break;
        }

        bool dummy = _bgPhase;
        renderAnim(_bg, elapsed % period, _bgStep, dummy, true);
        FastLED.show();
    }
}

// ----------------------------------------------------------------
void LightAir_LEDStrip_HW::renderAnim(const StripAnimation& a,
                                       uint32_t elapsed,
                                       uint8_t& step,
                                       bool&    phase,
                                       bool     looping) {
    uint16_t period = a.durationMs ? a.durationMs : 1000;

    switch (a.effect) {
        case StripEffect::Off:
            setAll(0, 0, 0);
            break;

        case StripEffect::Fill:
            setAll(a.r, a.g, a.b);
            break;

        case StripEffect::Wipe: {
            // Advance to the LED that should be lit now
            uint8_t target = (uint8_t)(elapsed / 50);  // ~50 ms per LED
            if (target > _numLeds) target = _numLeds;
            if (target > step) {
                // Light up to target, leaving trail on
                for (uint8_t i = step; i < target && i < _numLeds; i++)
                    _leds[i] = CRGB(a.r, a.g, a.b);
                step = target;
            }
            break;
        }

        case StripEffect::Pulse: {
            // Sine-approximated brightness: full→dim→full over period
            uint32_t t     = elapsed % period;
            uint8_t  phase2 = (uint8_t)((t * 255) / period);
            // Triangle wave: 0→255→0
            uint8_t  bright = (phase2 < 128) ? (phase2 * 2) : ((255 - phase2) * 2);
            uint8_t  minB   = 20;
            bright = minB + (uint8_t)((bright * (255 - minB)) / 255);
            _leds[0] = CRGB(a.r, a.g, a.b);
            for (uint8_t i = 0; i < _numLeds; i++) {
                _leds[i].r = (uint8_t)((a.r * bright) / 255);
                _leds[i].g = (uint8_t)((a.g * bright) / 255);
                _leds[i].b = (uint8_t)((a.b * bright) / 255);
            }
            break;
        }

        case StripEffect::Blink:
        case StripEffect::BlinkFast:
            if (!phase) setAll(a.r, a.g, a.b);
            else        setAll(0, 0, 0);
            break;

        case StripEffect::Chase: {
            setAll(0, 0, 0);
            uint8_t pos = step % _numLeds;
            _leds[pos] = CRGB(a.r, a.g, a.b);
            break;
        }

        case StripEffect::Alternate:
            setAlternate(a.r, a.g, a.b, a.r2, a.g2, a.b2, phase);
            break;
    }
}

void LightAir_LEDStrip_HW::setAll(uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < _numLeds; i++)
        _leds[i] = CRGB(r, g, b);
}

void LightAir_LEDStrip_HW::setAlternate(uint8_t r1, uint8_t g1, uint8_t b1,
                                          uint8_t r2, uint8_t g2, uint8_t b2,
                                          bool phase) {
    for (uint8_t i = 0; i < _numLeds; i++) {
        bool even = (i & 1) == 0;
        if (even ^ phase) _leds[i] = CRGB(r1, g1, b1);
        else              _leds[i] = CRGB(r2, g2, b2);
    }
}
