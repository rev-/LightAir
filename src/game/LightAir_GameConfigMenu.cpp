#include "LightAir_GameConfigMenu.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

/* =========================================================
 *   FREE FUNCTIONS — config blob serialization
 * ========================================================= */

uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen) {
    if (maxLen < 4) return 0;

    // Header: typeId
    uint32_t id = game.typeId;
    memcpy(buf, &id, 4);
    uint16_t pos = 4;

    // One int32 per configVar, in declaration order.
    for (uint8_t v = 0; v < game.configCount && pos + 4 <= maxLen; v++) {
        int32_t val = (int32_t)*game.configVars[v].value;
        memcpy(buf + pos, &val, 4);
        pos += 4;
    }
    return pos;
}

bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len) {
    if (len < 4) return false;

    uint32_t id;
    memcpy(&id, buf, 4);
    if (id != game.typeId) return false;

    uint16_t pos = 4;
    for (uint8_t v = 0; v < game.configCount && pos + 4 <= len; v++) {
        const ConfigVar& var = game.configVars[v];
        int32_t val;
        memcpy(&val, buf + pos, 4);
        pos += 4;
        // Clamp to declared range.
        if (val < var.min) val = var.min;
        if (val > var.max) val = var.max;
        *var.value = (int)val;
    }
    return true;
}

/* =========================================================
 *   LightAir_GameConfigMenu
 * ========================================================= */

LightAir_GameConfigMenu::LightAir_GameConfigMenu(const LightAir_Game& game,
                                                   LightAir_Display&   display,
                                                   LightAir_InputCtrl& input,
                                                   uint8_t             keypadId,
                                                   LightAir_Radio&     radio,
                                                   uint8_t             msgType)
    : _game(game), _display(display), _input(input),
      _keypadId(keypadId), _radio(radio), _msgType(msgType)
{
    _configCount = game.configCount < DisplayDefaults::MAX_BINDINGS
                 ? game.configCount
                 : DisplayDefaults::MAX_BINDINGS;
}

void LightAir_GameConfigMenu::renderMenu(uint8_t cfgVar) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    const ConfigVar& var = _game.configVars[cfgVar];
    char buf[24];

    _display.clear();
    _display.setColor(true);

    // Row 0: header
    snprintf(buf, sizeof(buf), "Config  %u / %u", cfgVar + 1, _configCount);
    _display.print(0, 0, buf);

    // Row 1: variable name
    _display.print(0, fh, var.name);

    // Row 2: "< value >"
    int step = var.step ? var.step : 1;
    snprintf(buf, sizeof(buf), "<  %d  >  (step %d)", *var.value, step);
    _display.print(0, fh * 2, buf);

    // Row 3: controls
    _display.print(0, fh * 3, "A:OK  ^/V:nav  <>:val");

    _display.flush();
}

bool LightAir_GameConfigMenu::promptShare() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,      "Share config?");
    _display.print(0, fh,     "A: YES   B: Skip");
    _display.flush();
    return waitForKey() == 'A';
}

void LightAir_GameConfigMenu::shareConfig() {
    uint8_t blob[4 + GameDefaults::MAX_GAMES * 4];  // generous upper bound
    uint16_t len = game_serialize_config(_game, blob, sizeof(blob));
    if (len > 0) _radio.broadcast(_msgType, blob, len);
}

char LightAir_GameConfigMenu::waitForKey() {
    while (true) {
        const InputReport& rep = _input.poll();
        for (uint8_t i = 0; i < rep.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.state != KeyState::RELEASED &&
                ke.state != KeyState::RELEASED_HELD) continue;
            return ke.key;
        }
        delay(GameDefaults::LOOP_MS);
    }
}

MenuResult LightAir_GameConfigMenu::run() {
    if (_configCount == 0) return MenuResult::Confirmed;

    uint8_t cur = 0;
    renderMenu(cur);

    while (true) {
        char key = waitForKey();
        const ConfigVar& var = _game.configVars[cur];
        int step = var.step ? var.step : 1;

        switch (key) {
            case '^':
                if (cur > 0) { cur--; renderMenu(cur); }
                break;
            case 'V':
                if (cur < _configCount - 1) { cur++; renderMenu(cur); }
                break;
            case '<': {
                int val = *var.value - step;
                if (val < var.min) val = var.min;
                *var.value = val;
                renderMenu(cur);
                break;
            }
            case '>': {
                int val = *var.value + step;
                if (val > var.max) val = var.max;
                *var.value = val;
                renderMenu(cur);
                break;
            }
            case 'A':
                if (promptShare()) shareConfig();
                return MenuResult::Confirmed;
            case 'B':
                return MenuResult::Cancelled;
        }
    }
}
