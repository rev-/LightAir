#include "LightAir_ToolsMenu.h"
#include <Arduino.h>
#include <stdio.h>

bool LightAir_ToolsMenu::addTool(LightAir_HoldTool& tool) {
    if (_count >= MAX_TOOLS) return false;
    _tools[_count++] = &tool;
    return true;
}

bool LightAir_ToolsMenu::keyDown(const InputReport& rep, char key) const {
    for (uint8_t i = 0; i < rep.keyEventCount; i++) {
        const InputReport::KeyEntry& ke = rep.keyEvents[i];
        if (ke.keypadId != _keypadId || ke.key != key) continue;
        return ke.state == KeyState::PRESSED || ke.state == KeyState::HELD;
    }
    return false;
}

void LightAir_ToolsMenu::draw(uint8_t sel) {
    _disp.clear();
    _disp.setColor(true);
    _disp.print(0, 0, "-- Tools --");
    for (uint8_t i = 0; i < _count; i++) {
        char row[24];
        snprintf(row, sizeof(row), "%c %s", (i == sel) ? '>' : ' ', _tools[i]->holdName());
        _disp.print(0, DisplayDefaults::FONT_HEIGHT * (1 + i), row);
    }
    const char* legend = "O:Select  X:Back";
    const uint16_t w = _disp.textWidth(legend);
    _disp.print(w < DisplayDefaults::SCREEN_WIDTH
                    ? (uint8_t)((DisplayDefaults::SCREEN_WIDTH - w) / 2) : 0,
                DisplayDefaults::BOTTOM_LINE_Y, legend);
    _disp.flush();
}

void LightAir_ToolsMenu::runHeld(LightAir_HoldHost& host) {
    // The A+B that opened the menu is still down: let both go first.
    for (;;) {
        const InputReport& rep = _input.poll();
        host.service();
        if (!keyDown(rep, 'A') && !keyDown(rep, 'B')) break;
        delay(10);
    }

    uint8_t sel = 0;
    draw(sel);
    for (;;) {
        const InputReport& rep = _input.poll();
        host.service();

        for (uint8_t i = 0; i < rep.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep.keyEvents[i];
            if (ke.keypadId != _keypadId || ke.state != KeyState::PRESSED) continue;
            if (ke.key == 'B') return;
            if (ke.key == '^' && sel > 0)          { sel--; draw(sel); }
            if (ke.key == 'V' && sel + 1 < _count) { sel++; draw(sel); }
            if (ke.key == 'A' && _count > 0) {
                _tools[sel]->runHeld(host);
                return;                              // straight back to the game
            }
        }
        delay(10);
    }
}
