#include "EnlightTestMode.h"
#include "../config.h"
#include <stdio.h>
#include <string.h>

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
        _prevKeyState[(uint8_t)ke.key] = (uint8_t)ke.state;

        if (ke.state == KeyState::RELEASED || ke.state == KeyState::RELEASED_HELD)
            _prevKeyState[(uint8_t)ke.key] = (uint8_t)KeyState::OFF;

        if (ke.state == KeyState::PRESSED && (KeyState)prev == KeyState::OFF) {
            _lastHeldReturn[(uint8_t)ke.key] = millis();
            return {ke.key, (uint8_t)KeyState::PRESSED};
        }
        if (ke.state == KeyState::HELD && (KeyState)prev == KeyState::PRESSED) {
            _lastHeldReturn[(uint8_t)ke.key] = millis();
            return {ke.key, (uint8_t)KeyState::HELD};
        }
        if (ke.key != 'A' && ke.key != 'B') {
            if (ke.state == KeyState::HELD && (KeyState)prev == KeyState::HELD) {
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
        if (be.state == ButtonState::PRESSED && (ButtonState)prev == ButtonState::OFF) {
            _lastButtonHeld[be.id] = millis();
            return {vk, (uint8_t)KeyState::PRESSED};
        }
        if (be.state == ButtonState::HELD && (ButtonState)prev == ButtonState::PRESSED) {
            _lastButtonHeld[be.id] = millis();
            return {vk, (uint8_t)KeyState::HELD};
        }
        if (be.state == ButtonState::HELD && (ButtonState)prev == ButtonState::HELD) {
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
    uint32_t legendW = 0, legendX = 0;
    const uint32_t MIN_REPS = 1, MAX_REPS = 100;

    char diagColor[12]         = "--";
    char diagRgb[24]           = "--";
    char diagColorCoord[24]    = "--";
    char diagSs[24]            = "--";

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

        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 2, diagColorCoord);
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 3, diagRgb);
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * 4, diagSs);

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

            EnlightRawMeasure  raw   = _e.rawMeasure();
            EnlightColorCoords color = _e.colorCoords();

            long long rawSum = raw.rout + raw.gout + raw.bout;
            long long sumShr = rawSum;
            int bitCount = 0;
            while (sumShr > 1024) { sumShr >>= 1; bitCount++; }

            snprintf(diagRgb, sizeof(diagRgb), "r:%03lld g:%03lld b:%03lld >>%d",
                     raw.rout >> bitCount, raw.gout >> bitCount, raw.bout >> bitCount, bitCount);
            snprintf(diagColorCoord, sizeof(diagColorCoord), "r:%.2f a:%.2f",
                     (double)color.outr, (double)color.outang);
            const uint32_t satPct = (raw.totalSamples > 0)
                ? (uint32_t)(raw.satCount * 100u / raw.totalSamples) : 0u;
            EnlightPhaseGate pg = _e.phaseGate();
            snprintf(diagSs, sizeof(diagSs), "sat:%lu%% p:%3.0f z:%4.1f",
                     (unsigned long)satPct, (double)pg.phiDeg, (double)pg.snrPhase);

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
