#pragma once
#include "LightAir_Game.h"
#include "LightAir_GameOutput.h"
#include "../input/LightAir_InputCtrl.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/display/LightAir_DisplayCtrl.h"
#include "../ui/LightAir_UICtrl.h"

// ----------------------------------------------------------------
// LightAir_GameRunner — runtime driver for one LightAir_Game.
//
// Owns the three-phase game loop:
//   1. READ   — poll InputCtrl + LightAir_Radio
//   2. LOGIC  — evaluate transition rules, run state behavior;
//               callbacks write to a GameOutput buffer
//   3. OUTPUT — displayCtrl.update(), flush queued radio messages,
//               flush queued UI events to LightAir_UICtrl
//
// Each update() enforces a fixed duration (GameDefaults::LOOP_MS).
//
// Display binding sets are created automatically in begin() from
// GameVar::stateMask — no DisplayCtrl code needed in game files.
//
// LightAir_UICtrl is optional.  If not provided, UI events queued
// in GameOutput::ui are silently discarded.
//
// Roster management
//   The roster is the ordered list of player IDs expected to
//   participate in end-game score collection.  It is filled by the
//   pre-start condition before begin() is called:
//
//     runner.clearRoster();
//     runner.addToRoster(playerId_1);
//     runner.addToRoster(playerId_2);
//     ...
//
// Usage:
//
//   LightAir_GameRunner runner;
//
//   // setup():
//   runner.begin(myGame, displayCtrl, inputCtrl, radio);
//   // or with UI:
//   runner.begin(myGame, displayCtrl, inputCtrl, radio, &uiCtrl);
//
//   // loop():
//   runner.update();
// ----------------------------------------------------------------
class LightAir_GameRunner {
public:

    // One-time setup.  Creates display binding sets from MonitorVar::stateMask,
    // resets state to initialState, calls game.onBegin (if set).
    // ui is optional: pass nullptr (default) to disable UI event dispatch.
    void begin(const LightAir_Game& game,
               LightAir_DisplayCtrl& display,
               LightAir_InputCtrl&   input,
               LightAir_Radio&       radio,
               LightAir_UICtrl*      ui = nullptr);

    // One loop iteration: read → logic → output.
    // Delays for the remainder of GameDefaults::LOOP_MS if logic finishes early.
    void update();

    // ---- Roster management ----
    // Call clearRoster() + addToRoster() from the pre-start condition to register
    // the players expected to participate in end-game score collection.
    void clearRoster();
    void addToRoster(uint8_t playerId);  // ignores duplicates; caps at MAX_PLAYER_ID

private:
    const LightAir_Game*  _game    = nullptr;
    LightAir_DisplayCtrl* _display = nullptr;
    LightAir_InputCtrl*   _input   = nullptr;
    LightAir_Radio*       _radio   = nullptr;
    LightAir_UICtrl*      _ui      = nullptr;  // optional

    // State → display binding-set mapping
    struct StateBinding { uint8_t state; uint8_t setId; };
    StateBinding _bindings[DisplayDefaults::MAX_SETS];
    uint8_t      _bindingCount = 0;

    // ---- Roster ----
    uint8_t _roster[PlayerDefs::MAX_PLAYER_ID];
    uint8_t _rosterCount = 0;

    // ---- End-game score accumulation ----
    bool     _scoreActive      = false;   // true while in scoringState; prevents re-trigger
    bool     _scoreResultShown = false;   // winner display already triggered
    uint32_t _scoreAccumMask   = 0;       // bit r set = _scoreSlots[r] is valid
    uint32_t _scoreSentAt      = 0;       // millis() of last broadcast; 0 = not yet sent
    uint8_t  _scoreSlots[PlayerDefs::MAX_PLAYER_ID][GameDefaults::MAX_WINNER_VARS * 4];

    // ---- Helpers ----
    void activateStateDisplay(uint8_t state);
    void flushOutput(const GameOutput& out);

    // Score collection helpers (all defined in .cpp)
    void scoreFillSlot(uint8_t* buf) const;
    bool scoreSlotBeats(const uint8_t* a, const uint8_t* b) const;
    bool scoreSlotsEqual(const uint8_t* a, const uint8_t* b) const;
    void scoreAnnounce() const;
    void scoreBroadcastFused(GameOutput& output) const;
};
