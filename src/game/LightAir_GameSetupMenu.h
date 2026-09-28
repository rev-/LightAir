#pragma once
#include "LightAir_Game.h"
#include "LightAir_GameRunner.h"
#include "LightAir_GameManager.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"
#include "../radio/LightAir_Radio.h"
#include "../config.h"
#include "LightAir_ConfigBlob.h"

class EnlightCalibRoutine;
class EnlightTestMode;
class GameFileServer;

// ----------------------------------------------------------------
// KeyEvent — returned by waitForKey() with both key and state info
// ----------------------------------------------------------------
struct MenuKeyEvent {
    char key;
    KeyState state;
};

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
//   S4b   Teams (3 visible; </> cycle team 0..N-1, ^/V navigate, B back)
//   S4c   Totems (16 slots; </> cycle role, O cycle option, ^/V navigate, B back)
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

    // Optional: register a calibration tool accessible from Settings → Calibration.
    // Must be called before run().
    void setCalibTool(EnlightCalibRoutine& t) { _calibTool = &t; }

    // Optional: register a test mode tool accessible from Settings → Test Mode.
    // Must be called before run().
    void setTestTool(EnlightTestMode& t) { _testTool = &t; }

    // Optional: register the game share server (Settings → Share games):
    // WiFi AP + web page to download the .lua games from this device and
    // upload new ones to it.  Must be called before run().
    void setShareTool(GameFileServer& t) { _shareTool = &t; }

    // Valid after Confirmed return (which guarantees a selected game).
    const LightAir_Game& selectedGame() const { return *_game; }

private:
    LightAir_GameManager& _mgr;
    LightAir_GameRunner&  _runner;
    LightAir_Display&     _display;
    LightAir_InputCtrl&   _input;
    uint8_t               _keypadId;
    LightAir_Radio&       _radio;
    uint8_t               _msgType;

    EnlightCalibRoutine* _calibTool = nullptr;
    EnlightTestMode*     _testTool  = nullptr;
    GameFileServer*      _shareTool = nullptr;
    bool                 _isDm   = false;
    const LightAir_Game* _game   = nullptr;
    uint8_t              _gameIdx = 0;

    // Team assignments: _teams[id] = team index 0..teamCount-1
    // (0 = O, 1 = X in two-team games); 0xFF = unassigned/teamless.
    uint8_t _teams[PlayerDefs::MAX_PLAYER_ID] = {};

    // Totem slot assignments: _totemAssignment[slot] = TotemRoleId constant.
    // 0 = TotemRoleId::NONE (unassigned).
    uint8_t _totemAssignment[TotemDefs::MAX_TOTEMS] = {};
    // Per-slot option picked with O (1-based index into the role's
    // declared options; 0 = none).  BASE/FLAG teams are not stored here:
    // O on those rewrites _totemAssignment (BASE_O ↔ BASE_X ↔ BASE).
    uint8_t _totemOption[TotemDefs::MAX_TOTEMS] = {};

    // Discovery state
    static constexpr uint8_t MAX_DISC = GameDefaults::MAX_PARTICIPANTS;
    uint8_t _seenIds[MAX_DISC] = {};
    uint8_t _seenCount = 0;

    // Pre-start countdown (seconds); set at entry of runPreStart, read by renderSummary.
    uint8_t _countdownSecs = GameDefaults::COUNTDOWN_DEFAULT_S;

    // ---- Home / Settings ----
    void runHomeScreen();    // blocks until O:Play; O:Settings handled inline
    void runSettingsMenu();
    void runIdSettings();
    void runShareTool();     // Settings → Share games (reboots on exit)
    void saveIsDm(bool val);
    bool loadIsDm();
    // Config-blob checkpoint (same wire format as the radio broadcast),
    // used to make S1 "Restart last game" restore actual values instead
    // of the file's defaults.  See saveLastConfig()'s doc comment.
    void saveLastConfig();
    void applyLastConfig(const LightAir_Game& game);

    // ---- Non-DM waiting path ----
    MenuResult runWaiter();

    // ---- S1 / S2 ----
    bool     runRestartPrompt();            // true → use last game, skip S2–S4
    // Game picker.  true → _game / _gameIdx now hold the selection; false →
    // nothing was selected (no games installed) and _game is still null, so
    // the caller must go back Home instead of entering S4/S5.
    bool     runGameList();
    void     renderGameList(uint8_t sel);

    // ---- S4 ----
    bool     runSetupMenu();
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
    const char* totemRoleLabel(uint8_t roleId) const;   // label including "*" for required
    const char* totemEntryLabel(uint8_t roleId) const;  // BASE/FLAG folded, "*" for required
    const char* totemOptionLabel(uint8_t slot) const;   // "" when the slot has no option
    bool     isRoleAvailable(uint8_t slot, uint8_t roleId) const;
    bool     isRoleDeclared(uint8_t roleId) const;
    const LightAir_TotemRequirement* requirementFor(uint8_t roleId) const;
    uint8_t  firstAvailableInEntry(uint8_t slot, uint8_t roleId) const;
    bool     cycleTotemOption(uint8_t slot);             // false = role has no options
    void     setTotemRole(uint8_t slot, uint8_t roleId);
    void     renderTotemEntry(uint8_t cursor, const char* legend = nullptr);

    // ---- S5 ----
    MenuResult runPreStart();
    void     recordSeen(uint8_t id);
    bool     wasSeen(uint8_t id) const;
    void     renderSummary(uint8_t vScroll);
    void     runCountdownSequence(uint8_t secs);
    void     commitToRunner();

    // ---- Shared ----
    // Non-blocking single-pass key/button check. key==0 means "no event this tick".
    MenuKeyEvent pollKeyEvent();
    MenuKeyEvent waitForKey();
    void     resetKeyStates();  // Reset prevState to reflect current input reality
    void     showMessage2(const char* line0, const char* line1,
                          const char* line2, const char* line3);
    // The refusal screen for a game the loader would not accept, carrying
    // the loader's own reason rather than a bare "failed to load".
    void     showLoadFailure();
    // Print text centered horizontally at pixel row y.
    void     printLegend(const char* text, uint8_t y);
};
