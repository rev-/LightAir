#include "LightAir_SSD1306Display.h"

LightAir_SSD1306Display::LightAir_SSD1306Display(uint8_t sda, uint8_t scl, uint8_t address)
    : _hw(address, sda, scl) {}

void LightAir_SSD1306Display::begin() {
    _hw.init();
    _hw.flipScreenVertically();
    _hw.setFont(ArialMT_Plain_10);
    _hw.clear();
    _hw.display();
}

void LightAir_SSD1306Display::clear() {
    _hw.clear();
}

void LightAir_SSD1306Display::setColor(bool on) {
    _hw.setColor(on ? WHITE : BLACK);
}

void LightAir_SSD1306Display::fillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    _hw.fillRect(x, y, w, h);
}

void LightAir_SSD1306Display::drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    _hw.drawRect(x, y, w, h);
}

void LightAir_SSD1306Display::drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                                   const uint8_t* data) {
    _hw.drawXbm(x, y, w, h, (const uint8_t*)data);
}

void LightAir_SSD1306Display::print(uint8_t x, uint8_t y, const char* text) {
    _hw.drawString(x, y, String(text));
}

uint16_t LightAir_SSD1306Display::textWidth(const char* text) {
    return _hw.getStringWidth(String(text));
}

void LightAir_SSD1306Display::flush() {
    _hw.display();
}
