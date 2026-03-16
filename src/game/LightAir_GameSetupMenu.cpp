#include "LightAir_GameSetupMenu.h"
#include "../enlight/EnlightCalibRoutine.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

#define MGR_NVS_NAMESPACE  "lightair"
#define MGR_NVS_KEY_DM     "is_dm"

/* =========================================================
 *   CONFIG BLOB FREE FUNCTIONS
 * ========================================================= */

uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen,
                                const uint8_t genericRoles[TotemDefs::MAX_TOTEMS]) {
    // Minimum: 4 (typeId) + 16*4 (generic roles) must fit.
    uint16_t minNeeded = 4
        + (uint16_t)game.configCount   * 4
        + (uint16_t)game.totemVarCount * 4
        + (game.hasTeams ? 4 : 0)
        + TotemDefs::MAX_TOTEMS * 4;
    if (maxLen < minNeeded) return 0;

    uint32_t id = game.typeId;
    memcpy(buf, &id, 4);
    uint16_t pos = 4;

    // configVars
    for (uint8_t v = 0; v < game.configCount; v++) {
        int32_t val = (int32_t)*game.configVars[v].value;
        memcpy(buf + pos, &val, 4);
        pos += 4;
    }

    // totemVar IDs
    for (uint8_t v = 0; v < game.totemVarCount; v++) {
        int32_t val = game.totemVars ? (int32_t)*game.totemVars[v].id : 0;
        memcpy(buf + pos, &val, 4);
        pos += 4;
    }

    // teamBitmask (if hasTeams)
    if (game.hasTeams) {
        int32_t mask = game.teamBitmask ? (int32_t)*game.teamBitmask : 0;
        memcpy(buf + pos, &mask, 4);
        pos += 4;
    }

    // 16 generic totem roles (always present; unknown/null → 0)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        int32_t role = (genericRoles ? (int32_t)genericRoles[s] : 0);
        memcpy(buf + pos, &role, 4);
        pos += 4;
    }

    return pos;
}

bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len,
                        uint8_t genericRolesOut[TotemDefs::MAX_TOTEMS]) {
    if (len < 4) return false;
    uint32_t id;
    memcpy(&id, buf, 4);
    if (id != game.typeId) return false;

    uint16_t pos = 4;

    // configVars
    for (uint8_t v = 0; v < game.configCount && pos + 4 <= len; v++) {
        const ConfigVar& var = game.configVars[v];
        int32_t val;
        memcpy(&val, buf + pos, 4);
        pos += 4;
        if (val < var.min) val = var.min;
        if (val > var.max) val = var.max;
        *var.value = (int)val;
    }

    // totemVar IDs
    if (game.totemVars) {
        for (uint8_t v = 0; v < game.totemVarCount && pos + 4 <= len; v++) {
            int32_t val;
            memcpy(&val, buf + pos, 4);
            pos += 4;
            *game.totemVars[v].id = (int)val;
        }
    } else {
        pos += (uint16_t)game.totemVarCount * 4;
    }

    // teamBitmask
    if (game.hasTeams && pos + 4 <= len) {
        int32_t mask;
        memcpy(&mask, buf + pos, 4);
        pos += 4;
        if (game.teamBitmask) *game.teamBitmask = (int)mask;
    }

    // generic roles
    if (genericRolesOut) {
        for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS && pos + 4 <= len; s++) {
            int32_t val;
            memcpy(&val, buf + pos, 4);
            pos += 4;
            genericRolesOut[s] = (uint8_t)val;
        }
    }

    return true;
}

/* =========================================================
 *   CONSTRUCTOR
 * ========================================================= */

LightAir_GameSetupMenu::LightAir_GameSetupMenu(LightAir_GameManager& mgr,
                                                LightAir_GameRunner&  runner,
                                                LightAir_Display&     display,
                                                LightAir_InputCtrl&   input,
                                                uint8_t               keypadId,
                                                LightAir_Radio&       radio,
                                                uint8_t               configMsgType)
    : _mgr(mgr), _runner(runner), _display(display),
      _input(input), _keypadId(keypadId), _radio(radio), _msgType(configMsgType)
{}

/* =========================================================
 *   PUBLIC API
 * ========================================================= */

MenuResult LightAir_GameSetupMenu::run() {
    // ---- S0: DM detection ----
    loadOrDetectDm();
    if (!_isDm) return runWaiter();

    // ---- S1: Restart last game? ----
    bool restart = runRestartPrompt();

    if (!restart) {
        // ---- S2: Game list ----
        runGameList();

        // S4: Setup menu (returns on A)
        runSetupMenu();
    }

    // ---- S5: Pre-start ----
    runPreStart();
    return MenuResult::Confirmed;
}

/* =========================================================
 *   S0 — DM DETECTION
 * ========================================================= */

void LightAir_GameSetupMenu::loadOrDetectDm() {
    // Sample initial key state.
    bool caretPressed = false, bPressed = false;
    {
        const InputReport& rep = _input.poll();
        for (uint8_t i = 0; i < rep.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.key == '^' &&
                (ke.state == KeyState::PRESSED || ke.state == KeyState::HELD))
                caretPressed = true;
            if (ke.key == 'B' &&
                (ke.state == KeyState::PRESSED || ke.state == KeyState::HELD))
                bPressed = true;
        }
    }

    // Confirm ^ hold → DM mode.
    if (caretPressed) {
        delay(InputDefaults::LONG_PRESS_MS);
        const InputReport& rep2 = _input.poll();
        for (uint8_t i = 0; i < rep2.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep2.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.key == '^' && ke.state == KeyState::HELD)
                saveIsDm(true);
        }
    }

    // Confirm B hold → calibration mode (runs before normal setup resumes).
    if (bPressed && _calibRoutine) {
        delay(InputDefaults::LONG_PRESS_MS);
        const InputReport& rep2 = _input.poll();
        for (uint8_t i = 0; i < rep2.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep2.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.key == 'B' && ke.state == KeyState::HELD)
                _calibRoutine->run();
        }
    }

    _isDm = loadIsDm();
}

void LightAir_GameSetupMenu::saveIsDm(bool val) {
    nvs_handle_t h;
    if (nvs_open(MGR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, MGR_NVS_KEY_DM, val ? 1u : 0u);
    nvs_commit(h);
    nvs_close(h);
}

bool LightAir_GameSetupMenu::loadIsDm() {
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(MGR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, MGR_NVS_KEY_DM, &val);
    nvs_close(h);
    return val != 0;
}

/* =========================================================
 *   NON-DM WAITING PATH
 * ========================================================= */

MenuResult LightAir_GameSetupMenu::runWaiter() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,      "Waiting for host");
    _display.print(0, fh,     "A:Join  B:Cancel");
    _display.flush();

    // Find the game matching any incoming config typeId.
    while (true) {
        // Check input
        const InputReport& inp = _input.poll();
        for (uint8_t i = 0; i < inp.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = inp.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.state != KeyState::RELEASED && ke.state != KeyState::RELEASED_HELD) continue;
            if (ke.key == 'B') return MenuResult::Cancelled;
            // A or TRIG1: if a game was already selected (from config receipt), confirm.
            if ((ke.key == 'A') && _game) {
                commitToRunner();
                return MenuResult::Confirmed;
            }
        }

        // Check radio for config packets
        const RadioReport& rad = _radio.poll();
        for (uint8_t e = 0; e < rad.count; e++) {
            const RadioEvent& ev = rad.events[e];
            if (ev.type != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != _msgType) continue;
            // Try to match against registered games.
            for (uint8_t g = 0; g < _mgr.count(); g++) {
                const LightAir_Game& candidate = _mgr.game(g);
                uint8_t genericBuf[TotemDefs::MAX_TOTEMS] = {};
                if (game_apply_config(candidate, ev.packet.payload,
                                      ev.packet.payloadLen, genericBuf)) {
                    _game    = &candidate;
                    _gameIdx = g;
                    // Apply generic roles to runner immediately so commitToRunner can use them.
                    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++)
                        _totemAssignment[s] = genericBuf[s];
                    // Show "Game ready" prompt.
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%.16s", candidate.name);
                    _display.clear();
                    _display.setColor(true);
                    _display.print(0, 0,      "Game ready:");
                    _display.print(0, fh,     buf);
                    _display.print(0, fh * 2, "A:Join  B:Cancel");
                    _display.flush();
                    break;
                }
            }
        }

        delay(GameDefaults::LOOP_MS);
    }
}

/* =========================================================
 *   S1 — RESTART PROMPT
 * ========================================================= */

bool LightAir_GameSetupMenu::runRestartPrompt() {
    uint8_t lastIdx = _mgr.loadLastPlayed();
    if (lastIdx == 0xFF || lastIdx >= _mgr.count()) return false;  // no saved game

    const LightAir_Game& lastGame = _mgr.game(lastIdx);
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;

    char buf[24];
    snprintf(buf, sizeof(buf), "%.16s", lastGame.name);

    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,  "Last:");
    _display.print(0, fh, buf);
    _display.print(0, fh * 2, "A:Restart  B:New");
    _display.flush();

    while (true) {
        char key = waitForKey();
        if (key == 'A') {
            _game    = &lastGame;
            _gameIdx = lastIdx;
            initTotemAssignment();
            return true;
        }
        if (key == 'B') return false;
    }
}

/* =========================================================
 *   S2 — GAME LIST
 * ========================================================= */

void LightAir_GameSetupMenu::renderGameList(uint8_t sel) {
    const uint8_t fh   = DisplayDefaults::FONT_HEIGHT;
    const uint8_t n    = _mgr.count();

    _display.clear();
    _display.setColor(true);

    // Row 0: game above cursor (blank at top)
    if (sel > 0) {
        char buf[20];
        snprintf(buf, sizeof(buf), "  %.16s", _mgr.game(sel - 1).name);
        _display.print(0, 0, buf);
    }

    // Row 1: current game (cursor)
    {
        char buf[20];
        snprintf(buf, sizeof(buf), "> %.16s", _mgr.game(sel).name);
        _display.print(0, fh, buf);
    }

    // Row 2: game below cursor (blank at bottom)
    if (sel + 1 < n) {
        char buf[20];
        snprintf(buf, sizeof(buf), "  %.16s", _mgr.game(sel + 1).name);
        _display.print(0, fh * 2, buf);
    }

    // Row 3: controls
    _display.print(0, fh * 3, "A:Start  B:Setup");

    _display.flush();
}

void LightAir_GameSetupMenu::runGameList() {
    if (_mgr.count() == 0) {
        // Fallback: no games registered.
        _game    = &_mgr.game(0);
        _gameIdx = 0;
        return;
    }

    uint8_t lastIdx = _mgr.loadLastPlayed();
    uint8_t sel = (lastIdx < _mgr.count()) ? lastIdx : 0;

    renderGameList(sel);

    while (true) {
        const InputReport& rep = _input.poll();
        for (uint8_t i = 0; i < rep.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.state != KeyState::RELEASED && ke.state != KeyState::RELEASED_HELD) continue;

            switch (ke.key) {
                case '^':
                    if (sel > 0) { sel--; renderGameList(sel); }
                    break;
                case 'V':
                    if (sel + 1 < _mgr.count()) { sel++; renderGameList(sel); }
                    break;
                case 'A':
                    // Start with default/current config — skip S4.
                    _game    = &_mgr.game(sel);
                    _gameIdx = sel;
                    _mgr.saveLastPlayed(sel);
                    initTotemAssignment();
                    return;
                case 'B':
                    // Enter setup.
                    _game    = &_mgr.game(sel);
                    _gameIdx = sel;
                    _mgr.saveLastPlayed(sel);
                    initTotemAssignment();
                    runSetupMenu();
                    return;
            }
        }
        delay(GameDefaults::LOOP_MS);
    }
}

/* =========================================================
 *   S4 — SETUP MENU
 * ========================================================= */

void LightAir_GameSetupMenu::runSetupMenu() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;

    // Build entry list: always Config + optional Teams + always Totems
    // Entries: 0=Config, 1=Teams (if hasTeams), 2=Totems
    // We always show all 3 rows (blank row if inapplicable).
    // Cursor only moves to applicable entries.

    // Collect applicable entry indices (0=Config,1=Teams,2=Totems)
    // We always allow Config and Totems; Teams only if hasTeams.
    // But cursor only lands on entries that are "active".
    // We represent entries as indices 0..2 and skip via allowed[].
    bool allowed[3] = { true, _game->hasTeams, true };
    uint8_t cursor = 0;

    auto renderSetupMenu = [&]() {
        _display.clear();
        _display.setColor(true);
        const char* labels[3] = { "Config", "Teams", "Totems" };
        for (uint8_t r = 0; r < 3; r++) {
            char buf[20];
            if (allowed[r]) {
                snprintf(buf, sizeof(buf), "%s %.6s",
                         (cursor == r) ? ">" : " ", labels[r]);
            } else {
                buf[0] = '\0';  // blank row
            }
            _display.print(0, fh * r, buf);
        }
        _display.print(0, fh * 3, "A:Start  B:Enter");
        _display.flush();
    };

    renderSetupMenu();

    while (true) {
        char key = waitForKey();
        switch (key) {
            case '^': {
                // Move cursor to previous allowed entry.
                int8_t c = (int8_t)cursor - 1;
                while (c >= 0 && !allowed[c]) c--;
                if (c >= 0) { cursor = (uint8_t)c; renderSetupMenu(); }
                break;
            }
            case 'V': {
                uint8_t c = cursor + 1;
                while (c < 3 && !allowed[c]) c++;
                if (c < 3) { cursor = c; renderSetupMenu(); }
                break;
            }
            case 'B':
                // Enter highlighted submenu.
                if (cursor == 0) runConfigSubmenu();
                else if (cursor == 1) runTeamsSubmenu();
                else                  runTotemsSubmenu();
                renderSetupMenu();
                break;
            case 'A':
                // Validate required totems before proceeding.
                if (!validateTotems()) {
                    // Show error for 2 s.
                    _display.clear();
                    _display.setColor(true);
                    _display.print(0, fh, "Rules not met!");
                    _display.flush();
                    delay(2000);
                    renderSetupMenu();
                } else {
                    return;  // proceed to S5
                }
                break;
        }
    }
}

bool LightAir_GameSetupMenu::validateTotems() const {
    if (!_game->totemVars) return true;
    for (uint8_t i = 0; i < _game->totemVarCount; i++) {
        if (!_game->totemVars[i].required) continue;
        // Check if this totemVar is assigned to any slot.
        uint8_t targetVal = GenericTotemRoles::COUNT + i;
        bool found = false;
        for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
            if (_totemAssignment[s] == targetVal) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

/* =========================================================
 *   S4a — CONFIG SUBMENU
 * ========================================================= */

void LightAir_GameSetupMenu::renderConfigEntry(uint8_t cursor, uint8_t total) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);

    // Show 3 entries: above cursor, cursor, below cursor.
    for (int8_t delta = -1; delta <= 1; delta++) {
        int8_t idx = (int8_t)cursor + delta;
        uint8_t row = (uint8_t)(delta + 1);  // row 0, 1, 2
        if (idx < 0 || idx >= (int8_t)total) continue;

        const ConfigVar& var = _game->configVars[idx];
        char buf[20];
        snprintf(buf, sizeof(buf), "%s%-8s%d",
                 (delta == 0) ? ">" : " ",
                 var.name, *var.value);
        _display.print(0, fh * row, buf);
    }
    _display.print(0, fh * 3, "^/V:sel <>:val B:");
    _display.flush();
}

void LightAir_GameSetupMenu::runConfigSubmenu() {
    if (_game->configCount == 0) {
        showMessage2("Config", "No config vars", "", "B:back");
        while (waitForKey() != 'B') {}
        return;
    }

    uint8_t cursor = 0;
    const uint8_t n = _game->configCount;
    renderConfigEntry(cursor, n);

    while (true) {
        char key = waitForKey();
        const ConfigVar& var = _game->configVars[cursor];
        int step = var.step ? var.step : 1;
        switch (key) {
            case '^':
                if (cursor > 0) { cursor--; renderConfigEntry(cursor, n); }
                break;
            case 'V':
                if (cursor < n - 1) { cursor++; renderConfigEntry(cursor, n); }
                break;
            case '<': {
                int val = *var.value - step;
                if (val < var.min) val = var.min;
                *var.value = val;
                renderConfigEntry(cursor, n);
                break;
            }
            case '>': {
                int val = *var.value + step;
                if (val > var.max) val = var.max;
                *var.value = val;
                renderConfigEntry(cursor, n);
                break;
            }
            case 'B': return;
        }
    }
}

/* =========================================================
 *   S4b — TEAMS SUBMENU
 * ========================================================= */

void LightAir_GameSetupMenu::renderTeamEntry(uint8_t cursor) {
    // cursor is a player ID index 0-based (0 = player ID 1, …, 14 = player ID 15).
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);

    for (int8_t delta = -1; delta <= 1; delta++) {
        int8_t idx = (int8_t)cursor + delta;
        uint8_t row = (uint8_t)(delta + 1);
        if (idx < 0 || idx >= (PlayerDefs::MAX_PLAYER_ID - 1)) continue;

        uint8_t pid = (uint8_t)(idx + 1);  // player IDs 1–15
        char buf[20];
        snprintf(buf, sizeof(buf), "%s%-3s  %c",
                 (delta == 0) ? ">" : " ",
                 PlayerDefs::playerShort[pid],
                 _teams[pid] ? 'X' : 'O');
        _display.print(0, fh * row, buf);
    }
    _display.print(0, fh * 3, "^/V:sel <>:team B:");
    _display.flush();
}

void LightAir_GameSetupMenu::runTeamsSubmenu() {
    uint8_t cursor = 0;  // 0-based index into player IDs 1..15
    renderTeamEntry(cursor);

    while (true) {
        char key = waitForKey();
        uint8_t pid = cursor + 1;
        switch (key) {
            case '^':
                if (cursor > 0) { cursor--; renderTeamEntry(cursor); }
                break;
            case 'V':
                if (cursor < PlayerDefs::MAX_PLAYER_ID - 2) { cursor++; renderTeamEntry(cursor); }
                break;
            case '<':
            case '>':
                _teams[pid] ^= 1;  // toggle O/X
                renderTeamEntry(cursor);
                break;
            case 'B': return;
        }
    }
}

/* =========================================================
 *   S4c — TOTEMS SUBMENU
 * ========================================================= */

void LightAir_GameSetupMenu::initTotemAssignment() {
    memset(_totemAssignment, 0, sizeof(_totemAssignment));
    if (!_game->totemVars) return;

    for (uint8_t i = 0; i < _game->totemVarCount; i++) {
        int id = *_game->totemVars[i].id;
        if (id == 0) continue;
        if (!TotemDefs::isTotemId((uint8_t)id)) continue;
        uint8_t slot = TotemDefs::totemIndex((uint8_t)id);
        _totemAssignment[slot] = GenericTotemRoles::COUNT + i;
    }
}

bool LightAir_GameSetupMenu::isRoleAvailable(uint8_t slot, uint8_t val) const {
    // Generic roles are always available.
    if (val < GenericTotemRoles::COUNT) return true;
    // totemVar roles are available only if not assigned to another slot.
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        if (s == slot) continue;
        if (_totemAssignment[s] == val) return false;
    }
    return true;
}

const char* LightAir_GameSetupMenu::totemRoleLabel(uint8_t val) const {
    static char buf[12];
    if (val < GenericTotemRoles::COUNT) {
        return GenericTotemRoles::names[val];
    }
    uint8_t tvIdx = val - GenericTotemRoles::COUNT;
    if (_game->totemVars && tvIdx < _game->totemVarCount) {
        const TotemVar& tv = _game->totemVars[tvIdx];
        if (tv.required) {
            snprintf(buf, sizeof(buf), "%.9s*", tv.name);
        } else {
            snprintf(buf, sizeof(buf), "%.10s", tv.name);
        }
        return buf;
    }
    return "?";
}

uint8_t LightAir_GameSetupMenu::nextTotemRole(uint8_t slot, int8_t dir) const {
    // Total options = GenericTotemRoles::COUNT + totemVarCount.
    uint8_t tvCount = _game->totemVars ? _game->totemVarCount : 0;
    uint8_t total   = GenericTotemRoles::COUNT + tvCount;
    uint8_t cur     = _totemAssignment[slot];

    // Step dir until we find an available option (or exhaust all).
    for (uint8_t attempt = 0; attempt < total; attempt++) {
        if (dir > 0) {
            cur = (cur + 1 >= total) ? 0 : cur + 1;
        } else {
            cur = (cur == 0) ? total - 1 : cur - 1;
        }
        if (isRoleAvailable(slot, cur)) return cur;
    }
    return _totemAssignment[slot];  // no change possible
}

void LightAir_GameSetupMenu::renderTotemEntry(uint8_t cursor) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);

    for (int8_t delta = -1; delta <= 1; delta++) {
        int8_t slot = (int8_t)cursor + delta;
        uint8_t row = (uint8_t)(delta + 1);
        if (slot < 0 || slot >= (int8_t)TotemDefs::MAX_TOTEMS) continue;

        char buf[20];
        snprintf(buf, sizeof(buf), "%s%s: %-8s",
                 (delta == 0) ? ">" : " ",
                 TotemDefs::totemShort[slot],
                 totemRoleLabel(_totemAssignment[slot]));
        _display.print(0, fh * row, buf);
    }
    _display.print(0, fh * 3, "^/V:sel <>:role B:");
    _display.flush();
}

void LightAir_GameSetupMenu::runTotemsSubmenu() {
    uint8_t cursor = 0;
    renderTotemEntry(cursor);

    while (true) {
        char key = waitForKey();
        switch (key) {
            case '^':
                if (cursor > 0) { cursor--; renderTotemEntry(cursor); }
                break;
            case 'V':
                if (cursor < TotemDefs::MAX_TOTEMS - 1) { cursor++; renderTotemEntry(cursor); }
                break;
            case '<':
                _totemAssignment[cursor] = nextTotemRole(cursor, -1);
                renderTotemEntry(cursor);
                break;
            case '>':
                _totemAssignment[cursor] = nextTotemRole(cursor, +1);
                renderTotemEntry(cursor);
                break;
            case 'B': return;
        }
    }
}

/* =========================================================
 *   S5 — PRE-START SEQUENCE
 * ========================================================= */

void LightAir_GameSetupMenu::runPreStart() {
    // Phase A: share config?
    shareConfig();

    // Phase B + C: discovery + summary (repeatable with B).
    while (true) {
        runDiscovery();

        uint8_t vScroll = 0, hScroll = 0;
        renderSummary(vScroll, hScroll);

        while (true) {
            char key = waitForKey();
            if (key == 'A') {
                commitToRunner();
                return;
            }
            if (key == 'B') {
                // Redo discovery.
                break;
            }
            if (key == '^' && vScroll > 0) { vScroll--; renderSummary(vScroll, hScroll); }
            if (key == 'V') { vScroll++; renderSummary(vScroll, hScroll); }
            if (key == '<' && hScroll > 0) { hScroll--; renderSummary(vScroll, hScroll); }
            if (key == '>') { hScroll++; renderSummary(vScroll, hScroll); }
        }
    }
}

void LightAir_GameSetupMenu::shareConfig() {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,  "Share config?");
    _display.print(0, fh, "A:YES  B:Skip");
    _display.flush();

    char key = waitForKey();
    if (key != 'A') return;

    // Build genericRoles array from _totemAssignment (only generic values < COUNT).
    uint8_t genericBuf[TotemDefs::MAX_TOTEMS] = {};
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t v = _totemAssignment[s];
        genericBuf[s] = (v < GenericTotemRoles::COUNT) ? v : 0;
    }

    uint8_t blob[4 + 32 * 4 + 16 * 4 + 4 + 16 * 4];  // generous upper bound
    uint16_t len = game_serialize_config(*_game, blob, sizeof(blob), genericBuf);
    if (len > 0) _radio.broadcast(_msgType, blob, len);
}

void LightAir_GameSetupMenu::runDiscovery() {
    _seenCount = 0;

    // Add self immediately.
    recordSeen(_radio.playerId());

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
        if (millis() >= nextBroadcast) {
            _radio.broadcast(GameDefaults::MSG_ROSTER, nullptr, 0);
            nextBroadcast = millis() + GameDefaults::ROSTER_RETRY_MS;
        }

        const RadioReport& rep = _radio.poll();
        for (uint8_t i = 0; i < rep.count; i++) {
            const RadioEvent& ev = rep.events[i];
            if (ev.type != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != GameDefaults::MSG_ROSTER) continue;
            recordSeen(ev.packet.senderId);
        }

        delay(GameDefaults::LOOP_MS);
    }
}

void LightAir_GameSetupMenu::recordSeen(uint8_t id) {
    for (uint8_t i = 0; i < _seenCount; i++)
        if (_seenIds[i] == id) return;
    if (_seenCount < MAX_DISC) _seenIds[_seenCount++] = id;
}

bool LightAir_GameSetupMenu::wasSeen(uint8_t id) const {
    for (uint8_t i = 0; i < _seenCount; i++)
        if (_seenIds[i] == id) return true;
    return false;
}

void LightAir_GameSetupMenu::renderSummary(uint8_t vScroll, uint8_t hScroll) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);

    // Row 0: player count (+team breakdown if hasTeams)
    {
        // Count players (IDs not in totem range)
        uint8_t nPlayers = 0, nO = 0, nX = 0;
        for (uint8_t i = 0; i < _seenCount; i++) {
            uint8_t id = _seenIds[i];
            if (TotemDefs::isTotemId(id)) continue;
            nPlayers++;
            if (_game->hasTeams) {
                if (_teams[id] == 0) nO++; else nX++;
            }
        }
        char buf[24];
        if (_game->hasTeams) {
            snprintf(buf, sizeof(buf), "Players:%u(%uO %uX)", nPlayers, nO, nX);
        } else {
            snprintf(buf, sizeof(buf), "Players: %u", nPlayers);
        }
        _display.print(0, 0, buf);
    }

    // Rows 1–2: totem entries (from _totemAssignment, non----- slots)
    // Build the flat totem entry list.
    struct TotemEntry { uint8_t slot; uint8_t val; };
    TotemEntry entries[TotemDefs::MAX_TOTEMS];
    uint8_t    entryCount = 0;
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t v = _totemAssignment[s];
        if (v == GenericTotemRoles::NONE) continue;
        entries[entryCount++] = { s, v };
    }

    // Show 2 rows of totem entries with vertical scroll.
    for (uint8_t row = 0; row < 2; row++) {
        uint8_t ei = vScroll + row;
        if (ei >= entryCount) break;

        const TotemEntry& te = entries[ei];
        uint8_t id = TotemDefs::idFromIndex(te.slot);
        bool seen  = wasSeen(id);

        // Build full line then horizontal-scroll.
        char full[48];
        uint8_t off = 0;

        // Role label
        const char* label = totemRoleLabel(te.val);
        off += snprintf(full + off, sizeof(full) - off, "%s: %s %s",
                        label,
                        TotemDefs::totemShort[te.slot],
                        seen ? "\x7F" : "X");  // ✓ (0x7F or similar) vs X

        // Team annotation for named totemVars
        if (te.val >= GenericTotemRoles::COUNT && _game->totemVars) {
            uint8_t tvIdx = te.val - GenericTotemRoles::COUNT;
            if (tvIdx < _game->totemVarCount) {
                const TotemVar& tv = _game->totemVars[tvIdx];
                if (tv.team != 0xFF) {
                    off += snprintf(full + off, sizeof(full) - off,
                                    " %c", tv.team == 0 ? 'O' : 'X');
                }
            }
        }

        // Horizontal scroll: clip to 18 chars
        constexpr uint8_t ROW_CHARS = 18;
        uint8_t start = (hScroll < strlen(full)) ? hScroll : 0;
        char rowBuf[ROW_CHARS + 2];
        snprintf(rowBuf, sizeof(rowBuf), "%s", full + start);
        _display.print(0, fh * (row + 1), rowBuf);
    }

    _display.print(0, fh * 3, "A:Start  B:Redo");
    _display.flush();
}

void LightAir_GameSetupMenu::commitToRunner() {
    _runner.clearRoster();
    _runner.clearTotems();

    // Clear previous team/generic role maps.
    for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++) _runner.setTeam(i, _teams[i]);
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t v = _totemAssignment[s];
        if (v > 0 && v < GenericTotemRoles::COUNT) {
            _runner.setGenericTotemRole(s, v);
        }
    }

    // Add players to roster.
    for (uint8_t i = 0; i < _seenCount; i++) {
        uint8_t id = _seenIds[i];
        if (!TotemDefs::isTotemId(id)) _runner.addToRoster(id);
    }

    // Add configured totems to roster + totem list.
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t v = _totemAssignment[s];
        if (v == GenericTotemRoles::NONE) continue;

        uint8_t id = TotemDefs::idFromIndex(s);

        // Named totemVar role
        if (v >= GenericTotemRoles::COUNT && _game->totemVars) {
            uint8_t tvIdx = v - GenericTotemRoles::COUNT;
            if (tvIdx < _game->totemVarCount) {
                *_game->totemVars[tvIdx].id = (int)id;
                _runner.addToRoster(id);
                _runner.addTotem(id, tvIdx);
                continue;
            }
        }

        // Generic role — add to roster only (no named role entry).
        if (v < GenericTotemRoles::COUNT) {
            _runner.addToRoster(id);
        }
    }
}

/* =========================================================
 *   SHARED INPUT
 * ========================================================= */

char LightAir_GameSetupMenu::waitForKey() {
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

void LightAir_GameSetupMenu::showMessage2(const char* l0, const char* l1,
                                           const char* l2, const char* l3) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    if (l0 && l0[0]) _display.print(0, 0,      l0);
    if (l1 && l1[0]) _display.print(0, fh,     l1);
    if (l2 && l2[0]) _display.print(0, fh * 2, l2);
    if (l3 && l3[0]) _display.print(0, fh * 3, l3);
    _display.flush();
}
