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
//   registerAllGames(manager);         // defined in rulesets/AllGames.cpp
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

    // NVS persistence helpers (used by LightAir_GameSetupMenu).
    void    saveLastPlayed(uint8_t idx);
    uint8_t loadLastPlayed();

private:
    const LightAir_Game* _games[GameDefaults::MAX_GAMES] = {};
    uint8_t              _count = 0;
};
