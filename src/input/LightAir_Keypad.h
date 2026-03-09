#pragma once
#include <stdint.h>

// Raw edge event from a keypad hardware scan.
// Reported only when a key transitions (debounced):
//   pressed = true  → key just became pressed
//   pressed = false → key just became released
struct KeypadRawEvent {
    char key;
    bool pressed;
};

// Abstract interface for a keypad input.
// Concrete implementations scan the hardware, apply debounce, and
// report only edge transitions. State machine logic (PRESSED/HELD/
// RELEASED/RELEASED_HELD) is handled by LightAir_InputCtrl.
class LightAir_Keypad {
public:
    // Scan hardware and return debounced edge events since last call.
    // Writes up to maxN events into buf; returns the number written.
    virtual uint8_t getEvents(KeypadRawEvent* buf, uint8_t maxN) = 0;

    virtual ~LightAir_Keypad() = default;
};
