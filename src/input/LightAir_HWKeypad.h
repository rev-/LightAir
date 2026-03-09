#pragma once
#include <Arduino.h>
#include "LightAir_Keypad.h"
#include "../config.h"

// Matrix keypad driver — no external library required.
// Scanning algorithm adapted from the Keypad library by
// Mark Stanley and Alexander Brevig (MIT License, 2009).
//
// Wiring convention:
//   Row pins : configured as OUTPUT; driven LOW one at a time during scan.
//   Col pins : configured as INPUT_PULLUP; read while row is driven LOW.
//   Active-low: a pressed key pulls the column LOW.
//
// Debounce is non-blocking: a transition is accepted only after the
// raw reading has been stable for debounceMs milliseconds.
// Row settling (~10 µs per row) uses delayMicroseconds(); all other
// timing uses millis() comparison so getEvents() is not blocked.
//
// ----------------------------------------------------------------
// V6R2 hardware — 2×3 key layout
//
//   Char map (row-major, passed as keys[] to constructor):
//     { '<', '^', '>' }   ← row 0  (SW_R1, GPIO 7)
//     { 'A', 'V', 'B' }   ← row 1  (SW_R2, GPIO 17)
//       col:  C1   C2  C3
//             16   15  18  (SW_C1 / SW_C2 / SW_C3)
//
//   Physical layout:
//     ┌───┬───┬───┐
//     │ < │ ^ │ > │
//     ├───┼───┼───┤
//     │ A │ V │ B │
//     └───┴───┴───┘
//
//   Key meanings used throughout the library:
//     '<'  LEFT  arrow  — decrease value / navigate left
//     '^'  UP    arrow  — navigate up   / previous item
//     '>'  RIGHT arrow  — increase value / navigate right
//     'A'  Accept / OK  — confirm selection
//     'V'  DOWN  arrow  — navigate down / next item
//     'B'  Back / Cancel— go back / cancel
//
//   Instantiation example:
//     static const char    keys[]    = { '<','^','>','A','V','B' };
//     static const uint8_t rowPins[] = { 7, 17 };
//     static const uint8_t colPins[] = { 16, 15, 18 };
//     LightAir_HWKeypad keypad(keys, rowPins, 2, colPins, 3);
// ----------------------------------------------------------------
class LightAir_HWKeypad : public LightAir_Keypad {
public:
    // keys      : flat char array, row-major (row0col0, row0col1 … rowRcolC)
    // rowPins   : GPIO pin numbers for each row (length = rows)
    // colPins   : GPIO pin numbers for each column (length = cols)
    // rows,cols : keypad dimensions; rows*cols must be ≤ InputDefaults::MAX_KEYPAD_KEYS
    // debounceMs: minimum stable time before a transition is reported
    LightAir_HWKeypad(const char*    keys,
                      const uint8_t* rowPins, uint8_t rows,
                      const uint8_t* colPins, uint8_t cols,
                      uint32_t debounceMs = InputDefaults::DEBOUNCE_MS);

    // Configure GPIO directions. Call once in setup().
    void begin();

    // Scan matrix, apply debounce, return edge events.
    uint8_t getEvents(KeypadRawEvent* buf, uint8_t maxN) override;

private:
    const char*    _keys;
    const uint8_t* _rowPins;
    const uint8_t* _colPins;
    uint8_t        _rows;
    uint8_t        _cols;
    uint8_t        _keyCount;   // rows*cols, clamped to MAX_KEYPAD_KEYS
    uint32_t       _debounceMs;

    struct KeyRaw {
        bool     debounced;   // last accepted (debounced) state
        bool     raw;         // last raw reading
        uint32_t changedAt;   // millis() when raw last differed from debounced
    };
    KeyRaw _state[InputDefaults::MAX_KEYPAD_KEYS];
};
