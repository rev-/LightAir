#include "LightAir_HWKeypad.h"

LightAir_HWKeypad::LightAir_HWKeypad(const char*    keys,
                                       const uint8_t* rowPins, uint8_t rows,
                                       const uint8_t* colPins, uint8_t cols,
                                       uint32_t debounceMs)
    : _keys(keys), _rowPins(rowPins), _colPins(colPins),
      _rows(rows), _cols(cols), _debounceMs(debounceMs)
{
    _keyCount = rows * cols;
    if (_keyCount > InputDefaults::MAX_KEYPAD_KEYS)
        _keyCount = InputDefaults::MAX_KEYPAD_KEYS;
    for (uint8_t i = 0; i < _keyCount; i++)
        _state[i] = { false, false, 0 };
}

void LightAir_HWKeypad::begin() {
    for (uint8_t r = 0; r < _rows; r++) {
        pinMode(_rowPins[r], OUTPUT);
        digitalWrite(_rowPins[r], HIGH);   // idle: all rows high (not selected)
    }
    for (uint8_t c = 0; c < _cols; c++)
        pinMode(_colPins[c], INPUT_PULLUP);
}

uint8_t LightAir_HWKeypad::getEvents(KeypadRawEvent* buf, uint8_t maxN) {
    uint32_t now     = millis();
    uint8_t  evCount = 0;

    // --- 1. Scan matrix: update raw state for every key ---
    for (uint8_t r = 0; r < _rows; r++) {
        digitalWrite(_rowPins[r], LOW);
        delayMicroseconds(10);              // allow column lines to settle (RC ≈ 2 µs, 5× margin)

        for (uint8_t c = 0; c < _cols; c++) {
            uint8_t idx = r * _cols + c;
            if (idx >= _keyCount) break;

            bool rawNow = (digitalRead(_colPins[c]) == LOW);   // active-low
            if (rawNow != _state[idx].raw) {
                _state[idx].raw       = rawNow;
                _state[idx].changedAt = now;
            }
        }

        digitalWrite(_rowPins[r], HIGH);   // de-select row
    }

    // --- 2. Debounce: promote stable transitions to events ---
    for (uint8_t i = 0; i < _keyCount && evCount < maxN; i++) {
        KeyRaw& k = _state[i];
        if (k.raw != k.debounced && (now - k.changedAt) >= _debounceMs) {
            k.debounced       = k.raw;
            buf[evCount++]    = { _keys[i], k.debounced };
        }
    }

    return evCount;
}
