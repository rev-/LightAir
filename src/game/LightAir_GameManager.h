#pragma once
#include "LightAir_Game.h"
#include "../ui/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

// ----------------------------------------------------------------
// LightAir_GameManager — game registry, selection menu, and NVS
// last-played persistence.
//
// Usage:
//
//   // --- sketch globals ---
//   LightAir_GameManager manager;
//   LightAir_GameRunner   runner;
//
//   // --- setup() ---
//   registerAllGames(manager);         // defined in rulesets/AllGames.cpp
//   const LightAir_Game& sel =
//       manager.selectGame(rawDisplay, input, InputDefaults::KEYPAD_ID);
//   runner.begin(sel, displayCtrl, input, radio);
//
//   // --- loop() ---
//   runner.update();
//
// Selection menu layout (scrollable list):
//
//   ┌──────────────────┐
//   │ Select game      │   row 0
//   │ 1 / 3            │   row 1   (index / total)
//   │ Free for All     │   row 2   (game name)
//   │ A:OK  ^/V:scroll │   row 3
//   └──────────────────┘
//
// ^/V scroll through registered games.
// A confirms and saves the choice to NVS.
// B wraps back to first game (no cancel — a game must be selected).
//
// NVS namespace : "lightair"
// NVS key       : "last_game"
// ----------------------------------------------------------------
class LightAir_GameManager {
public:
    // Register a game.  Descriptors must remain valid for the
    // lifetime of the manager.  Returns false if registry is full.
    bool registerGame(const LightAir_Game& game);

    // Blocking selection menu.  Returns the selected game by reference.
    // Saves the choice index to NVS for next power-on.
    const LightAir_Game& selectGame(LightAir_Display&   display,
                                     LightAir_InputCtrl& input,
                                     uint8_t             keypadId);

    // Direct access by index.
    const LightAir_Game& game(uint8_t idx) const;
    uint8_t              count()           const { return _count; }

private:
    const LightAir_Game* _games[GameDefaults::MAX_GAMES] = {};
    uint8_t              _count = 0;

    void    renderMenu(LightAir_Display& disp, uint8_t sel);
    void    saveLastPlayed(uint8_t idx);
    uint8_t loadLastPlayed();
};
