#pragma once
#include "../config.h"
#include "LightAir_Game.h"
#include "LightAir_GameOutput.h"
#include "../input/LightAir_InputCtrl.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "../ui/player/LightAir_UICtrl.h"

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
// Roster and totem management
//   The roster is the ordered list of participant IDs (players and
//   totems) expected to take part in end-game score collection.
//   Call clearRoster()/addToRoster() and clearTotems()/addTotem()
//   from LightAir_ParticipantMenu (or directly) before begin():
//
//     runner.clearRoster();
//     runner.clearTotems();
//     runner.addToRoster(playerId_1);      // shooter player
//     runner.addToRoster(playerId_2);
//     runner.addToRoster(totemId_1);       // totem (also scores)
//     runner.addTotem(totemId_1, roleIdx); // record its role
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
    // Call clearRoster() + addToRoster() before begin() to register all
    // participants (players and totems) expected in end-game score collection.
    void clearRoster();
    void addToRoster(uint8_t id);  // ignores duplicates; caps at MAX_PARTICIPANTS

    // ---- Totem management ----
    // Call clearTotems() + addTotem() before begin() to record totem roles.
    // roleIdx is an index into game.totemRoles[].
    void clearTotems();
    void addTotem(uint8_t id, uint8_t roleIdx);  // ignores duplicates; caps at MAX_PARTICIPANTS

    // Read-back accessors (for game logic or post-game queries).
    uint8_t rosterCount()          const { return _rosterCount; }
    uint8_t rosterId(uint8_t i)    const { return _roster[i]; }
    uint8_t totemCount()           const { return _totemCount; }
    uint8_t totemId(uint8_t i)     const { return _totems[i].id; }
    uint8_t totemRole(uint8_t i)   const { return _totems[i].roleIdx; }

    // ---- Team map ----
    // Call setTeam() from LightAir_GameSetupMenu::commitToRunner() before begin().
    // teamOf() returns 0=O or 1=X; 0 if id is out of range.
    void    setTeam(uint8_t id, uint8_t team);
    uint8_t teamOf(uint8_t id)  const;

    // ---- Generic totem roles ----
    // slot = TotemDefs::totemIndex(id) (0 = totem01 = ID 254).
    // Call setGenericTotemRole() from commitToRunner() before begin().
    void    setGenericTotemRole(uint8_t slot, uint8_t role);
    uint8_t genericTotemRole(uint8_t slot) const;

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

    // ---- Roster (players + totems for score collection) ----
    uint8_t _roster[GameDefaults::MAX_PARTICIPANTS];
    uint8_t _rosterCount = 0;

    // ---- Totem entries (id → role index) ----
    struct TotemEntry { uint8_t id; uint8_t roleIdx; };
    TotemEntry _totems[GameDefaults::MAX_PARTICIPANTS];
    uint8_t    _totemCount = 0;

    // ---- Team assignments ----
    uint8_t _teamMap[PlayerDefs::MAX_PLAYER_ID]  = {};  // 0=O, 1=X per player ID

    // ---- Generic totem roles (by TotemDefs slot index) ----
    uint8_t _genericRoles[TotemDefs::MAX_TOTEMS] = {};  // GenericTotemRoles value per slot

    // ---- End-game score accumulation ----
    bool     _scoreActive      = false;   // true while in scoringState; prevents re-trigger
    bool     _scoreResultShown = false;   // winner display already triggered
    bool     _endExitReady     = false;   // true after scoreAnnounce; A+B triggers reboot
    uint8_t  _emptyBindingSetId = 255;    // binding set with no vars; activated after scoreAnnounce
    uint32_t _scoreAccumMask   = 0;       // bit r set = _scoreSlots[r] is valid
    uint32_t _scoreSentAt      = 0;       // millis() of last broadcast; 0 = not yet sent
    uint8_t  _scoreSlots[GameDefaults::MAX_PARTICIPANTS][GameDefaults::MAX_WINNER_VARS * 4];

    // ---- Helpers ----
    void activateStateDisplay(uint8_t state);
    void flushOutput(const GameOutput& out);

    // Score collection helpers (all defined in .cpp)
    void postScoreAnnounce();
    void scoreFillSlot(uint8_t* buf) const;
    bool scoreSlotBeats(const uint8_t* a, const uint8_t* b) const;
    bool scoreSlotsEqual(const uint8_t* a, const uint8_t* b) const;
    void scoreAnnounce() const;
    void scoreBroadcastFused(GameOutput& output) const;
};
