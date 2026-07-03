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

// True when the file at path exists and its content equals data[0..len).
static bool fileMatches(const char* path, const unsigned char* data, size_t len) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    if ((size_t)f.size() != len) { f.close(); return false; }
    uint8_t buf[256];
    size_t  off = 0;
    while (off < len) {
        size_t chunk = (len - off < sizeof(buf)) ? (len - off) : sizeof(buf);
        if (f.read(buf, chunk) != chunk) { f.close(); return false; }
        if (memcmp(buf, data + off, chunk) != 0) { f.close(); return false; }
        off += chunk;
    }
    f.close();
    return true;
}

void LightAir_GameStore::seedDefaults() {
    for (const EmbeddedGameFile& ef : kEmbeddedGames) {
        // Content-exact check: stock filenames are firmware-owned — any
        // difference (new firmware version OR a local edit) restores the
        // shipped copy.  Customised games belong under a new filename,
        // which seeding never touches.
        if (fileMatches(ef.path, ef.data, ef.len)) continue;
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
