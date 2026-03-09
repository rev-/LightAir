#pragma once
#include <Arduino.h>
#include "LightAir_InputTypes.h"
#include "LightAir_Button.h"
#include "LightAir_Keypad.h"
#include "../config.h"

// Aggregates buttons and keypads into a single snapshot per loop iteration.
//
// Usage:
//   LightAir_HWButton trigger(TRIG_1_PIN);
//   trigger.begin();
//   inputCtrl.registerButton(0, trigger);
//
//   // in loop():
//   const InputReport& report = inputCtrl.poll();
//   for (uint8_t i = 0; i < report.buttonCount; i++) { ... }
//
// poll() scans all registered hardware, runs state machines, and
// returns a reference to an internal snapshot valid until the next poll().
class LightAir_InputCtrl {
public:

    // Register a button. id is user-assigned (used in InputReport).
    // longPressMs = 0 uses InputDefaults::LONG_PRESS_MS.
    // Returns false if MAX_BUTTONS is exceeded.
    bool registerButton(uint8_t id, LightAir_Button& btn,
                        uint32_t longPressMs = 0);

    // Register a keypad. id is user-assigned (used in InputReport).
    // longPressMs = 0 uses InputDefaults::LONG_PRESS_MS.
    // Returns false if MAX_KEYPADS is exceeded.
    bool registerKeypad(uint8_t id, LightAir_Keypad& kpd,
                        uint32_t longPressMs = 0);

    // Scan all registered inputs, run state machines, return snapshot.
    // Call exactly once per main loop iteration.
    // The returned reference is valid until the next call to poll().
    const InputReport& poll();

private:

    // ---- Button tracking ----
    struct ButtonTrack {
        uint8_t          id;
        LightAir_Button* btn;
        ButtonState      state;
        uint32_t         pressedAt;
        uint32_t         longPressMs;
    };
    ButtonTrack _buttons[InputDefaults::MAX_BUTTONS];
    uint8_t     _buttonCount = 0;

    // ---- Keypad tracking ----
    struct KeyTrack {
        char     key;
        KeyState state;
        uint32_t pressedAt;
        bool     active;
    };
    struct KeypadReg {
        uint8_t          id;
        LightAir_Keypad* kpd;
        uint32_t         longPressMs;
        KeyTrack         keys[InputDefaults::MAX_KEYPAD_KEYS];
    };
    KeypadReg _keypads[InputDefaults::MAX_KEYPADS];
    uint8_t   _keypadCount = 0;

    InputReport _report;

    void pollButton(ButtonTrack& bt, uint32_t now);
    void pollKeypad(KeypadReg& kr, uint32_t now);

    KeyTrack* findKey(KeypadReg& kr, char key);
    KeyTrack* allocKey(KeypadReg& kr, char key, uint32_t now);
};
