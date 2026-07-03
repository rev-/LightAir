#include "LightAir_GameStore.h"

#ifdef ESP32

#include <Arduino.h>
#include <ArduinoLog.h>
#include <FS.h>
#include <LittleFS.h>
#include "LightAir_LuaGame.h"
#include "LightAir_GamesBundle.h"

/* =========================================================
 *   MOUNT + SEED
 * ========================================================= */

bool LightAir_GameStore::begin() {
    if (_mounted) return true;
    if (!LittleFS.begin(true /* format on first mount */)) {
        Log.errorln("GameStore: LittleFS mount failed");
        return false;
    }
    _mounted = true;
    LittleFS.mkdir(LuaDefaults::GAMES_DIR);
    LittleFS.mkdir(LuaDefaults::LIB_DIR);
    seedDefaults();
    return true;
}

void LightAir_GameStore::seedDefaults() {
    for (const EmbeddedGameFile& ef : kEmbeddedGames) {
        bool write = true;
        File existing = LittleFS.open(ef.path, "r");
        if (existing) {
            // Cheap freshness check: same size = same shipped version.
            // User-edited files of a different size are overwritten by the
            // stock copy only if the size differs — uploading customised
            // games under a new filename is the supported path.
            write = ((size_t)existing.size() != ef.len);
            existing.close();
        }
        if (!write) continue;
        File f = LittleFS.open(ef.path, "w");
        if (!f) {
            Log.errorln("GameStore: cannot write %s", ef.path);
            continue;
        }
        size_t n = f.write(ef.data, ef.len);
        f.close();
        if (n != ef.len) Log.errorln("GameStore: short write on %s", ef.path);
        else             Log.infoln("GameStore: seeded %s (%d bytes)", ef.path, (int)ef.len);
    }
}

/* =========================================================
 *   SCAN + REGISTER
 * ========================================================= */

uint8_t LightAir_GameStore::registerLuaGames(LightAir_GameManager& mgr) {
    if (!_mounted) return 0;

    uint8_t registered = 0;
    File dir = LittleFS.open(LuaDefaults::GAMES_DIR);
    if (!dir || !dir.isDirectory()) {
        Log.errorln("GameStore: %s missing", LuaDefaults::GAMES_DIR);
        return 0;
    }

    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;                 // skips /games/lib
        const char* name = f.name();
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".lua") != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), "%s/%s", LuaDefaults::GAMES_DIR, name);
        f.close();

        // Instances registered with the manager must outlive it; games are
        // loaded once per boot, so the allocation is deliberately permanent.
        LightAir_LuaGame* game = new LightAir_LuaGame();
        if (!game->load(path)) {
            delete game;
            continue;
        }
        if (!mgr.registerGame(game->descriptor())) {
            Log.errorln("GameStore: '%s' not registered (typeId clash or registry full)",
                        game->name());
            // Keep the instance: its trampoline slot is claimed; freeing it
            // would dangle s_instances[]. One skipped game wastes ~50 KB of
            // PSRAM until reboot — acceptable for a misconfigured file set.
            continue;
        }
        registered++;
    }
    Log.infoln("GameStore: %d Lua game(s) registered", registered);
    return registered;
}

#else  // !ESP32 — the store is device-only; keep the TU compilable on host.

bool LightAir_GameStore::begin() { return false; }
uint8_t LightAir_GameStore::registerLuaGames(LightAir_GameManager&) { return 0; }
void LightAir_GameStore::seedDefaults() {}

#endif
