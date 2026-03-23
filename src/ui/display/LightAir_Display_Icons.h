#ifndef LIGHTAIR_DISPLAY_ICONS_H
#define LIGHTAIR_DISPLAY_ICONS_H

#include <Arduino.h>

// ----------------------------------------------------------------
// Semantic icon identifiers — display-agnostic.
// ----------------------------------------------------------------
enum IconType {
    ICON_LIGHT,
    ICON_LIFE,
    ICON_FLAG,
    ICON_HOURGLASS,
    ICON_SCORE,
    ICON_ROLE
};

// ----------------------------------------------------------------
// 8x8 monochrome bitmaps, LSB-first (XBM-compatible).
// ----------------------------------------------------------------
static const uint8_t ICON_LIGHT_BITMAP[8] PROGMEM = {
    0b00111000,
    0b01010100,
    0b10111010,
    0b01111100,
    0b01111100,
    0b10111010,
    0b01010100,
    0b00111000
};

static const uint8_t ICON_LIFE_BITMAP[8] PROGMEM = {
    0b01100110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
    0b00000000,
    0b00000000
};

static const uint8_t ICON_FLAG_BITMAP[8] PROGMEM = {
    0b01111000,
    0b01111000,
    0b01100110,
    0b01111000,
    0b01100000,
    0b01100000,
    0b01100000,
    0b00000000
};

static const uint8_t ICON_HOURGLASS_BITMAP[8] PROGMEM = {
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000,
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111
};

static const uint8_t ICON_SCORE_BITMAP[8] PROGMEM = {
    0b01111110,
    0b11111111,
    0b00111100,
    0b00111100,
    0b00011000,
    0b00111100,
    0b01111110,
    0b00000000
};

static const uint8_t ICON_ROLE_BITMAP[8] PROGMEM = {
    0b01111110,
    0b10111101,
    0b11111111,
    0b10011001,
    0b11111111,
    0b01000010,
    0b00111100,
    0b00000000
};

#endif
