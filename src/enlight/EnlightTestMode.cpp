#include "EnlightTestMode.h"
#include "../config.h"
#include <Arduino.h>

static const char* TAG = "TestMode";

// Key state tracking for test mode input
static KeyState gPrevKeyState[256] = {};
static uint32_t gLastHeldReturn[256] = {};

EnlightTestMode::EnlightTestMode(Enlight&            e,
                                 LightAir_Display&   disp,
                                 LightAir_InputCtrl& input,
                                 LightAir_UICtrl&    uiCtrl,
                                 uint8_t             keypadId)
    : _e(e), _disp(disp), _input(input), _uiCtrl(uiCtrl), _keypadId(keypadId) {
}

void EnlightTestMode::run() {
    _repetitions = 5;
    _e.setRepetitions(_repetitions);
    _lastHeldTime = 0;
    memset(gPrevKeyState, 0, sizeof(gPrevKeyState));
    memset(gLastHeldReturn, 0, sizeof(gLastHeldReturn));

    bool showResult = false;
    char resultText[32] = "";

    while (true) {
        renderDisplay(showResult, resultText);
        showResult = false;

        // Non-blocking: poll input and Enlight result
        uint32_t now = millis();
        const InputReport& report = _input.poll();

        // Process keypad input events
        for (uint8_t i = 0; i < report.keyEventCount; i++) {
            const InputReport::KeyEntry& keyEvent = report.keyEvents[i];
            if (keyEvent.keypadId != _keypadId) continue;

            const KeyState state = keyEvent.state;
            const char key = keyEvent.key;

            // Update previous state for edge detection
            KeyState prevState = gPrevKeyState[(uint8_t)key];
            gPrevKeyState[(uint8_t)key] = state;

            // A (O) = return to menu, B (X) = return to menu
            if ((key == 'A' || key == 'B') && state == KeyState::PRESSED) {
                return;
            }

            // < and > to adjust repetitions
            if (key == '<' || key == '>') {
                int8_t delta = (key == '<') ? -1 : 1;

                // On PRESSED, apply once
                if (state == KeyState::PRESSED && prevState != KeyState::PRESSED) {
                    int32_t newVal = (int32_t)_repetitions + delta;
                    if (newVal < 1) newVal = 100;
                    if (newVal > 100) newVal = 1;
                    _repetitions = (uint32_t)newVal;
                    _e.setRepetitions(_repetitions);
                    gLastHeldReturn[(uint8_t)key] = now;
                }
                // On HELD, repeat every 100ms
                else if (state == KeyState::HELD && (now - gLastHeldReturn[(uint8_t)key] >= 100)) {
                    int32_t newVal = (int32_t)_repetitions + delta;
                    if (newVal < 1) newVal = 100;
                    if (newVal > 100) newVal = 1;
                    _repetitions = (uint32_t)newVal;
                    _e.setRepetitions(_repetitions);
                    gLastHeldReturn[(uint8_t)key] = now;
                }
            }
        }

        // Process TRIG_1 button
        for (uint8_t i = 0; i < report.buttonCount; i++) {
            if (report.buttons[i].id == InputDefaults::TRIG_1_ID &&
                report.buttons[i].state == ButtonState::PRESSED) {
                if (runEnlight()) {
                    // A hit was detected; display result
                    showResult = true;
                }
            }
        }

        // Poll Enlight for continuous measurement
        if (_e.isActive()) {
            EnlightResult res = _e.poll();
            if (res.status == EnlightStatus::PLAYER_HIT) {
                // Got a color hit
                const char* colorName = getColorShortName(res.id);
                snprintf(resultText, sizeof(resultText), "LIT %s", colorName);
                showResult = true;
                _uiCtrl.trigger(LightAir_UICtrl::UIEvent::Lit);
            } else if (res.status == EnlightStatus::LOW_POW) {
                snprintf(resultText, sizeof(resultText), "NO TARGET");
                showResult = true;
            } else if (res.status == EnlightStatus::NO_HIT) {
                snprintf(resultText, sizeof(resultText), "NO COLOR");
                showResult = true;
            }
        }

        delay(10);  // Small delay to avoid spinning CPU
    }
}

bool EnlightTestMode::runEnlight() {
    _e.setRepetitions(_repetitions);
    if (!_e.run()) {
        return false;  // Already running
    }

    // Block until measurement completes
    uint32_t timeout = millis() + 5000;
    while (_e.isActive() && millis() < timeout) {
        delay(10);
    }

    return true;
}

void EnlightTestMode::renderDisplay(bool showResult, const char* resultText) {
    _disp.clear();
    _disp.setColor(true);
    _disp.print(0, 0, "-- Test Mode --");

    char repLine[20];
    snprintf(repLine, sizeof(repLine), "Reps: %u", (unsigned)_repetitions);
    _disp.print(0, DisplayDefaults::FONT_HEIGHT, repLine);

    if (showResult && resultText && resultText[0] != '\0') {
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 2, resultText);
    } else {
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 2, "Press TRIG_1");
    }

    _disp.print(0, DisplayDefaults::FONT_HEIGHT * 3, "<  >  adjust");

    char legend[30];
    snprintf(legend, sizeof(legend), "O:Menu  X:Menu");
    _disp.print(0, DisplayDefaults::BOTTOM_LINE_Y, legend);

    _disp.flush();
}

const char* EnlightTestMode::getColorShortName(uint8_t playerId) const {
    if (playerId < PlayerDefs::MAX_PLAYER_ID) {
        return PlayerDefs::playerNames[playerId];
    }
    return "???";
}
