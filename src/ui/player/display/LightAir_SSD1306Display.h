#ifndef LIGHTAIR_SSD1306DISPLAY_H
#define LIGHTAIR_SSD1306DISPLAY_H

#include "LightAir_Display.h"
#include <SSD1306Wire.h>

// ----------------------------------------------------------------
// LightAir_SSD1306Display — concrete display driver wrapping SSD1306Wire.
//
// Call begin() once before passing this to LightAir_DisplayCtrl.
// ----------------------------------------------------------------
class LightAir_SSD1306Display : public LightAir_Display {
public:
    LightAir_SSD1306Display(uint8_t sda, uint8_t scl, uint8_t address = 0x3C);

    // Hardware init: must be called before LightAir_DisplayCtrl::begin().
    void begin();

    void     clear()    override;
    void     setColor(bool on) override;
    void     fillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) override;
    void     drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) override;
    void     drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                        const uint8_t* data) override;
    void     print(uint8_t x, uint8_t y, const char* text) override;
    uint16_t textWidth(const char* text) override;
    void     flush() override;

private:
    SSD1306Wire _hw;
};

#endif
