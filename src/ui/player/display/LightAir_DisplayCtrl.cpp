#include "LightAir_DisplayCtrl.h"

/* =========================================================
 *   CONSTRUCTOR
 * ========================================================= */

LightAir_DisplayCtrl::LightAir_DisplayCtrl(LightAir_Display& display)
: _display(display),
  _setCount(0),
  _selectedSet(0),
  _activeSet(0)
{
    for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES; i++) {
        _tray[i].active = false;
        _tray[i].dirty  = false;
    }
}

void LightAir_DisplayCtrl::begin() {
    _display.clear();
    _display.flush();
}

/* =========================================================
 *   SET MANAGEMENT
 * ========================================================= */

uint8_t LightAir_DisplayCtrl::createBindingSet() {
    if (_setCount >= DisplayDefaults::MAX_SETS) return 255;
    _sets[_setCount].count = 0;
    _sets[_setCount].locked = false;
    return _setCount++;
}

void LightAir_DisplayCtrl::selectBindingSet(uint8_t setId) {
    if (setId < _setCount)
        _selectedSet = setId;
}

void LightAir_DisplayCtrl::activateBindingSet(uint8_t setId) {
    if (setId < _setCount) {
        _activeSet = setId;
        _sets[setId].locked = true;
        _display.clear();
    }
}

/* =========================================================
 *   BINDING
 * ========================================================= */

bool LightAir_DisplayCtrl::bindIntVariable(int* variable, IconType icon, uint8_t x, uint8_t y) {
    if (y >= DisplayDefaults::CONTENT_HEIGHT) return false;

    BindingSet& set = _sets[_selectedSet];
    if (set.locked || set.count >= DisplayDefaults::MAX_BINDINGS) return false;

    VariableBinding& b = set.bindings[set.count++];
    b.variable  = variable;
    b.icon      = icon;
    b.type      = TYPE_INT;
    b.x         = x;
    b.y         = y;
    b.lastValue = INT32_MIN;
    return true;
}

bool LightAir_DisplayCtrl::bindCooldownVariable(
    int* variable,
    IconType icon,
    uint8_t x,
    uint8_t y,
    uint32_t cooldownTimeMs,
    uint8_t barWidth
) {
    if (y >= DisplayDefaults::CONTENT_HEIGHT) return false;

    BindingSet& set = _sets[_selectedSet];
    if (set.locked || set.count >= DisplayDefaults::MAX_BINDINGS) return false;

    VariableBinding& b = set.bindings[set.count++];
    b.variable       = variable;
    b.icon           = icon;
    b.type           = TYPE_COOLDOWN;
    b.x              = x;
    b.y              = y;
    b.cooldownTime   = cooldownTimeMs;
    b.cooldownActive = false;
    b.barWidth       = barWidth;
    b.lastValue      = INT32_MIN;
    return true;
}

bool LightAir_DisplayCtrl::bindStringVariable(const char* str, IconType icon, uint8_t x, uint8_t y) {
    if (y >= DisplayDefaults::CONTENT_HEIGHT) return false;

    BindingSet& set = _sets[_selectedSet];
    if (set.locked || set.count >= DisplayDefaults::MAX_BINDINGS) return false;

    VariableBinding& b = set.bindings[set.count++];
    b.strVariable = str;
    b.icon        = icon;
    b.type        = TYPE_STRING;
    b.x           = x;
    b.y           = y;
    b.lastText[0] = '\0';
    return true;
}

/* =========================================================
 *   TRAY MESSAGES
 * ========================================================= */

void LightAir_DisplayCtrl::showMessage(const char* text, uint32_t durationMs) {
    // push existing messages down one slot
    for (uint8_t i = DisplayDefaults::TRAY_MAX_MESSAGES - 1; i > 0; i--) {
        _tray[i]       = _tray[i - 1];
        _tray[i].dirty = true;
    }

    strncpy(_tray[0].text, text, sizeof(_tray[0].text) - 1);
    _tray[0].text[sizeof(_tray[0].text) - 1] = '\0';
    _tray[0].expireAt = (durationMs > 0) ? (millis() + durationMs) : 0;
    _tray[0].active   = true;
    _tray[0].dirty    = true;
}

void LightAir_DisplayCtrl::clearTray() {
    for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES; i++) {
        if (_tray[i].active) {
            _tray[i].active = false;
            _tray[i].dirty  = true;
        }
    }
}

/* =========================================================
 *   UPDATE
 * ========================================================= */

void LightAir_DisplayCtrl::update() {
    BindingSet& set = _sets[_activeSet];

    for (uint8_t i = 0; i < set.count; i++) {
        renderBinding(set.bindings[i]);
    }

    renderTray();
    _display.flush();
}

/* =========================================================
 *   RENDERING
 * ========================================================= */

void LightAir_DisplayCtrl::renderBinding(VariableBinding& b) {
    if (b.type == TYPE_INT)
        renderInt(b);
    else if (b.type == TYPE_COOLDOWN)
        renderCooldown(b);
    else
        renderString(b);
}

void LightAir_DisplayCtrl::renderInt(VariableBinding& b) {
    int value = *b.variable;
    if (value == b.lastValue) return;
    b.lastValue = value;

    _display.setColor(false);
    _display.fillRect(b.x, b.y, DisplayDefaults::CELL_WIDTH, DisplayDefaults::CELL_HEIGHT);
    _display.setColor(true);

    drawIcon(b.icon, b.x, b.y + DisplayDefaults::FONT_TOP_PADDING);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", value);
    _display.print(b.x + 10, b.y, buf);
}

void LightAir_DisplayCtrl::renderCooldown(VariableBinding& b) {
    int value = *b.variable;

    if (value > 0) {
        b.cooldownActive = false;
        renderInt(b);
        return;
    }

    if (!b.cooldownActive) {
        b.cooldownStart = millis();
        b.cooldownActive = true;
    }

    uint32_t elapsed = millis() - b.cooldownStart;
    if (elapsed >= b.cooldownTime) {
        b.cooldownActive = false;
        return;
    }

    float ratio = (float)elapsed / b.cooldownTime;

    _display.setColor(false);
    _display.fillRect(b.x, b.y, DisplayDefaults::CELL_WIDTH, DisplayDefaults::CELL_HEIGHT);
    _display.setColor(true);

    drawIcon(ICON_HOURGLASS, b.x, b.y + DisplayDefaults::FONT_TOP_PADDING);
    drawBar(b.x + 10, b.y + 2, b.barWidth, 6, ratio);
}

void LightAir_DisplayCtrl::renderString(VariableBinding& b) {
    const char* str = b.strVariable ? b.strVariable : "";
    if (strncmp(str, b.lastText, sizeof(b.lastText)) == 0) return;

    strncpy(b.lastText, str, sizeof(b.lastText) - 1);
    b.lastText[sizeof(b.lastText) - 1] = '\0';

    _display.setColor(false);
    _display.fillRect(b.x, b.y, DisplayDefaults::CELL_WIDTH, DisplayDefaults::CELL_HEIGHT);
    _display.setColor(true);

    drawIcon(b.icon, b.x, b.y + DisplayDefaults::FONT_TOP_PADDING);
    _display.print(b.x + 10, b.y, b.lastText);
}

/* =========================================================
 *   TRAY RENDER
 * ========================================================= */

void LightAir_DisplayCtrl::renderTray() {
    uint32_t now = millis();

    // expire timed-out messages
    for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES; i++) {
        if (_tray[i].active && _tray[i].expireAt > 0 && now >= _tray[i].expireAt) {
            _tray[i].active = false;
            _tray[i].dirty  = true;
        }
    }

    // compact: promote lower slots if upper ones are empty
    for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES - 1; i++) {
        if (!_tray[i].active && _tray[i + 1].active) {
            _tray[i]          = _tray[i + 1];
            _tray[i].dirty    = true;
            _tray[i+1].active = false;
            _tray[i+1].dirty  = true;
        }
    }

    // incremental redraw: only dirty slots
    for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES; i++) {
        if (!_tray[i].dirty) continue;
        uint8_t y = DisplayDefaults::CONTENT_HEIGHT + i * DisplayDefaults::FONT_HEIGHT;
        _display.setColor(false);
        _display.fillRect(0, y, DisplayDefaults::SCREEN_WIDTH, DisplayDefaults::FONT_HEIGHT);
        _display.setColor(true);
        if (_tray[i].active)
            _display.print(0, y, _tray[i].text);
        _tray[i].dirty = false;
    }
}

/* ========================================================= */

void LightAir_DisplayCtrl::drawIcon(IconType icon, uint8_t x, uint8_t y) {
    _display.drawBitmap(x, y, 8, 8, getIconBitmap(icon));
}

const uint8_t* LightAir_DisplayCtrl::getIconBitmap(IconType icon) {
    switch (icon) {
        case ICON_LIGHT:     return ICON_LIGHT_BITMAP;
        case ICON_LIFE:      return ICON_LIFE_BITMAP;
        case ICON_FLAG:      return ICON_FLAG_BITMAP;
        case ICON_HOURGLASS: return ICON_HOURGLASS_BITMAP;
        case ICON_SCORE:     return ICON_SCORE_BITMAP;
        case ICON_ROLE:      return ICON_ROLE_BITMAP;
        case ICON_ENERGY:    return ICON_ENERGY_BITMAP;
        case ICON_DOWN:      return ICON_DOWN_BITMAP;
        default:             return ICON_LIGHT_BITMAP;
    }
}

void LightAir_DisplayCtrl::drawBar(uint8_t x, uint8_t y, uint8_t width, uint8_t height, float ratio) {
    uint8_t filled = width * ratio;
    _display.drawRect(x, y, width, height);
    if (filled > 2)
        _display.fillRect(x + 1, y + 1, filled - 2, height - 2);
}
