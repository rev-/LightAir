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

    // One-time setup.  Creates display binding sets from GameVar::stateMask,
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

    // Round-robin state: true while in roundRobinState, prevents re-trigger.
    bool _rrActive = false;

    void activateStateDisplay(uint8_t state);
    void flushOutput(const GameOutput& out);
};
