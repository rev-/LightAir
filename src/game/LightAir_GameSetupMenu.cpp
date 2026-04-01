#include "LightAir_GameSetupMenu.h"
#include "../enlight/EnlightCalibRoutine.h"
#include "../nvs_config.h"
#include "../totem-rulesets/TotemRoleIds.h"
#include <Arduino.h>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "GameConfig";

#define MGR_NVS_NAMESPACE  "lightair"
#define MGR_NVS_KEY_DM     "is_dm"

/* =========================================================
 *   CONFIG BLOB FREE FUNCTIONS
 * ========================================================= */

uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen,
                                const uint8_t totemAssignment[TotemDefs::MAX_TOTEMS],
                                const uint8_t teamMap[PlayerDefs::MAX_PLAYER_ID],
                                uint8_t sessionToken) {
    uint16_t minNeeded = 2
        + (uint16_t)game.configCount * 4
        + (game.teamCount > 0 ? PlayerDefs::MAX_PLAYER_ID : 0)
        + TotemDefs::MAX_TOTEMS   // 16 × uint8_t roleId
        + 1;
    if (maxLen < minNeeded) return 0;

    uint16_t id = game.typeId;
    memcpy(buf, &id, 2);
    uint16_t pos = 2;

    // configVars
    for (uint8_t v = 0; v < game.configCount; v++) {
        int32_t val = (int32_t)*game.configVars[v].value;
        memcpy(buf + pos, &val, 4);
        pos += 4;
    }

    // teamMap (MAX_PLAYER_ID bytes; only if game.teamCount > 0)
    if (game.teamCount > 0) {
        for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++)
            buf[pos++] = teamMap ? teamMap[i] : 0xFF;
    }

    // 16 totem slot assignments (roleId per slot; 0 = unassigned)
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++)
        buf[pos++] = totemAssignment ? totemAssignment[s] : 0;

    // Session token (1 byte; 0 = no session isolation)
    buf[pos++] = sessionToken;

    return pos;
}

bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len,
                        uint8_t totemAssignmentOut[TotemDefs::MAX_TOTEMS],
                        uint8_t teamMapOut[PlayerDefs::MAX_PLAYER_ID],
                        uint8_t* sessionTokenOut) {
    uint16_t minNeeded = 2
        + (uint16_t)game.configCount * 4
        + (game.teamCount > 0 ? PlayerDefs::MAX_PLAYER_ID : 0)
        + TotemDefs::MAX_TOTEMS
        + 1;
    if (len < minNeeded) {
        ESP_LOGW(TAG, "config blob too short: got %u, need %u", (unsigned)len, (unsigned)minNeeded);
        return false;
    }

    uint16_t id;
    memcpy(&id, buf, 2);
    if (id != game.typeId) return false;

    uint16_t pos = 2;

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

    // teamMap (MAX_PLAYER_ID bytes; only if game.teamCount > 0)
    if (game.teamCount > 0) {
        for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID && pos < len; i++) {
            uint8_t t = buf[pos++];
            if (teamMapOut)    teamMapOut[i]    = t;
            if (game.teamMap) game.teamMap[i]   = t;
        }
    }

    // totem slot assignments (roleId per slot)
    if (totemAssignmentOut) {
        for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS && pos < len; s++)
            totemAssignmentOut[s] = buf[pos++];
    } else {
        pos += TotemDefs::MAX_TOTEMS;
    }

    // Session token (last byte)
    if (sessionTokenOut)
        *sessionTokenOut = (pos < len) ? buf[pos] : 0;

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
    _isDm = loadIsDm();

    // ---- Home screen ----
    while (true) {
        const uint8_t pid = _radio.playerId();
        char playerLine[20];
        snprintf(playerLine, sizeof(playerLine), "Player %s",
                 (pid < PlayerDefs::MAX_PLAYER_ID) ? PlayerDefs::playerNames[pid] : "???");
        _display.clear();
        _display.setColor(true);
        _display.print(0, 0,                              "Welcome to LightAir");
        _display.print(0, DisplayDefaults::FONT_HEIGHT,    playerLine);
        printLegend("O:Play  X:Settings", DisplayDefaults::BOTTOM_LINE_Y);
        _display.flush();

        char key = waitForKey();
        if (key == 'B') {
            runSettingsMenu();
            _isDm = loadIsDm();   // refresh in case DM was toggled
            continue;
        }
        if (key == 'A') break;
    }

    // ---- Branch on DM ----
    if (!_isDm) return runWaiter();

    // ---- S1: Restart last game? ----
    bool restart = runRestartPrompt();

    if (!restart) {
        // ---- S2: Game list ----
        runGameList();
    }

    // ---- S4 + S5: Setup → Pre-start (B in pre-start returns here) ----
    while (true) {
        runSetupMenu();
        if (runPreStart() == MenuResult::Confirmed) break;
    }
    return MenuResult::Confirmed;
}

/* =========================================================
 *   HOME / SETTINGS
 * ========================================================= */

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

void LightAir_GameSetupMenu::runSettingsMenu() {
    static const char* const kEntries[] = { "Calibration", "ID / DM" };
    static constexpr uint8_t kCount = 2;
    uint8_t sel = 0;

    while (true) {
        _display.clear();
        _display.setColor(true);
        _display.print(0, 0, "-- Settings --");
        for (uint8_t i = 0; i < kCount; i++) {
            char row[20];
            snprintf(row, sizeof(row), "%c %s", (i == sel) ? '>' : ' ', kEntries[i]);
            _display.print(0, DisplayDefaults::FONT_HEIGHT * (1 + i), row);
        }
        printLegend("O:Select  X:Back", DisplayDefaults::BOTTOM_LINE_Y);
        _display.flush();

        char key = waitForKey();
        if (key == 'B') return;
        if (key == '^' && sel > 0) { sel--; continue; }
        if (key == 'V' && sel < kCount - 1) { sel++; continue; }
        if (key == 'A') {
            if (sel == 0 && _calibRoutine) _calibRoutine->run();
            if (sel == 1) runIdSettings();
        }
    }
}

void LightAir_GameSetupMenu::runIdSettings() {
    PlayerConfig cfg;
    player_config_load(cfg);
    uint8_t id = (cfg.id < PlayerDefs::MAX_PLAYER_ID) ? cfg.id : 0;
    bool dm = _isDm;
    uint8_t cursor = 0;  // 0 = ID row, 1 = DM row

    while (true) {
        char idRow[20], dmRow[20];
        snprintf(idRow, sizeof(idRow), "%cID: %s",
                 (cursor == 0) ? '>' : ' ',
                 PlayerDefs::playerNames[id]);
        snprintf(dmRow, sizeof(dmRow), "%cDM: %s",
                 (cursor == 1) ? '>' : ' ',
                 dm ? "Yes" : "No");
        _display.clear();
        _display.setColor(true);
        _display.print(0, 0,                                                            idRow);
        _display.print(0, DisplayDefaults::FONT_HEIGHT,                                 dmRow);
        printLegend("</> Chg  ^/V Nav", DisplayDefaults::BOTTOM_LINE_Y - DisplayDefaults::FONT_HEIGHT);
        printLegend("O:Save   X:Cancel", DisplayDefaults::BOTTOM_LINE_Y);
        _display.flush();

        char key = waitForKey();
        if (key == 'B') return;
        if (key == '^' && cursor > 0) { cursor--; continue; }
        if (key == 'V' && cursor < 1) { cursor++; continue; }
        if (key == '<') {
            if (cursor == 0) id = (id == 0) ? (PlayerDefs::MAX_PLAYER_ID - 1) : (id - 1);
            if (cursor == 1) dm = !dm;
        }
        if (key == '>') {
            if (cursor == 0) id = (id + 1) % PlayerDefs::MAX_PLAYER_ID;
            if (cursor == 1) dm = !dm;
        }
        if (key == 'A') {
            const bool idChanged = (id != cfg.id);
            cfg.id = id;
            player_config_save(cfg);
            saveIsDm(dm);
            if (idChanged) {
                showMessage2("ID saved.", "Reboot to apply.", "", "");
                waitForKey();
            }
            return;
        }
    }
}

/* =========================================================
 *   NON-DM WAITING PATH
 * ========================================================= */

MenuResult LightAir_GameSetupMenu::runWaiter() {
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,      "Waiting for host");
    printLegend("O:Join  X:Cancel", DisplayDefaults::BOTTOM_LINE_Y);
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
                uint8_t token = 0;
                if (game_apply_config(candidate, ev.packet.payload,
                                      ev.packet.payloadLen, _totemAssignment, _teams, &token)) {
                    _game    = &candidate;
                    _gameIdx = g;
                    if (token != 0) _radio.setSessionToken(token);
                    // Show "Game ready" prompt.
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%.16s", candidate.name);
                    _display.clear();
                    _display.setColor(true);
                    _display.print(0, 0,                              "Game ready:");
                    _display.print(0, DisplayDefaults::FONT_HEIGHT,   buf);
                    printLegend("O:Join  X:Cancel", DisplayDefaults::BOTTOM_LINE_Y);
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

    char buf[24];
    snprintf(buf, sizeof(buf), "%.16s", lastGame.name);

    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,                              "Last:");
    _display.print(0, DisplayDefaults::FONT_HEIGHT,   buf);
    printLegend("O:Restart  X:New", DisplayDefaults::BOTTOM_LINE_Y);
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
    const uint8_t n = _mgr.count();

    _display.clear();
    _display.setColor(true);

    // Show up to 5 entries: sel-2 … sel+2, cursor pinned to row 2.
    for (int8_t delta = -2; delta <= 2; delta++) {
        int8_t idx = (int8_t)sel + delta;
        if (idx < 0 || idx >= (int8_t)n) continue;
        uint8_t row = (uint8_t)(delta + 2);  // rows 0-4
        char buf[20];
        snprintf(buf, sizeof(buf), "%s%.16s",
                 (delta == 0) ? "> " : "  ", _mgr.game((uint8_t)idx).name);
        _display.print(0, DisplayDefaults::FONT_HEIGHT * row, buf);
    }

    printLegend("O:Start  X:Setup", DisplayDefaults::BOTTOM_LINE_Y);
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

    // Build entry list: always Config + optional Teams + always Totems
    // Entries: 0=Config, 1=Teams (if teamCount > 0), 2=Totems
    // Cursor only lands on applicable entries (blank row if inapplicable).
    bool allowed[3] = { true, _game->teamCount > 0, true };
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
            _display.print(0, DisplayDefaults::FONT_HEIGHT * r, buf);
        }
        printLegend("O:Start  X:Enter", DisplayDefaults::BOTTOM_LINE_Y);
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
                    _display.print(0, DisplayDefaults::FONT_HEIGHT, "Rules not met!");
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
    if (!_game->totemRequirements) return true;
    for (uint8_t r = 0; r < _game->totemRequirementCount; r++) {
        const LightAir_TotemRequirement& req = _game->totemRequirements[r];
        if (req.minCount == 0) continue;
        uint8_t count = 0;
        for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
            if (_totemAssignment[s] == req.roleId) count++;
        }
        if (count < req.minCount) return false;
    }
    return true;
}

/* =========================================================
 *   S4a — CONFIG SUBMENU
 * ========================================================= */

void LightAir_GameSetupMenu::renderConfigEntry(uint8_t cursor, uint8_t total) {
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
        _display.print(0, DisplayDefaults::FONT_HEIGHT * row, buf);
    }
    printLegend("^/V:sel <>:val X:", DisplayDefaults::BOTTOM_LINE_Y);
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
    _display.clear();
    _display.setColor(true);

    for (int8_t delta = -1; delta <= 1; delta++) {
        int8_t idx = (int8_t)cursor + delta;
        uint8_t row = (uint8_t)(delta + 1);
        if (idx < 0 || idx >= (PlayerDefs::MAX_PLAYER_ID - 1)) continue;

        uint8_t pid = (uint8_t)(idx + 1);  // player IDs 1–15
        char buf[20];
        snprintf(buf, sizeof(buf), "%s%-3s  T%u",
                 (delta == 0) ? ">" : " ",
                 PlayerDefs::playerShort[pid],
                 _teams[pid]);
        _display.print(0, DisplayDefaults::FONT_HEIGHT * row, buf);
    }
    printLegend("^/V:sel <>:team X:", DisplayDefaults::BOTTOM_LINE_Y);
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
            case '<': {
                uint8_t n = _game->teamCount;
                _teams[pid] = (_teams[pid] == 0) ? n - 1 : _teams[pid] - 1;
                renderTeamEntry(cursor);
                break;
            }
            case '>': {
                uint8_t n = _game->teamCount;
                _teams[pid] = (_teams[pid] + 1) % n;
                renderTeamEntry(cursor);
                break;
            }
            case 'B': return;
        }
    }
}

/* =========================================================
 *   S4c — TOTEMS SUBMENU
 * ========================================================= */

void LightAir_GameSetupMenu::initTotemAssignment() {
    memset(_totemAssignment, 0, sizeof(_totemAssignment));
}

bool LightAir_GameSetupMenu::isRoleAvailable(uint8_t slot, uint8_t roleId) const {
    if (roleId == TotemRoleId::NONE) return true;
    if (!_game->totemRequirements) return false;
    for (uint8_t r = 0; r < _game->totemRequirementCount; r++) {
        const LightAir_TotemRequirement& req = _game->totemRequirements[r];
        if (req.roleId != roleId) continue;
        // Count assignments to this role on other slots.
        uint8_t count = 0;
        for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
            if (s != slot && _totemAssignment[s] == roleId) count++;
        }
        return count < req.maxCount;
    }
    return false;  // roleId not in requirements
}

const char* LightAir_GameSetupMenu::totemRoleLabel(uint8_t roleId) const {
    if (roleId == TotemRoleId::NONE) return "----";
    // Append '*' for required roles (minCount > 0).
    if (_game->totemRequirements) {
        for (uint8_t r = 0; r < _game->totemRequirementCount; r++) {
            if (_game->totemRequirements[r].roleId == roleId &&
                _game->totemRequirements[r].minCount > 0) {
                static char buf[12];
                snprintf(buf, sizeof(buf), "%.9s*", totemRoleName(roleId));
                return buf;
            }
        }
    }
    return totemRoleName(roleId);
}

uint8_t LightAir_GameSetupMenu::nextTotemRole(uint8_t slot, int8_t dir) const {
    // Options: index 0 = NONE, index 1..N = totemRequirements[0..N-1].roleId
    uint8_t reqCount = _game->totemRequirements ? _game->totemRequirementCount : 0;
    uint8_t total    = 1 + reqCount;

    // Find current index in the cycle.
    uint8_t curRole = _totemAssignment[slot];
    uint8_t curIdx  = 0;
    for (uint8_t r = 0; r < reqCount; r++) {
        if (_game->totemRequirements[r].roleId == curRole) { curIdx = r + 1; break; }
    }

    for (uint8_t attempt = 0; attempt < total; attempt++) {
        if (dir > 0) curIdx = (curIdx + 1 >= total) ? 0 : curIdx + 1;
        else         curIdx = (curIdx == 0) ? total - 1 : curIdx - 1;
        uint8_t candidate = (curIdx == 0) ? TotemRoleId::NONE
                                          : _game->totemRequirements[curIdx - 1].roleId;
        if (isRoleAvailable(slot, candidate)) return candidate;
    }
    return _totemAssignment[slot];  // no change possible
}

void LightAir_GameSetupMenu::renderTotemEntry(uint8_t cursor) {
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
        _display.print(0, DisplayDefaults::FONT_HEIGHT * row, buf);
    }
    printLegend("^/V:sel <>:role X:", DisplayDefaults::BOTTOM_LINE_Y);
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

MenuResult LightAir_GameSetupMenu::runPreStart() {
    shareConfig();

    // Reset discovery state; include self from the start.
    _seenCount = 0;
    recordSeen(_radio.playerId());

    uint8_t  vScroll = 0, hScroll = 0;
    uint32_t nextBroadcast = 0;

    renderSummary(vScroll, hScroll);

    while (true) {
        // Broadcast MSG_ROSTER periodically so other devices discover us.
        if (millis() >= nextBroadcast) {
            _radio.broadcast(GameDefaults::MSG_ROSTER, nullptr, 0);
            nextBroadcast = millis() + GameDefaults::PRESTART_BROADCAST_MS;
        }

        // Collect incoming MSG_ROSTER replies; refresh summary when count grows.
        const RadioReport& rep = _radio.poll();
        uint8_t prevCount = _seenCount;
        for (uint8_t i = 0; i < rep.count; i++) {
            const RadioEvent& ev = rep.events[i];
            if (ev.type != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != GameDefaults::MSG_ROSTER) continue;
            recordSeen(ev.packet.senderId);
        }
        if (_seenCount != prevCount)
            renderSummary(vScroll, hScroll);

        // Non-blocking key poll.
        const InputReport& inp = _input.poll();
        for (uint8_t i = 0; i < inp.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = inp.keyEvents[i];
            if (ke.keypadId != _keypadId) continue;
            if (ke.state != KeyState::RELEASED &&
                ke.state != KeyState::RELEASED_HELD) continue;
            switch (ke.key) {
                case 'A':
                    commitToRunner();
                    return MenuResult::Confirmed;
                case 'B':
                    return MenuResult::Cancelled;
                case '^':
                    if (vScroll > 0) { vScroll--; renderSummary(vScroll, hScroll); }
                    break;
                case 'V':
                    vScroll++; renderSummary(vScroll, hScroll);
                    break;
                case '<':
                    if (hScroll > 0) { hScroll--; renderSummary(vScroll, hScroll); }
                    break;
                case '>':
                    hScroll++; renderSummary(vScroll, hScroll);
                    break;
            }
        }

        delay(GameDefaults::LOOP_MS);
    }
}

void LightAir_GameSetupMenu::shareConfig() {
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0,  "Share config?");
    printLegend("O:YES  X:Skip", DisplayDefaults::BOTTOM_LINE_Y);
    _display.flush();

    char key = waitForKey();
    if (key != 'A') return;

    // Generate session token (1–255; 0 is UNSET sentinel, skip it).
    uint8_t token = 0;
    while (token == 0) token = (uint8_t)esp_random();
    _radio.setSessionToken(token);

    uint8_t blob[GameDefaults::RADIO_OUT_PAYLOAD];
    uint16_t len = game_serialize_config(*_game, blob, GameDefaults::RADIO_OUT_PAYLOAD, _totemAssignment, _teams, token);
    if (len > 0) _radio.broadcast(_msgType, blob, len);
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
    _display.clear();
    _display.setColor(true);

    // Row 0: player count (+team breakdown if teamCount > 0)
    {
        uint8_t nPlayers = 0;
        uint8_t counts[TeamColors::kCount] = {};
        for (uint8_t i = 0; i < _seenCount; i++) {
            uint8_t id = _seenIds[i];
            if (TotemDefs::isTotemId(id)) continue;
            nPlayers++;
            if (_game->teamCount > 0) {
                uint8_t t = _teams[id];
                if (t < _game->teamCount) counts[t]++;
            }
        }
        char buf[24];
        if (_game->teamCount > 0) {
            uint8_t off = (uint8_t)snprintf(buf, sizeof(buf), "P:%u(", nPlayers);
            for (uint8_t t = 0; t < _game->teamCount && off + 4 < sizeof(buf); t++)
                off += (uint8_t)snprintf(buf + off, sizeof(buf) - off, "%u:%u ", t, counts[t]);
            if (off > 0 && buf[off - 1] == ' ') buf[off - 1] = ')';
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
        if (v == TotemRoleId::NONE) continue;
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

        // Team annotation based on roleId
        uint8_t team = 0xFF;
        switch (te.val) {
            case TotemRoleId::BASE_O: case TotemRoleId::FLAG_O: team = 0; break;
            case TotemRoleId::BASE_X: case TotemRoleId::FLAG_X: team = 1; break;
        }
        if (team != 0xFF)
            off += snprintf(full + off, sizeof(full) - off, " T%u", team);

        // Horizontal scroll: clip to 18 chars
        constexpr uint8_t ROW_CHARS = 18;
        uint8_t start = (hScroll < strlen(full)) ? hScroll : 0;
        char rowBuf[ROW_CHARS + 2];
        snprintf(rowBuf, sizeof(rowBuf), "%s", full + start);
        _display.print(0, DisplayDefaults::FONT_HEIGHT * (row + 1), rowBuf);
    }

    printLegend("O:Start  X:Back", DisplayDefaults::BOTTOM_LINE_Y);
    _display.flush();
}

void LightAir_GameSetupMenu::commitToRunner() {
    _runner.clearRoster();
    _runner.clearTotems();

    // For teamless games every player uses team=0xFF.
    // For team games, _teams[] was set by the DM (runTeamsSubmenu) or
    // populated from the config blob (runWaiter via game_apply_config).
    if (_game && _game->teamCount == 0) {
        for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++)
            _teams[i] = 0xFF;
    }

    for (uint8_t i = 0; i < PlayerDefs::MAX_PLAYER_ID; i++)
        _runner.setTeam(i, _teams[i]);

    // Set this device's team on the radio so it is stamped into every
    // outgoing packet for the duration of the game.
    _radio.setTeam(_teams[_radio.playerId()]);

    // Add players to roster.
    for (uint8_t i = 0; i < _seenCount; i++) {
        uint8_t id = _seenIds[i];
        if (!TotemDefs::isTotemId(id)) _runner.addToRoster(id);
    }

    // Add configured totems to roster + totem list.
    for (uint8_t s = 0; s < TotemDefs::MAX_TOTEMS; s++) {
        uint8_t roleId = _totemAssignment[s];
        if (roleId == TotemRoleId::NONE) continue;
        uint8_t id = TotemDefs::idFromIndex(s);
        _runner.addToRoster(id);
        _runner.addTotem(id, roleId);
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
    _display.clear();
    _display.setColor(true);
    if (l0 && l0[0]) _display.print(0, 0,                                l0);
    if (l1 && l1[0]) _display.print(0, DisplayDefaults::FONT_HEIGHT,     l1);
    if (l2 && l2[0]) _display.print(0, DisplayDefaults::FONT_HEIGHT * 2, l2);
    if (l3 && l3[0]) _display.print(0, DisplayDefaults::BOTTOM_LINE_Y,   l3);
    _display.flush();
}

void LightAir_GameSetupMenu::printLegend(const char* text, uint8_t y) {
    uint16_t w = _display.textWidth(text);
    uint8_t  x = (w < DisplayDefaults::SCREEN_WIDTH)
                 ? (uint8_t)((DisplayDefaults::SCREEN_WIDTH - w) / 2)
                 : 0;
    _display.print(x, y, text);
}
