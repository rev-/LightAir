#pragma once
#include "LightAir_Game.h"
#include "LightAir_GameRunner.h"
#include "../ui/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

// ----------------------------------------------------------------
// LightAir_ParticipantMenu — blocking pre-game participant editor.
//
// Runs after the config menu and before runner.begin().  Lets the
// host register which player IDs and totem IDs are taking part,
// then populates the runner's roster and totem list.
//
// Flow:
//   Phase 0 (players):
//     Select player IDs with </>; A adds, B finishes phase.
//     B is blocked until minPlayers IDs have been added.
//
//   Phase 1..totemRoleCount (one per TotemRole):
//     Same UI; A adds the selected ID as a totem of that role.
//     B is blocked until totemRoles[r].minCount IDs have been added.
//
//   On Confirmed:
//     runner.clearRoster() / addToRoster()  — all players then totems.
//     runner.clearTotems() / addTotem()     — totem ID → role index.
//
// Note: ID 0 (NON) is skipped in the selector.  IDs already added
// in any phase are hidden from subsequent phases.
//
// Display layout (4 rows, same style as GameConfigMenu):
//
//   Players phase:
//   ┌──────────────────┐
//   │ Players  2 added │
//   │ < GRN >          │
//   │ A:Add  B:Done    │
//   │ Need 2 min       │   (shown only when min not yet met)
//   └──────────────────┘
//
//   Totem phase (e.g. "Respawn Base"):
//   ┌──────────────────┐
//   │ Respawn Base  1  │
//   │ < YLW >          │
//   │ A:Add  B:Done    │
//   │ Need 2 min       │   (shown only when min not yet met)
//   └──────────────────┘
//
// Usage:
//   LightAir_ParticipantMenu pm(game, runner, rawDisplay, input, keypadId);
//   MenuResult r = pm.run();
//   if (r == MenuResult::Confirmed)
//       runner.begin(game, displayCtrl, input, radio);
// ----------------------------------------------------------------
class LightAir_ParticipantMenu {
public:
    LightAir_ParticipantMenu(const LightAir_Game&  game,
                              LightAir_GameRunner&  runner,
                              LightAir_Display&     display,
                              LightAir_InputCtrl&   input,
                              uint8_t               keypadId);

    // Blocking.  Returns Confirmed once all requirements are met and
    // the user confirms; returns Cancelled if the user exits early (B
    // in the player phase before any participant has been added).
    MenuResult run();

private:
    const LightAir_Game& _game;
    LightAir_GameRunner& _runner;
    LightAir_Display&    _display;
    LightAir_InputCtrl&  _input;
    uint8_t              _keypadId;

    // Local lists built during the menu; committed to runner on Confirmed.
    static constexpr uint8_t MAX_P = GameDefaults::MAX_PARTICIPANTS;
    uint8_t    _players[MAX_P];
    uint8_t    _playerCount = 0;
    struct TotemEntry { uint8_t id; uint8_t roleIdx; };
    TotemEntry _totems[MAX_P];
    uint8_t    _totemCount = 0;

    // UI helpers
    void renderPlayerPhase(uint8_t selectedId, bool needMore);
    void renderTotemPhase(uint8_t roleIdx, uint8_t count,
                          uint8_t selectedId, bool needMore);
    void renderError(const char* line1, const char* line2 = nullptr);

    // Input helper — blocks until a key is RELEASED or RELEASED_HELD.
    char waitForKey();

    // ID navigation: advance id forward (+1) or backward (-1), skipping
    // already-added IDs and ID 0 (NON).  Wraps within [1, MAX_PLAYER_ID-1].
    uint8_t nextAvailableId(uint8_t id, bool forward) const;

    // True if id is already registered in _players or _totems.
    bool isUsed(uint8_t id) const;

    // Commit local lists into the runner.
    void commitToRunner();
};
