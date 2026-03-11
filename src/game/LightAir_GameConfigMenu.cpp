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

LightAir_GameConfigMenu::LightAir_GameConfigMenu(const LightAir_Game&  game,
                                                   LightAir_GameRunner&  runner,
                                                   LightAir_Display&     display,
                                                   LightAir_InputCtrl&   input,
                                                   uint8_t               keypadId,
                                                   LightAir_Radio&       radio,
                                                   uint8_t               msgType)
    : _game(game), _runner(runner), _display(display), _input(input),
      _keypadId(keypadId), _radio(radio), _msgType(msgType)
{
    _configCount = game.configCount < DisplayDefaults::MAX_BINDINGS
                 ? game.configCount
                 : DisplayDefaults::MAX_BINDINGS;
}

/* =========================================================
 *   PUBLIC API
 * ========================================================= */

MenuResult LightAir_GameConfigMenu::run() {
    // ---- Phase 1: Config var editor ----
    if (_configCount > 0) {
        uint8_t cur = 0;
        renderMenu(cur);

        bool editing = true;
        while (editing) {
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
                    editing = false;
                    break;
                case 'B':
                    return MenuResult::Cancelled;
            }
        }
    }

    // ---- Phase 2: Config share ----
    if (promptShare()) shareConfig();

    // ---- Phase 3 + 4: Roster discovery and summary (repeatable) ----
    runDiscovery();
    while (true) {
        renderSummary();
        char key = waitForKey();
        if (key == 'A') {
            commitToRunner();
            return MenuResult::Confirmed;
        }
        if (key == 'B') {
            runDiscovery();  // redo discovery
        }
    }
}

/* =========================================================
 *   PHASE 1 — CONFIG EDITOR
 * ========================================================= */

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

/* =========================================================
 *   PHASE 2 — CONFIG SHARE
 * ========================================================= */

bool LightAir_GameConfigMenu::promptShare() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,  "Share config?");
    _display.print(0, fh, "A: YES   B: Skip");
    _display.flush();
    return waitForKey() == 'A';
}

void LightAir_GameConfigMenu::shareConfig() {
    uint8_t blob[4 + GameDefaults::MAX_GAMES * 4];  // generous upper bound
    uint16_t len = game_serialize_config(_game, blob, sizeof(blob));
    if (len > 0) _radio.broadcast(_msgType, blob, len);
}

/* =========================================================
 *   PHASE 3 — ROSTER DISCOVERY
 * ========================================================= */

void LightAir_GameConfigMenu::runDiscovery() {
    _playerCount = 0;
    _totemCount  = 0;

    // Add self immediately.
    recordParticipant(_radio.playerId(), _radio.role());

    // Show discovery screen.
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,      "Discovering...");
    _display.print(0, fh,     "Listening 3s");
    _display.print(0, fh * 3, "Please wait");
    _display.flush();

    uint32_t windowEnd     = millis() + GameDefaults::ROSTER_WINDOW_MS;
    uint32_t nextBroadcast = 0;

    while (millis() < windowEnd) {
        // Re-broadcast own presence periodically.
        if (millis() >= nextBroadcast) {
            uint8_t payload = _radio.role();
            _radio.broadcast(GameDefaults::MSG_ROSTER, &payload, 1);
            nextBroadcast = millis() + GameDefaults::ROSTER_RETRY_MS;
        }

        // Collect incoming presence announcements.
        const RadioReport& rep = _radio.poll();
        for (uint8_t i = 0; i < rep.count; i++) {
            const RadioEvent& ev = rep.events[i];
            if (ev.type != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != GameDefaults::MSG_ROSTER) continue;
            if (ev.packet.payloadLen < 1) continue;
            recordParticipant(ev.packet.senderId, ev.packet.payload[0]);
        }

        delay(GameDefaults::LOOP_MS);
    }
}

void LightAir_GameConfigMenu::recordParticipant(uint8_t id, uint8_t role) {
    if (role == 0) {
        // Shooter / player.
        for (uint8_t i = 0; i < _playerCount; i++)
            if (_players[i] == id) return;  // duplicate
        if (_playerCount < MAX_DISC) _players[_playerCount++] = id;
    } else {
        // Totem — role value - 1 = totemRoles[] index.
        uint8_t roleIdx = role - 1;
        if (roleIdx >= _game.totemRoleCount) return;  // role unknown for this game
        for (uint8_t i = 0; i < _totemCount; i++)
            if (_totems[i].id == id) return;  // duplicate
        if (_totemCount < MAX_DISC) _totems[_totemCount++] = { id, roleIdx };
    }
}

/* =========================================================
 *   PHASE 4 — ROSTER SUMMARY
 * ========================================================= */

void LightAir_GameConfigMenu::renderSummary() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    char buf[24];

    _display.clear();
    _display.setColor(true);

    // Row 0: player count.
    snprintf(buf, sizeof(buf), "Players: %u", _playerCount);
    _display.print(0, 0, buf);

    // Rows 1–2: totem roles (up to 2 lines — row 3 reserved for controls).
    uint8_t row = 1;
    for (uint8_t r = 0; r < _game.totemRoleCount && row <= 2; r++) {
        // Collect totem IDs for this role, comma-separated.
        char line[24];
        uint8_t off = 0;

        // Role label — truncate name to 6 chars to leave room for IDs.
        off += snprintf(line + off, sizeof(line) - off, "%.6s:", _game.totemRoles[r].name);

        bool any = false;
        for (uint8_t i = 0; i < _totemCount && off < 20; i++) {
            if (_totems[i].roleIdx != r) continue;
            if (any) { line[off++] = ','; }
            off += snprintf(line + off, sizeof(line) - off, "%u", _totems[i].id);
            any = true;
        }
        if (!any) {
            snprintf(line + off, sizeof(line) - off, "-");
        }

        _display.print(0, fh * row, line);
        row++;
    }

    // Row 3: controls.
    _display.print(0, fh * 3, "A:Start  B:Redo");

    _display.flush();
}

void LightAir_GameConfigMenu::commitToRunner() {
    _runner.clearRoster();
    _runner.clearTotems();

    // Players first, then totems (totems also enter the roster for scoring).
    for (uint8_t i = 0; i < _playerCount; i++)
        _runner.addToRoster(_players[i]);
    for (uint8_t i = 0; i < _totemCount; i++) {
        _runner.addToRoster(_totems[i].id);
        _runner.addTotem(_totems[i].id, _totems[i].roleIdx);
    }
}

/* =========================================================
 *   SHARED INPUT
 * ========================================================= */

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
