#ifndef LIGHTAIR_DISPLAY_H
#define LIGHTAIR_DISPLAY_H

#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_Display — abstract drawing primitive interface
//
// Concrete implementations wrap a specific display driver
// (e.g. LightAir_SSD1306 wraps SSD1306Wire).
//
// Coordinate system: pixel-based, origin top-left.
// Color model: monochrome — true = on (white), false = off (black).
// Bitmap format: LSB-first (XBM-compatible); concrete implementations
//   that require MSB-first must bit-flip internally.
// Font: fixed implicitly by the concrete implementation.
// ----------------------------------------------------------------
class LightAir_Display {
public:
    virtual ~LightAir_Display() {}

    virtual void clear() = 0;
    virtual void setColor(bool on) = 0;
    virtual void fillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) = 0;
    virtual void drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) = 0;
    virtual void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                            const uint8_t* data) = 0;
    virtual void print(uint8_t x, uint8_t y, const char* text) = 0;
    virtual uint16_t textWidth(const char* text) = 0;
    virtual void flush() = 0;
};

#endif
