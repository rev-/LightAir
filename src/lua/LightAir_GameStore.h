#pragma once
#include <stdint.h>
#include "../game/LightAir_GameManager.h"

// ----------------------------------------------------------------
// LightAir_GameStore — LittleFS-backed store of .lua game files,
// built to scale to dozens of games.
//
//   begin()            mounts LittleFS (formatting a blank partition)
//                      and seeds the embedded stock games into
//                      /games whenever the on-flash copy differs
//                      byte-for-byte from the one compiled into the
//                      firmware — a freshly flashed device is
//                      playable with no upload step, and a firmware
//                      update refreshes its stock games.  Stock
//                      filenames are firmware-owned; custom games
//                      live under their own filenames.
//
//   registerLuaGames() scans /games/*.lua and registers a lightweight
//                      MANIFEST per file (name + typeId, ~80 bytes)
//                      with the GameManager, plus a load hook.  The
//                      full game — Lua state, variable slots, totem
//                      programs — is realized on ONE shared instance
//                      only when the menu actually selects it
//                      (GameManager::load()), so 50 games on flash
//                      cost 50 manifests in RAM, not 50 interpreters.
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
    bool realize(LightAir_Game& game);
    static bool realizeHook(LightAir_Game& game);   // -> singleton
    static LightAir_GameStore* s_instance;
};
