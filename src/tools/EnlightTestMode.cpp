#include "EnlightTestMode.h"
#include "../config.h"
#include <stdio.h>

static inline char buttonVirtualKey(uint8_t id) { return (char)(0x01 + id); }

EnlightTestMode::EnlightTestMode(Enlight&            e,
                                  LightAir_UICtrl&    ui,
                                  LightAir_Display&   disp,
                                  LightAir_InputCtrl& input,
                                  uint8_t             keypadId)
    : _e(e), _ui(ui), _disp(disp), _input(input), _keypadId(keypadId) {}

EnlightTestMode::KeyEvent EnlightTestMode::pollKey() {
    const InputReport& rep = _input.poll();

    // Keypad keys — rise-edge and held-repeat detection
    for (uint8_t i = 0; i < rep.keyEventCount; i++) {
        const InputReport::KeyEntry& ke = rep.keyEvents[i];
        if (ke.keypadId != _keypadId) continue;

        uint8_t prev = _prevKeyState[(uint8_t)ke.key];
        _prevKeyState[(uint8_t)ke.key] = ke.state;

        if (ke.state == KeyState::RELEASED || ke.state == KeyState::RELEASED_HELD)
            _prevKeyState[(uint8_t)ke.key] = KeyState::OFF;

        if (ke.state == KeyState::PRESSED && prev == KeyState::OFF) {
            _lastHeldReturn[(uint8_t)ke.key] = millis();
            return {ke.key, (uint8_t)KeyState::PRESSED};
        }
        if (ke.state == KeyState::HELD && prev == KeyState::PRESSED) {
            _lastHeldReturn[(uint8_t)ke.key] = millis();
            return {ke.key, (uint8_t)KeyState::HELD};
        }
        if (ke.key != 'A' && ke.key != 'B') {
            if (ke.state == KeyState::HELD && prev == KeyState::HELD) {
                uint32_t now = millis();
                if (now - _lastHeldReturn[(uint8_t)ke.key] >= InputDefaults::HELD_REPEAT_MS) {
                    _lastHeldReturn[(uint8_t)ke.key] = now;
                    return {ke.key, (uint8_t)KeyState::HELD};
                }
            }
        }
    }

    // Buttons (triggers)
    for (uint8_t i = 0; i < rep.buttonCount; i++) {
        const InputReport::ButtonEntry& be = rep.buttons[i];
        if (be.id >= InputDefaults::MAX_BUTTONS) continue;

        uint8_t prev = _prevButtonState[be.id];
        _prevButtonState[be.id] = (uint8_t)be.state;

        if (be.state == ButtonState::RELEASED || be.state == ButtonState::RELEASED_HELD)
            _prevButtonState[be.id] = (uint8_t)ButtonState::OFF;

        const char vk = buttonVirtualKey(be.id);
        if (be.state == ButtonState::PRESSED && prev == (uint8_t)ButtonState::OFF) {
            _lastButtonHeld[be.id] = millis();
            return {vk, (uint8_t)KeyState::PRESSED};
        }
        if (be.state == ButtonState::HELD && prev == (uint8_t)ButtonState::PRESSED) {
            _lastButtonHeld[be.id] = millis();
            return {vk, (uint8_t)KeyState::HELD};
        }
        if (be.state == ButtonState::HELD && prev == (uint8_t)ButtonState::HELD) {
            uint32_t now = millis();
            if (now - _lastButtonHeld[be.id] >= InputDefaults::HELD_REPEAT_MS) {
                _lastButtonHeld[be.id] = now;
                return {vk, (uint8_t)KeyState::HELD};
            }
        }
    }

    return {'\0', 0};
}

void EnlightTestMode::run() {
    uint32_t reps = 5;
    const uint32_t MIN_REPS = 1, MAX_REPS = 100;

    char diagColor[12] = "--";
    char diagRa[12]    = "--";
    char diagSs[12]    = "--";

    // Initialize state tracking
    memset(_prevKeyState, (uint8_t)KeyState::OFF, sizeof(_prevKeyState));
    memset(_prevButtonState, (uint8_t)ButtonState::OFF, sizeof(_prevButtonState));
    _input.poll();

    while (true) {
        // ---- Render ----
        _disp.clear();
        _disp.setColor(true);

        char line0[22];
        snprintf(line0, sizeof(line0), "Test  reps:%lu", (unsigned long)reps);
        _disp.print(0, 0, line0);

        char line1[22];
        snprintf(line1, sizeof(line1), "Hit: %s", diagColor);
        _disp.print(0, DisplayDefaults::FONT_HEIGHT, line1);

        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 2, diagRa);
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 3, diagSs);

        uint16_t legendW = _disp.textWidth("<>:Reps  T1:Fire");
        uint8_t  legendX = (legendW < DisplayDefaults::SCREEN_WIDTH)
                         ? (uint8_t)((DisplayDefaults::SCREEN_WIDTH - legendW) / 2)
                         : 0;
        _disp.print(legendX, DisplayDefaults::BOTTOM_LINE_Y - DisplayDefaults::FONT_HEIGHT,
                    "<>:Reps  T1:Fire");

        legendW = _disp.textWidth("X:Back");
        legendX = (legendW < DisplayDefaults::SCREEN_WIDTH)
                ? (uint8_t)((DisplayDefaults::SCREEN_WIDTH - legendW) / 2)
                : 0;
        _disp.print(legendX, DisplayDefaults::BOTTOM_LINE_Y, "X:Back");

        _disp.flush();

        // ---- Input ----
        KeyEvent ev;
        do {
            ev = pollKey();
            delay(GameDefaults::LOOP_MS);
        } while (ev.key == '\0');

        const char key = ev.key;
        const uint8_t state = ev.state;

        if (key == 'B' && state == (uint8_t)KeyState::PRESSED) return;

        if (key == '<' && (state == (uint8_t)KeyState::PRESSED || state == (uint8_t)KeyState::HELD))
            reps = (reps > MIN_REPS) ? (reps - 1) : MIN_REPS;
        if (key == '>' && (state == (uint8_t)KeyState::PRESSED || state == (uint8_t)KeyState::HELD))
            reps = (reps < MAX_REPS) ? (reps + 1) : MAX_REPS;

        if (key == buttonVirtualKey(InputDefaults::TRIG_1_ID) && state == (uint8_t)KeyState::PRESSED) {
            _e.setRepetitions(reps);
            _e.run();

            EnlightResult res;
            do {
                res = _e.poll();
                delay(GameDefaults::LOOP_MS);
            } while (res.status == EnlightStatus::RUNNING);

            EnlightRawMeasure raw = _e.rawMeasure();
            const EnlightCalib& cal = _e.calib();
            float rw = (float)raw.rout * cal.rfact;
            float gw = (float)raw.gout;
            float bw = (float)raw.bout * cal.bfact;
            float s  = rw + gw + bw;
            float outr_n = (s > 0.f) ? (rw / s) : 0.f;
            float outang  = (s > 0.f && outr_n < 1.f) ? (gw / s) / (1.f - outr_n) : 0.f;

            long long rawSum = raw.rout + raw.gout + raw.bout;
            long long sumShr = rawSum >> 20;

            snprintf(diagRa, sizeof(diagRa), "r:%.2f a:%.2f",
                     (double)outr_n, (double)outang);
            snprintf(diagSs, sizeof(diagSs), "S:%lld sat:%lu",
                     (long long)sumShr, (unsigned long)raw.satCount);

            if (res.status == EnlightStatus::PLAYER_HIT &&
                res.id < PlayerDefs::MAX_PLAYER_ID) {
                snprintf(diagColor, sizeof(diagColor), "%s",
                         PlayerDefs::playerShort[res.id]);
                _ui.trigger(LightAir_UICtrl::UIEvent::Lit);
            } else {
                snprintf(diagColor, sizeof(diagColor), "none(%d)", (int)res.status);
            }
        }
    }
}
