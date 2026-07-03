#pragma once
#include <stdint.h>
#include "../game/LightAir_GameManager.h"

// ----------------------------------------------------------------
// LightAir_GameStore — LittleFS-backed store of .lua game files.
//
//   begin()            mounts LittleFS (formatting a blank partition)
//                      and seeds the embedded stock games into
//                      /games when missing or size-changed, so a
//                      freshly flashed device is playable without
//                      the HTTP upload path.
//
//   registerLuaGames() scans /games/*.lua, loads each file into a
//                      heap-allocated LightAir_LuaGame and registers
//                      its descriptor with the GameManager.  Files
//                      that fail validation are logged and skipped;
//                      typeId collisions defer to whichever game
//                      registered first.
//
// Registered instances live for the firmware's lifetime (the
// descriptor is referenced by the manager), so they are deliberately
// never freed.
// ----------------------------------------------------------------
class LightAir_GameStore {
public:
    // Mount the filesystem and seed stock games.  Returns false if
    // LittleFS cannot be mounted (Lua games unavailable).
    bool begin();

    // Load every /games/*.lua and register with the manager.
    // Returns the number of games successfully registered.
    uint8_t registerLuaGames(LightAir_GameManager& mgr);

private:
    bool _mounted = false;
    void seedDefaults();
};
