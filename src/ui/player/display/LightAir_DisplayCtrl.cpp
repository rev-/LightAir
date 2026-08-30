#include "LightAir_DisplayCtrl.h"

/* =========================================================
 *   CONSTRUCTOR
 * ========================================================= */

LightAir_DisplayCtrl::LightAir_DisplayCtrl(LightAir_Display& display)
: _display(display),
  _setCount(0),
  _selectedSet(0),
  _activeSet(0),
  _pendingClear(false)
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
        _pendingClear = true;
    }
}

/* =========================================================
 *   BINDING
 * ========================================================= */

bool LightAir_DisplayCtrl::bindIntVariable(int* variable, IconType icon, uint8_t x, uint8_t y) {
    if (y < DisplayDefaults::TRAY_HEIGHT) return false;

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

bool LightAir_DisplayCtrl::bindBarVariable(
    int* variable,
    IconType icon,
    uint8_t x,
    uint8_t y,
    int triggerValue,
    const int* fillSecs,
    uint8_t barWidth,
    const int* startMs
) {
    if (y < DisplayDefaults::TRAY_HEIGHT) return false;
    if (barWidth == 0 || barWidth > DisplayDefaults::BAR_WIDTH) return false;

    BindingSet& set = _sets[_selectedSet];
    if (set.locked || set.count >= DisplayDefaults::MAX_BINDINGS) return false;

    VariableBinding& b = set.bindings[set.count++];
    b.variable  = variable;
    b.icon      = icon;
    b.type      = TYPE_BAR;
    b.x         = x;
    b.y         = y;
    b.trigger   = triggerValue;
    b.fillSecs  = fillSecs;
    b.startMs   = startMs;
    b.fillStart = 0;
    b.filling   = false;
    b.barWidth  = barWidth;
    b.lastValue = INT32_MIN;
    return true;
}

bool LightAir_DisplayCtrl::bindStringVariable(const char* str, IconType icon, uint8_t x, uint8_t y) {
    if (y < DisplayDefaults::TRAY_HEIGHT) return false;

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
    if (_pendingClear) {
        _display.clear();
        // Force every binding in the new active set to redraw from scratch.
        BindingSet& s = _sets[_activeSet];
        for (uint8_t i = 0; i < s.count; i++) {
            if (s.bindings[i].type == TYPE_STRING)
                s.bindings[i].lastText[0] = '\0';
            else
                s.bindings[i].lastValue = INT32_MIN;
            // Entering a state is what starts a respawn wait, and a bar bound
            // to an always-at-trigger variable sees no value change to key off.
            if (s.bindings[i].type == TYPE_BAR)
                s.bindings[i].filling = false;
        }
        // Any tray message already queued must be redrawn on the fresh buffer.
        for (uint8_t i = 0; i < DisplayDefaults::TRAY_MAX_MESSAGES; i++) {
            if (_tray[i].active) _tray[i].dirty = true;
        }
        _pendingClear = false;
    }

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
    else if (b.type == TYPE_BAR)
        renderBar(b);
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
    _display.print(b.x + DisplayDefaults::ICON_GUTTER, b.y, buf);
}

void LightAir_DisplayCtrl::renderBar(VariableBinding& b) {
    int value = *b.variable;

    // Away from the trigger this slot is an ordinary number.
    if (value != b.trigger) {
        b.filling = false;
        renderInt(b);
        return;
    }

    // Arrived at the trigger (or the set was just activated): start filling.
    if (!b.filling) {
        b.fillStart = millis();
        b.filling   = true;
        b.lastValue = INT32_MIN;      // force the first bar frame to draw
    }

    uint32_t fillMs = b.fillSecs ? (uint32_t)(*b.fillSecs) * 1000u : 0u;
    if (fillMs == 0) return;          // nothing to time — leave the slot as is

    // The owner may know when the wait really began — a projector's recharge
    // starts on the trigger's release, not when the pool hit zero.  0 there
    // means "at the trigger, but not yet waiting": draw the bar empty rather
    // than animating a countdown that has not started.
    uint32_t start = b.fillStart;
    if (b.startMs) {
        if (*b.startMs == 0) {
            if (b.lastValue == 0) return;
            b.lastValue = 0;
            _display.setColor(false);
            _display.fillRect(b.x, b.y, DisplayDefaults::CELL_WIDTH,
                              DisplayDefaults::CELL_HEIGHT);
            _display.setColor(true);
            drawIcon(ICON_HOURGLASS, b.x, b.y + DisplayDefaults::FONT_TOP_PADDING);
            drawBar(b.x + DisplayDefaults::ICON_GUTTER, b.y + 2,
                    b.barWidth, DisplayDefaults::BAR_HEIGHT, 0.0f);
            return;
        }
        start = (uint32_t)*b.startMs;
    }

    uint32_t elapsed = millis() - start;
    if (elapsed > fillMs) elapsed = fillMs;

    // Redraw only when the drawn length would actually change.
    int filled = (int)((elapsed * b.barWidth) / fillMs);
    if (filled == b.lastValue) return;
    b.lastValue = filled;

    _display.setColor(false);
    _display.fillRect(b.x, b.y, DisplayDefaults::CELL_WIDTH, DisplayDefaults::CELL_HEIGHT);
    _display.setColor(true);

    drawIcon(ICON_HOURGLASS, b.x, b.y + DisplayDefaults::FONT_TOP_PADDING);
    drawBar(b.x + DisplayDefaults::ICON_GUTTER, b.y + 2,
            b.barWidth, DisplayDefaults::BAR_HEIGHT, (float)elapsed / (float)fillMs);
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
    _display.print(b.x + DisplayDefaults::ICON_GUTTER, b.y, b.lastText);
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
        uint8_t y = i * DisplayDefaults::FONT_HEIGHT;
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

// The one bar renderer in the firmware: every gauge on the player LCD
// comes through here.
void LightAir_DisplayCtrl::drawBar(uint8_t x, uint8_t y, uint8_t width, uint8_t height, float ratio) {
    uint8_t filled = width * ratio;
    _display.drawRect(x, y, width, height);
    if (filled > 2)
        _display.fillRect(x + 1, y + 1, filled - 2, height - 2);
}
