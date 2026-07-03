#pragma once
#include "LightAir_Game.h"
#include "../config.h"

// ----------------------------------------------------------------
// LightAir_GameManager — pure game registry with NVS last-played
// persistence.
//
// Usage:
//
//   // --- sketch globals ---
//   LightAir_GameManager manager;
//   LightAir_GameRunner   runner;
//
//   // --- setup() ---
//   gameStore.begin();                    // mount LittleFS + seed stock games
//   gameStore.registerLuaGames(manager);  // load /games/*.lua
//   LightAir_GameSetupMenu setupMenu(manager, runner, ...);
//   if (setupMenu.run() == MenuResult::Confirmed)
//       runner.begin(setupMenu.selectedGame(), displayCtrl, input, radio);
//
//   // --- loop() ---
//   runner.update();
//
// NVS namespace : "lightair"
// NVS keys      : "last_game" (uint8), "is_dm" (uint8)
// ----------------------------------------------------------------
class LightAir_GameManager {
public:
    // Register a game.  Descriptors must remain valid for the
    // lifetime of the manager.  Returns false if registry is full.
    bool registerGame(const LightAir_Game& game);

    // Direct access by index.
    const LightAir_Game& game(uint8_t idx) const;
    uint8_t              count()           const { return _count; }

    // ---- Lazy loading ----
    // Registered descriptors may be lightweight placeholders (name +
    // typeId only) owned by LightAir_GameStore; the store installs a
    // hook that realizes a placeholder in place — loading the .lua
    // file and filling the full descriptor — the first time the menu
    // actually needs it.  load() must be called before using anything
    // beyond name/typeId of game(idx).  Returns false if the game
    // file fails to load (the menu should refuse the selection).
    typedef bool (*LoadHook)(LightAir_Game& game);
    void setLoadHook(LoadHook hook) { _loadHook = hook; }
    bool load(uint8_t idx) {
        if (idx >= _count) return false;
        if (!_loadHook) return true;                    // nothing is lazy
        return _loadHook(const_cast<LightAir_Game&>(*_games[idx]));
    }

    // NVS persistence helpers (used by LightAir_GameSetupMenu).
    void    saveLastPlayed(uint8_t idx);
    uint8_t loadLastPlayed();

private:
    const LightAir_Game* _games[GameDefaults::MAX_GAMES] = {};
    uint8_t              _count = 0;
    LoadHook             _loadHook = nullptr;
};
