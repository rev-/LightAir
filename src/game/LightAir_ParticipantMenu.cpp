#include "LightAir_ParticipantMenu.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

/* =========================================================
 *   CONSTRUCTOR
 * ========================================================= */

LightAir_ParticipantMenu::LightAir_ParticipantMenu(const LightAir_Game&  game,
                                                    LightAir_GameRunner&  runner,
                                                    LightAir_Display&     display,
                                                    LightAir_InputCtrl&   input,
                                                    uint8_t               keypadId)
    : _game(game), _runner(runner), _display(display),
      _input(input), _keypadId(keypadId)
{}

/* =========================================================
 *   PUBLIC API
 * ========================================================= */

MenuResult LightAir_ParticipantMenu::run() {
    _playerCount = 0;
    _totemCount  = 0;

    // Start selector on ID 1 (skip NON = 0).
    uint8_t selId = 1;

    // ---- Phase 0: players ----
    renderPlayerPhase(selId, false);

    while (true) {
        char key = waitForKey();

        switch (key) {
            case '<':
                selId = nextAvailableId(selId, false);
                renderPlayerPhase(selId, false);
                break;

            case '>':
                selId = nextAvailableId(selId, true);
                renderPlayerPhase(selId, false);
                break;

            case 'A': {
                // Add the selected ID as a player.
                if (!isUsed(selId) && _playerCount < MAX_P) {
                    _players[_playerCount++] = selId;
                    // Advance selector to next available ID.
                    selId = nextAvailableId(selId, true);
                }
                renderPlayerPhase(selId, false);
                break;
            }

            case 'B': {
                bool minMet = (_playerCount >= _game.minPlayers);
                if (!minMet) {
                    // Show requirement error briefly, then return to phase.
                    char line[24];
                    snprintf(line, sizeof(line), "Need %u min", _game.minPlayers);
                    renderError("Not enough players", line);
                    delay(1500);
                    renderPlayerPhase(selId, false);
                } else {
                    // Advance to totem phases (or finish if no roles).
                    goto totem_phases;
                }
                break;
            }
        }
    }

totem_phases:
    // ---- Phases 1..totemRoleCount: one phase per TotemRole ----
    for (uint8_t ri = 0; ri < _game.totemRoleCount; ri++) {
        const TotemRole& role    = _game.totemRoles[ri];
        uint8_t          roleAdded = 0;   // totems added in this phase

        // Reset selector to first available ID for this phase.
        selId = nextAvailableId(0, true);  // finds first ID ≥ 1 not yet used

        renderTotemPhase(ri, roleAdded, selId, false);

        while (true) {
            char key = waitForKey();

            switch (key) {
                case '<':
                    selId = nextAvailableId(selId, false);
                    renderTotemPhase(ri, roleAdded, selId, false);
                    break;

                case '>':
                    selId = nextAvailableId(selId, true);
                    renderTotemPhase(ri, roleAdded, selId, false);
                    break;

                case 'A': {
                    if (!isUsed(selId) && _totemCount < MAX_P) {
                        _totems[_totemCount++] = { selId, ri };
                        roleAdded++;
                        selId = nextAvailableId(selId, true);
                    }
                    renderTotemPhase(ri, roleAdded, selId, false);
                    break;
                }

                case 'B': {
                    bool minMet = (roleAdded >= role.minCount);
                    if (!minMet) {
                        char line[24];
                        snprintf(line, sizeof(line), "Need %u min", role.minCount);
                        renderError("Not enough totems", line);
                        delay(1500);
                        renderTotemPhase(ri, roleAdded, selId, false);
                    } else {
                        goto next_role;
                    }
                    break;
                }
            }
        }
next_role:;
    }

    // ---- All phases done ----
    commitToRunner();
    return MenuResult::Confirmed;
}

/* =========================================================
 *   RENDERING
 * ========================================================= */

void LightAir_ParticipantMenu::renderPlayerPhase(uint8_t selectedId, bool needMore) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    char buf[24];

    _display.clear();
    _display.setColor(true);

    snprintf(buf, sizeof(buf), "Players  %u added", _playerCount);
    _display.print(0, 0, buf);

    snprintf(buf, sizeof(buf), "< %s >",
             PlayerDefs::playerShort[selectedId < PlayerDefs::MAX_PLAYER_ID
                                     ? selectedId : 0]);
    _display.print(0, fh, buf);

    _display.print(0, fh * 2, "A:Add  B:Done");

    if (needMore || (_game.minPlayers > 0 && _playerCount < _game.minPlayers)) {
        snprintf(buf, sizeof(buf), "Need %u min", _game.minPlayers);
        _display.print(0, fh * 3, buf);
    }

    _display.flush();
}

void LightAir_ParticipantMenu::renderTotemPhase(uint8_t roleIdx, uint8_t count,
                                                 uint8_t selectedId, bool needMore) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    char buf[24];

    _display.clear();
    _display.setColor(true);

    const TotemRole& role = _game.totemRoles[roleIdx];
    snprintf(buf, sizeof(buf), "%.14s %u", role.name, count);
    _display.print(0, 0, buf);

    snprintf(buf, sizeof(buf), "< %s >",
             PlayerDefs::playerShort[selectedId < PlayerDefs::MAX_PLAYER_ID
                                     ? selectedId : 0]);
    _display.print(0, fh, buf);

    _display.print(0, fh * 2, "A:Add  B:Done");

    if (needMore || (role.minCount > 0 && count < role.minCount)) {
        snprintf(buf, sizeof(buf), "Need %u min", role.minCount);
        _display.print(0, fh * 3, buf);
    }

    _display.flush();
}

void LightAir_ParticipantMenu::renderError(const char* line1, const char* line2) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    _display.clear();
    _display.setColor(true);
    _display.print(0, 0, line1);
    if (line2) _display.print(0, fh, line2);
    _display.flush();
}

/* =========================================================
 *   INPUT
 * ========================================================= */

char LightAir_ParticipantMenu::waitForKey() {
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

/* =========================================================
 *   ID NAVIGATION
 * ========================================================= */

uint8_t LightAir_ParticipantMenu::nextAvailableId(uint8_t id, bool forward) const {
    // Try up to MAX_PLAYER_ID-1 steps to find a free ID in [1, MAX_PLAYER_ID-1].
    for (uint8_t attempt = 0; attempt < PlayerDefs::MAX_PLAYER_ID; attempt++) {
        if (forward) {
            id = (id >= PlayerDefs::MAX_PLAYER_ID - 1) ? 1 : id + 1;
        } else {
            id = (id <= 1) ? PlayerDefs::MAX_PLAYER_ID - 1 : id - 1;
        }
        if (!isUsed(id)) return id;
    }
    return id;  // all IDs used (unlikely with MAX_PLAYER_ID = 16 and ≤15 participants)
}

bool LightAir_ParticipantMenu::isUsed(uint8_t id) const {
    for (uint8_t i = 0; i < _playerCount; i++)
        if (_players[i] == id) return true;
    for (uint8_t i = 0; i < _totemCount; i++)
        if (_totems[i].id == id) return true;
    return false;
}

/* =========================================================
 *   COMMIT
 * ========================================================= */

void LightAir_ParticipantMenu::commitToRunner() {
    _runner.clearRoster();
    _runner.clearTotems();

    // Players go into the roster first.
    for (uint8_t i = 0; i < _playerCount; i++)
        _runner.addToRoster(_players[i]);

    // Totems go into the roster (they may also score) and the totem list.
    for (uint8_t i = 0; i < _totemCount; i++) {
        _runner.addToRoster(_totems[i].id);
        _runner.addTotem(_totems[i].id, _totems[i].roleIdx);
    }
}
