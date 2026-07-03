#include "LightAir_GameStore.h"

LightAir_GameStore* LightAir_GameStore::s_instance = nullptr;

#ifdef ESP32

#include <Arduino.h>
#include <ArduinoLog.h>
#include <FS.h>
#include <LittleFS.h>
#include "LightAir_LuaGame.h"
#include "LightAir_GamesBundle.h"

// One shared fully-loaded game (the selected one) + one scratch
// instance for manifest scanning.  ~50 games cost 50 small manifests,
// not 50 Lua interpreters.
static LightAir_LuaGame s_loadedGame;
static LightAir_LuaGame s_scanner;

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
 *   MANIFEST SCAN + LAZY REALIZATION
 * ========================================================= */

uint8_t LightAir_GameStore::registerLuaGames(LightAir_GameManager& mgr) {
    if (!_mounted) return 0;
    s_instance = this;
    _count = 0;

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
        if (_count >= GameDefaults::MAX_GAMES) {
            Log.errorln("GameStore: manifest table full (%d)", GameDefaults::MAX_GAMES);
            break;
        }

        Manifest& m = _manifests[_count];
        snprintf(m.path, sizeof(m.path), "%s/%s", LuaDefaults::GAMES_DIR, name);
        f.close();

        if (!s_scanner.peekManifest(m.path, m.name, sizeof(m.name), &m.typeId)) {
            Log.errorln("GameStore: skipping %s (bad manifest)", m.path);
            continue;
        }

        // Duplicate typeIds: first file wins (deterministic scan order).
        bool dup = false;
        for (uint8_t i = 0; i < _count; i++)
            if (_manifests[i].typeId == m.typeId) dup = true;
        if (dup) {
            Log.errorln("GameStore: %s duplicates typeId 0x%x", m.path, m.typeId);
            continue;
        }

        // Lightweight placeholder: enough for the menu list; everything
        // else is filled by realize() on selection.
        LightAir_Game& ph = _placeholders[_count];
        memset(&ph, 0, sizeof(ph));
        ph.typeId       = m.typeId;
        ph.name         = m.name;
        ph.scoringState = 255;

        if (!mgr.registerGame(ph)) {
            Log.errorln("GameStore: registry rejected %s", m.path);
            continue;
        }
        _count++;
    }

    mgr.setLoadHook(&LightAir_GameStore::realizeHook);
    Log.infoln("GameStore: %d game manifest(s) registered", _count);
    return _count;
}

bool LightAir_GameStore::realizeHook(LightAir_Game& game) {
    return s_instance ? s_instance->realize(game) : false;
}

bool LightAir_GameStore::realize(LightAir_Game& game) {
    // Find the manifest owning this placeholder (also accepts a
    // re-realize of an already-filled descriptor).
    const Manifest* m = nullptr;
    for (uint8_t i = 0; i < _count; i++)
        if (_manifests[i].typeId == game.typeId) { m = &_manifests[i]; break; }
    if (!m) return true;                    // not one of ours (native etc.)

    if (!s_loadedGame.loaded() || s_loadedGame.typeId() != m->typeId) {
        Log.infoln("GameStore: loading %s", m->path);
        if (!s_loadedGame.load(m->path)) return false;
    }
    // Copy the full descriptor over the placeholder in place: every
    // pointer inside it targets the shared instance, so the menu's
    // and runner's references into the registry stay valid.
    game = s_loadedGame.descriptor();
    // ...except the name: the menu lists ALL games' names without
    // loading them, and the shared instance's name buffer changes on
    // every reload.  The manifest copy is stable for the store's life.
    game.name = m->name;
    return true;
}

#else  // !ESP32 — the store is device-only; keep the TU compilable on host.

bool LightAir_GameStore::begin() { return false; }
uint8_t LightAir_GameStore::registerLuaGames(LightAir_GameManager&) { return 0; }
void LightAir_GameStore::seedDefaults() {}
bool LightAir_GameStore::realize(LightAir_Game&) { return false; }
bool LightAir_GameStore::realizeHook(LightAir_Game&) { return false; }

#endif
