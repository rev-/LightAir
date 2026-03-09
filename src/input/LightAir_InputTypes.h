#pragma once
#include <stdint.h>
#include "../config.h"

// ---------------------------------------------------------------
// Button state machine:
//
//   OFF ──press──> PRESSED ──longPress──> HELD ──release──> RELEASED_HELD ──> OFF
//                     └──release──> RELEASED ──────────────────────────────> OFF
//
// RELEASED and RELEASED_HELD are guaranteed to appear in exactly
// one poll() call (they are set and returned in the same call).
// ---------------------------------------------------------------
enum class ButtonState : uint8_t {
    OFF,            // not pressed
    PRESSED,        // pressed, below long-press threshold
    HELD,           // pressed, past long-press threshold
    RELEASED,       // just released after PRESSED (one poll() cycle)
    RELEASED_HELD   // just released after HELD    (one poll() cycle)
};

// KeyState follows the same state machine as ButtonState.
enum class KeyState : uint8_t {
    OFF,
    PRESSED,
    HELD,
    RELEASED,
    RELEASED_HELD
};

// ---------------------------------------------------------------
// InputReport — full snapshot returned by InputCtrl::poll().
//
// buttons[]   : one entry per registered button, always present.
//               Check state; OFF means not pressed.
// keyEvents[] : only keys with non-OFF state this cycle.
//               Keys not listed are OFF.
// ---------------------------------------------------------------
struct InputReport {
    struct ButtonEntry {
        uint8_t     id;
        ButtonState state;
    };
    struct KeyEntry {
        uint8_t  keypadId;
        char     key;
        KeyState state;
    };

    ButtonEntry buttons[InputDefaults::MAX_BUTTONS];
    uint8_t     buttonCount   = 0;

    KeyEntry    keyEvents[InputDefaults::MAX_KEYPAD_EVENTS];
    uint8_t     keyEventCount = 0;
};
