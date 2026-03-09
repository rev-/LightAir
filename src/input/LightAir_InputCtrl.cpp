#include "LightAir_InputCtrl.h"

/* =========================================================
 *   REGISTRATION
 * ========================================================= */

bool LightAir_InputCtrl::registerButton(uint8_t id, LightAir_Button& btn,
                                         uint32_t longPressMs) {
    if (_buttonCount >= InputDefaults::MAX_BUTTONS) return false;
    ButtonTrack& bt = _buttons[_buttonCount++];
    bt.id          = id;
    bt.btn         = &btn;
    bt.state       = ButtonState::OFF;
    bt.pressedAt   = 0;
    bt.longPressMs = (longPressMs > 0) ? longPressMs : InputDefaults::LONG_PRESS_MS;
    return true;
}

bool LightAir_InputCtrl::registerKeypad(uint8_t id, LightAir_Keypad& kpd,
                                         uint32_t longPressMs) {
    if (_keypadCount >= InputDefaults::MAX_KEYPADS) return false;
    KeypadReg& kr  = _keypads[_keypadCount++];
    kr.id          = id;
    kr.kpd         = &kpd;
    kr.longPressMs = (longPressMs > 0) ? longPressMs : InputDefaults::LONG_PRESS_MS;
    for (uint8_t i = 0; i < InputDefaults::MAX_KEYPAD_KEYS; i++)
        kr.keys[i].active = false;
    return true;
}

/* =========================================================
 *   POLL
 * ========================================================= */

const InputReport& LightAir_InputCtrl::poll() {
    uint32_t now          = millis();
    _report.buttonCount   = 0;
    _report.keyEventCount = 0;

    for (uint8_t i = 0; i < _buttonCount; i++)
        pollButton(_buttons[i], now);

    for (uint8_t i = 0; i < _keypadCount; i++)
        pollKeypad(_keypads[i], now);

    return _report;
}

/* =========================================================
 *   BUTTON STATE MACHINE
 * ========================================================= */

void LightAir_InputCtrl::pollButton(ButtonTrack& bt, uint32_t now) {
    bool pressed = bt.btn->isPressed();

    switch (bt.state) {
        case ButtonState::OFF:
            if (pressed) { bt.state = ButtonState::PRESSED; bt.pressedAt = now; }
            break;

        case ButtonState::PRESSED:
            if (!pressed)
                bt.state = ButtonState::RELEASED;
            else if (now - bt.pressedAt >= bt.longPressMs)
                bt.state = ButtonState::HELD;
            break;

        case ButtonState::HELD:
            if (!pressed) bt.state = ButtonState::RELEASED_HELD;
            break;

        case ButtonState::RELEASED:
        case ButtonState::RELEASED_HELD:
            // These states were returned to the caller in this same poll() call.
            // Transition immediately so the next poll() sees the correct state.
            if (pressed) { bt.state = ButtonState::PRESSED; bt.pressedAt = now; }
            else           bt.state = ButtonState::OFF;
            break;
    }

    if (_report.buttonCount < InputDefaults::MAX_BUTTONS)
        _report.buttons[_report.buttonCount++] = { bt.id, bt.state };
}

/* =========================================================
 *   KEYPAD STATE MACHINE
 * ========================================================= */

void LightAir_InputCtrl::pollKeypad(KeypadReg& kr, uint32_t now) {
    KeypadRawEvent rawBuf[InputDefaults::MAX_KEYPAD_KEYS];
    uint8_t rawCount = kr.kpd->getEvents(rawBuf, InputDefaults::MAX_KEYPAD_KEYS);

    // Pass 1: process released events first so their slots can be
    // freed before pressed events potentially reuse them.
    for (uint8_t i = 0; i < rawCount; i++) {
        if (rawBuf[i].pressed) continue;
        KeyTrack* kt = findKey(kr, rawBuf[i].key);
        if (!kt) continue;
        kt->state = (kt->state == KeyState::HELD)
                  ? KeyState::RELEASED_HELD
                  : KeyState::RELEASED;
    }

    // Time-based PRESSED → HELD transition.
    for (uint8_t i = 0; i < InputDefaults::MAX_KEYPAD_KEYS; i++) {
        KeyTrack& kt = kr.keys[i];
        if (!kt.active) continue;
        if (kt.state == KeyState::PRESSED && (now - kt.pressedAt) >= kr.longPressMs)
            kt.state = KeyState::HELD;
    }

    // Emit all active keys to report; free RELEASED / RELEASED_HELD slots.
    for (uint8_t i = 0; i < InputDefaults::MAX_KEYPAD_KEYS; i++) {
        KeyTrack& kt = kr.keys[i];
        if (!kt.active) continue;

        if (_report.keyEventCount < InputDefaults::MAX_KEYPAD_EVENTS)
            _report.keyEvents[_report.keyEventCount++] = { kr.id, kt.key, kt.state };

        if (kt.state == KeyState::RELEASED || kt.state == KeyState::RELEASED_HELD) {
            kt.active = false;
            kt.state  = KeyState::OFF;
        }
    }

    // Pass 2: process pressed events — allocate slots after slots may
    // have been freed above, so a rapid release+press of the same key
    // yields RELEASED then PRESSED on consecutive poll() calls.
    for (uint8_t i = 0; i < rawCount; i++) {
        if (!rawBuf[i].pressed) continue;
        KeyTrack* kt = findKey(kr, rawBuf[i].key);
        if (!kt) kt = allocKey(kr, rawBuf[i].key, now);
        if (kt) { kt->state = KeyState::PRESSED; kt->pressedAt = now; }
    }
}

/* =========================================================
 *   HELPERS
 * ========================================================= */

LightAir_InputCtrl::KeyTrack*
LightAir_InputCtrl::findKey(KeypadReg& kr, char key) {
    for (uint8_t i = 0; i < InputDefaults::MAX_KEYPAD_KEYS; i++)
        if (kr.keys[i].active && kr.keys[i].key == key)
            return &kr.keys[i];
    return nullptr;
}

LightAir_InputCtrl::KeyTrack*
LightAir_InputCtrl::allocKey(KeypadReg& kr, char key, uint32_t now) {
    for (uint8_t i = 0; i < InputDefaults::MAX_KEYPAD_KEYS; i++) {
        if (!kr.keys[i].active) {
            kr.keys[i] = { key, KeyState::PRESSED, now, true };
            return &kr.keys[i];
        }
    }
    return nullptr;   // no free slot (all MAX_KEYPAD_KEYS simultaneously held)
}
