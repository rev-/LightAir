#pragma once
#include <stdint.h>
#include "../game/LightAir_GameManager.h"

// ----------------------------------------------------------------
// LightAir_GameStore — LittleFS-backed store of .lua game files,
// built so the menu scales with the flash, not with RAM.
//
//   begin()            mounts LittleFS (formatting a blank partition)
//                      and seeds the embedded stock games into /games,
//                      so a freshly flashed device is playable with no
//                      upload step.  A ruleset is a FILE and editing it
//                      is all it takes to change the game: seeding
//                      remembers the hash of what it wrote, refreshes a
//                      stock file only while it still matches, and
//                      leaves an edited one alone for ever after.
//                      Delete a file to get the stock version back.
//
//   registerLuaGames() scans /games/*.lua and registers a lightweight
//                      MANIFEST per file (name + typeId, ~80 bytes)
//                      with the GameManager, plus a load hook.  The
//                      full game — Lua state, variable slots, totem
//                      programs — is realized on ONE shared instance
//                      only when the menu actually selects it
//                      (GameManager::load()), so a full menu costs a
//                      table of manifests, not a table of interpreters.
//
// Realized descriptors are copied over their placeholder in place, so
// menu/runner pointers into the registry stay stable across the lazy
// load.  Files that fail even the manifest scan are skipped (logged).
// ----------------------------------------------------------------
class LightAir_GameStore {
public:
    // Mount the filesystem and seed stock games.  Returns false if
    // LittleFS cannot be mounted (Lua games unavailable).
    bool begin();

    // Scan /games/*.lua, register manifests + the realize hook.
    // Returns the number of games registered.
    uint8_t registerLuaGames(LightAir_GameManager& mgr);

private:
    struct Manifest {
        char     name[16];
        char     path[48];
        uint16_t typeId;
    };

    bool     _mounted = false;
    Manifest _manifests[GameDefaults::MAX_GAMES];
    // Placeholder descriptors the manager points at; realize() fills
    // them in place from the shared loaded instance.
    LightAir_Game _placeholders[GameDefaults::MAX_GAMES];
    uint8_t  _count = 0;

    void seedDefaults();
    bool realize(LightAir_Game& game, char* errOut, size_t errCap);
    static bool realizeHook(LightAir_Game& game, char* errOut, size_t errCap);
    static LightAir_GameStore* s_instance;
};
