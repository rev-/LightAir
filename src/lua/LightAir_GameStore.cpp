#include "LightAir_GameStore.h"

LightAir_GameStore* LightAir_GameStore::s_instance = nullptr;

#ifdef ESP32

#include <Arduino.h>
#include <ArduinoLog.h>
#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include "LightAir_LuaGame.h"
#include "LightAir_GamesBundle.h"

// Where seedDefaults() remembers what it last wrote (see the note there).
static const char* kStoreNvsNamespace = "lightair";
static const char* kSeedHashKey       = "seed_hash";

// ONE LightAir_LuaGame for both jobs.  A full menu costs a table of small
// manifests, not a table of Lua interpreters.
//
// The boot scan used to have an instance of its own, and it was 8 KB of
// waste: peekManifest() builds a fresh lua_State, runs the file's top level
// with la.lib() inert, reads three literal fields and closes the state again
// — it never touches the descriptor arrays (_slots, _progs, _configVars,
// _monitorVars, _rules, the registry refs) that are almost all of an
// instance's size, and it leaves the instance unloaded.  The scan also runs
// before any game is realized, so this one is idle at the time.  On a board
// with no PSRAM that second instance was 8 KB of internal RAM held for the
// life of the device to do nothing.
static LightAir_LuaGame s_loadedGame;

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

// FNV-1a over a byte range, and over a file.  Used only to answer "has
// anyone touched this since we wrote it" — not a security property.
static uint32_t hashBytes(const unsigned char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
}

// 0 when the file does not exist or cannot be read (no content hashes to 0
// in practice, and an unreadable file should be reseeded anyway).
static uint32_t hashFile(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    uint32_t h = 2166136261u;
    uint8_t  buf[256];
    for (;;) {
        size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
    }
    f.close();
    return h;
}

// ----------------------------------------------------------------
// Seeding, and who owns a stock game file.
//
// A ruleset is a FILE.  Editing it — over the Settings → Share games
// upload, or straight on the filesystem — has to be all it takes to change
// the game, or the whole point of shipping rulesets as files is lost and
// every tweak means a rebuild.  So seeding must never overwrite an edit.
//
// It must also still refresh the stock games on a firmware update, or a
// device would be stuck for ever on whatever it first booted with.
//
// Both, by remembering what we last wrote.  For each embedded file we keep
// the hash of the bytes we seeded, in NVS:
//
//   file missing              -> write it, remember the hash
//   on-flash hash == ours     -> untouched since we wrote it, so a changed
//                                bundle may refresh it
//   on-flash hash != ours     -> somebody edited it.  Leave it alone, and
//                                say so once on the log.
//
// An edited stock game therefore survives firmware updates, and keeps its
// own version of the ruleset rather than silently getting ours back.  To
// return one to stock, delete it and reboot.
// ----------------------------------------------------------------
void LightAir_GameStore::seedDefaults() {
    constexpr uint8_t kMax = sizeof(kEmbeddedGames) / sizeof(*kEmbeddedGames);
    uint32_t seeded[kMax] = {};
    size_t   blobLen = sizeof(seeded);

    nvs_handle_t nvs = 0;
    const bool haveNvs = (nvs_open(kStoreNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK);
    if (haveNvs && nvs_get_blob(nvs, kSeedHashKey, seeded, &blobLen) != ESP_OK)
        blobLen = 0;                       // first boot, or the table grew
    const bool haveHashes = (blobLen == sizeof(seeded));
    if (!haveHashes) memset(seeded, 0, sizeof(seeded));

    bool dirty = false;
    uint8_t i = 0;
    for (const EmbeddedGameFile& ef : kEmbeddedGames) {
        const uint32_t shipped = hashBytes(ef.data, ef.len);
        const uint32_t onFlash = hashFile(ef.path);

        if (onFlash == shipped) {          // already exactly this version
            if (seeded[i] != shipped) { seeded[i] = shipped; dirty = true; }
            i++;
            continue;
        }
        // Present, and not what we last wrote: the player's copy wins.
        if (onFlash != 0 && haveHashes && seeded[i] != 0 && onFlash != seeded[i]) {
            Log.infoln("GameStore: keeping edited %s (delete it to restore stock)",
                       ef.path);
            i++;
            continue;
        }

        File f = LittleFS.open(ef.path, "w");
        if (!f) {
            Log.errorln("GameStore: cannot write %s", ef.path);
            i++;
            continue;
        }
        size_t n = f.write(ef.data, ef.len);
        f.close();
        if (n != ef.len) {
            Log.errorln("GameStore: short write on %s", ef.path);
        } else {
            Log.infoln("GameStore: seeded %s (%d bytes)", ef.path, (int)ef.len);
            seeded[i] = shipped;
            dirty = true;
        }
        i++;
    }

    if (haveNvs) {
        if (dirty || !haveHashes) {
            nvs_set_blob(nvs, kSeedHashKey, seeded, sizeof(seeded));
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
}

/* =========================================================
 *   MANIFEST SCAN + LAZY REALIZATION
 * ========================================================= */

static void logHeadroom(const char* what, const char* path) {
    Log.infoln("GameStore: %s %s (psram %d B, heap %d B, largest block %d B)",
               what, path,
               (int)ESP.getFreePsram(), (int)ESP.getFreeHeap(),
               (int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

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

        if (!s_loadedGame.peekManifest(m.path, m.name, sizeof(m.name), &m.typeId)) {
            Log.errorln("GameStore: skipping %s (bad manifest)", m.path);
            // A scan that drops files one by one has emptied the menu once
            // already; say what the device had left when it dropped this one.
            logHeadroom("after failing", m.path);
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

bool LightAir_GameStore::realizeHook(LightAir_Game& game,
                                     char* errOut, size_t errCap) {
    if (s_instance) return s_instance->realize(game, errOut, errCap);
    if (errOut && errCap) snprintf(errOut, errCap, "no game store");
    return false;
}

bool LightAir_GameStore::realize(LightAir_Game& game,
                                 char* errOut, size_t errCap) {
    // Find the manifest owning this placeholder (also accepts a
    // re-realize of an already-filled descriptor).
    const Manifest* m = nullptr;
    for (uint8_t i = 0; i < _count; i++)
        if (_manifests[i].typeId == game.typeId) { m = &_manifests[i]; break; }
    if (!m) return true;                    // not one of ours (native etc.)

    if (!s_loadedGame.loaded() || s_loadedGame.typeId() != m->typeId) {
        // Log the headroom either side of the load.  A ruleset that loads
        // on the bench and refuses on the device is a memory question
        // first, and three numbers answer it without a second flash:
        // whether PSRAM is there at all (the Lua allocator asks for it
        // first and falls back silently), how much internal RAM is left,
        // and the largest single block in it — which is what a state built
        // from thousands of small allocations actually runs out of.
        logHeadroom("loading", m->path);
        if (!s_loadedGame.load(m->path)) {
            if (errOut && errCap) snprintf(errOut, errCap, "%s", s_loadedGame.loadError());
            return false;
        }
        logHeadroom("loaded ", m->path);
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
bool LightAir_GameStore::realize(LightAir_Game&, char*, size_t) { return false; }
bool LightAir_GameStore::realizeHook(LightAir_Game&, char*, size_t) { return false; }

#endif
