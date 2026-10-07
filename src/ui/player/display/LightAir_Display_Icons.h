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
    ICON_ROLE,
    ICON_ENERGY,
    ICON_DOWN,
    ICON_SPLASH,
    ICON_FAST,
    ICON_LONG,
    ICON_STRONG,
    ICON_COUNT,                  // real icons above this; not an icon itself
    ICON_TIME = ICON_HOURGLASS   // alias — reuses the hourglass CGRAM slot
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

// Radiation trefoil — energy / projection-light icon.
static const uint8_t ICON_ENERGY_BITMAP[8] PROGMEM = {

    0b01000010,
    0b01100110,
    0b00111100,
    0b11100111,
    0b01100110,
    0b00111100,
    0b01100110,
    0b01000010
};

// Downward arrow — depletions icon.
static const uint8_t ICON_DOWN_BITMAP[8] PROGMEM = {

    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b10011001,
    0b01011010,
    0b00111100,
    0b00011000
};

// SPLASH — a burst: a solid core with specks thrown clear of it.  Reads at
// 8x8 as "this one goes off around you", which is the whole point of the
// profile that carries it.
static const uint8_t ICON_SPLASH_BITMAP[8] PROGMEM = {

    0b10010010,
    0b01000100,
    0b00111000,
    0b10111010,
    0b00111000,
    0b01000100,
    0b10010010,
    0b00000000
};

// FAST — a forward chevron pair: quick, light, short.
static const uint8_t ICON_FAST_BITMAP[8] PROGMEM = {

    0b00100010,
    0b01000100,
    0b10001000,
    0b01000100,
    0b00100010,
    0b00000000,
    0b00000000,
    0b00000000
};

// LONG — a beam narrowing to a distant point.
static const uint8_t ICON_LONG_BITMAP[8] PROGMEM = {

    0b00000000,
    0b11000000,
    0b01110000,
    0b00111110,
    0b01110000,
    0b11000000,
    0b00000000,
    0b00000000
};

// STRONG — a filled burst: nothing thrown clear, unlike SPLASH.
static const uint8_t ICON_STRONG_BITMAP[8] PROGMEM = {

    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000
};

#endif
