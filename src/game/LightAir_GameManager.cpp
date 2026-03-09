#include "LightAir_GameManager.h"
#include "../config.h"
#include <Arduino.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <stdio.h>

#define MGR_NVS_NAMESPACE  "lightair"
#define MGR_NVS_KEY_LAST   "last_game"

/* =========================================================
 *   REGISTRY
 * ========================================================= */

bool LightAir_GameManager::registerGame(const LightAir_Game& game) {
    if (_count >= GameDefaults::MAX_GAMES) return false;
    _games[_count++] = &game;
    return true;
}

const LightAir_Game& LightAir_GameManager::game(uint8_t idx) const {
    return *_games[idx];
}

/* =========================================================
 *   SELECTION MENU
 * ========================================================= */

void LightAir_GameManager::renderMenu(LightAir_Display& disp, uint8_t sel) {
    const uint8_t fh = DisplayDefaults::FONT_HEIGHT;
    char buf[24];

    disp.clear();
    disp.setColor(true);

    disp.print(0, 0,      "Select game");
    snprintf(buf, sizeof(buf), "%u / %u", sel + 1, _count);
    disp.print(0, fh,     buf);
    disp.print(0, fh * 2, _games[sel]->name);
    disp.print(0, fh * 3, "A:OK  ^/V:scroll");

    disp.flush();
}

const LightAir_Game& LightAir_GameManager::selectGame(LightAir_Display&   display,
                                                        LightAir_InputCtrl& input,
                                                        uint8_t             keypadId) {
    if (_count == 0) {
        // Nothing registered — return game 0 (undefined, but avoids crash).
        return *_games[0];
    }

    uint8_t sel = loadLastPlayed();
    if (sel >= _count) sel = 0;

    renderMenu(display, sel);

    while (true) {
        const InputReport& rep = input.poll();

        for (uint8_t i = 0; i < rep.keyEventCount; i++) {
            const InputReport::KeyEntry& ke = rep.keyEvents[i];
            if (ke.keypadId != keypadId) continue;
            if (ke.state != KeyState::RELEASED &&
                ke.state != KeyState::RELEASED_HELD) continue;

            switch (ke.key) {
                case '^':
                    sel = (sel > 0) ? sel - 1 : _count - 1;
                    renderMenu(display, sel);
                    break;
                case 'V':
                    sel = (sel < _count - 1) ? sel + 1 : 0;
                    renderMenu(display, sel);
                    break;
                case 'A':
                    saveLastPlayed(sel);
                    return *_games[sel];
            }
        }

        delay(GameDefaults::LOOP_MS);
    }
}

/* =========================================================
 *   NVS PERSISTENCE
 * ========================================================= */

void LightAir_GameManager::saveLastPlayed(uint8_t idx) {
    nvs_handle_t h;
    if (nvs_open(MGR_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, MGR_NVS_KEY_LAST, idx);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t LightAir_GameManager::loadLastPlayed() {
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(MGR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, MGR_NVS_KEY_LAST, &val);
    nvs_close(h);
    return val;
}
