#pragma once
#include "LightAir_Game.h"
#include "LightAir_GameRunner.h"
#include "LightAir_GameManager.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"
#include "../radio/LightAir_Radio.h"
#include "../config.h"

class EnlightCalibRoutine;

// ----------------------------------------------------------------
// Config blob format (used by game_serialize_config / game_apply_config):
//
//   [uint32_t typeId]
//   [int32_t configVar0] … [int32_t configVarN]
//   [int32_t totemVar0.id] … [int32_t totemVarM.id]
//   [int32_t teamBitmask]              ← only if game.hasTeams
//   [int32_t genericRole0] … [int32_t genericRole15]   ← always 16 entries
//
// Receivers call game_apply_config() to update in-place.
// ----------------------------------------------------------------

// Serialize all config data of a game into a byte buffer.
// Returns bytes written, or 0 if maxLen is insufficient.
uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen,
                                const uint8_t genericRoles[TotemDefs::MAX_TOTEMS] = nullptr);

// Apply a received config blob.
// Returns false if typeId doesn't match or blob is too short.
// Values are clamped to [min, max].  Writes to genericRolesOut if non-null.
bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len,
                        uint8_t genericRolesOut[TotemDefs::MAX_TOTEMS] = nullptr);

// ----------------------------------------------------------------
// LightAir_GameSetupMenu — unified DM/player pre-game menu.
//
// Screens:
//   Home  "Welcome to LightAir / Player <name>"  A:Play  B:Settings
//   Sx    Settings menu (Calibration, ID/DM)
//   S1    "Last: <game>  A:Restart  B:New"        [DM only]
//   S2    Scrollable game list (^/V; A=start, B=setup)
//   S4    Setup sub-menu (Config / Teams / Totems)
//   S4a   Config vars (3 visible; </> change, ^/V navigate, B back)
//   S4b   Teams (3 visible; </> toggle O/X, ^/V navigate, B back)
//   S4c   Totems (16 slots; </> cycle role, ^/V navigate, B back)
//   S5    Pre-start: share config → discovery → summary → confirm
//
// Non-DM devices go from Home → A:Play → passive wait for host config.
//
// Usage:
//   LightAir_GameSetupMenu menu(manager, runner, rawDisplay,
//                               input, KEYPAD_ID, radio);
//   if (menu.run() == MenuResult::Confirmed)
//       runner.begin(menu.selectedGame(), displayCtrl, input, radio);
// ----------------------------------------------------------------
class LightAir_GameSetupMenu {
public:
    LightAir_GameSetupMenu(LightAir_GameManager& mgr,
                            LightAir_GameRunner&  runner,
                            LightAir_Display&     display,
                            LightAir_InputCtrl&   input,
                            uint8_t               keypadId,
                            LightAir_Radio&       radio,
                            uint8_t               configMsgType = GameDefaults::MSG_CONFIG);

    // Blocking.  Returns Confirmed or Cancelled.
    MenuResult run();

    // Optional: register a calibration routine accessible from Settings → Calibration.
    // Must be called before run().
    void setCalibRoutine(EnlightCalibRoutine& r) { _calibRoutine = &r; }

    // Valid after Confirmed return.
    const LightAir_Game& selectedGame() const { return *_game; }

private:
    LightAir_GameManager& _mgr;
    LightAir_GameRunner&  _runner;
    LightAir_Display&     _display;
    LightAir_InputCtrl&   _input;
    uint8_t               _keypadId;
    LightAir_Radio&       _radio;
    uint8_t               _msgType;

    EnlightCalibRoutine* _calibRoutine = nullptr;
    bool                 _isDm   = false;
    const LightAir_Game* _game   = nullptr;
    uint8_t              _gameIdx = 0;

    // Team assignments: _teams[id] = 0 (O) or 1 (X)
    uint8_t _teams[PlayerDefs::MAX_PLAYER_ID] = {};

    // Totem slot assignments: _totemAssignment[slot] encodes role as:
    //   value < GenericTotemRoles::COUNT → generic role
    //   value >= GenericTotemRoles::COUNT → totemVar index (value - COUNT)
    uint8_t _totemAssignment[TotemDefs::MAX_TOTEMS] = {};

    // Discovery state
    static constexpr uint8_t MAX_DISC = GameDefaults::MAX_PARTICIPANTS;
    uint8_t _seenIds[MAX_DISC] = {};
    uint8_t _seenCount = 0;

    // ---- Home / Settings ----
    void runSettingsMenu();
    void runIdSettings();
    void saveIsDm(bool val);
    bool loadIsDm();

    // ---- Non-DM waiting path ----
    MenuResult runWaiter();

    // ---- S1 / S2 ----
    bool     runRestartPrompt();            // true → use last game, skip S2–S4
    void     runGameList();                 // sets _game and _gameIdx
    void     renderGameList(uint8_t sel);

    // ---- S4 ----
    void     runSetupMenu();
    bool     validateTotems() const;

    // ---- S4a ----
    void     runConfigSubmenu();
    void     renderConfigEntry(uint8_t cursor, uint8_t total);

    // ---- S4b ----
    void     runTeamsSubmenu();
    void     renderTeamEntry(uint8_t cursor);   // cursor = player ID index 1–15

    // ---- S4c ----
    void     runTotemsSubmenu();
    void     initTotemAssignment();
    uint8_t  nextTotemRole(uint8_t slot, int8_t dir) const;
    const char* totemRoleLabel(uint8_t val) const;   // label including "*" for required
    bool     isRoleAvailable(uint8_t slot, uint8_t val) const;
    void     renderTotemEntry(uint8_t cursor);

    // ---- S5 ----
    MenuResult runPreStart();
    void     shareConfig();
    void     runDiscovery();
    void     recordSeen(uint8_t id);
    bool     wasSeen(uint8_t id) const;
    void     renderSummary(uint8_t vScroll, uint8_t hScroll);
    void     commitToRunner();

    // ---- Shared ----
    char     waitForKey();
    void     showMessage2(const char* line0, const char* line1,
                          const char* line2, const char* line3);
};
