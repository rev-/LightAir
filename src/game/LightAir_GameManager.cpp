#include "LightAir_GameManager.h"
#include "../config.h"
#include <nvs_flash.h>
#include <nvs.h>

#define MGR_NVS_NAMESPACE  "lightair"
#define MGR_NVS_KEY_LAST   "last_game"

/* =========================================================
 *   REGISTRY
 * ========================================================= */

bool LightAir_GameManager::registerGame(const LightAir_Game& game) {
    if (_count >= GameDefaults::MAX_GAMES) return false;
    for (uint8_t i = 0; i < _count; i++) {
        if (_games[i]->typeId == game.typeId) return false;  // duplicate typeId
    }
    _games[_count++] = &game;
    return true;
}

const LightAir_Game& LightAir_GameManager::game(uint8_t idx) const {
    return *_games[idx];
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
    uint8_t val = 0xFF;  // 0xFF = no saved game
    if (nvs_open(MGR_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0xFF;
    nvs_get_u8(h, MGR_NVS_KEY_LAST, &val);
    nvs_close(h);
    return val;
}
