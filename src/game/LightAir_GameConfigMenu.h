#pragma once
#include "LightAir_Game.h"
#include "LightAir_GameRunner.h"
#include "../ui/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"
#include "../radio/LightAir_Radio.h"

// ----------------------------------------------------------------
// Config blob format (used by serializeConfig / applyConfig):
//
//   [uint32_t typeId][int32_t var0][int32_t var1]...[int32_t varN]
//
//   All configVars are serialized in declaration order.
//   typeId lets receivers verify the blob belongs to the right game
//   before applying it.
// ----------------------------------------------------------------

// Serialize all configVars of a game into a byte buffer.
// Returns bytes written, or 0 if maxLen < 4.
uint16_t game_serialize_config(const LightAir_Game& game,
                                uint8_t* buf, uint16_t maxLen);

// Apply a received config blob.
// Returns false if typeId doesn't match or blob is too short.
// Values are clamped to [min, max].
bool game_apply_config(const LightAir_Game& game,
                        const uint8_t* buf, uint16_t len);

// ----------------------------------------------------------------
// LightAir_GameConfigMenu — blocking pre-game setup: config editing,
// config sharing, and automatic roster discovery.
//
// Run this once in setup() between selectGame() and runner.begin().
// No additional participant-list call is needed in the sketch.
//
// Phase 1 — Config editor:
//   Shows each configVar one at a time.  Adjust with </>, navigate
//   with ^/V, confirm with A (moves to Phase 2).  B cancels.
//
// Phase 2 — Config share:
//   Prompts to broadcast the config blob to all devices via radio.
//
// Phase 3 — Roster discovery (automatic, no user action needed):
//   Every device broadcasts MSG_ROSTER with its NVS role in payload[0].
//   This device listens for ROSTER_WINDOW_MS, accumulating all
//   senderIds + roles it hears.  Own presence is included immediately.
//   Presence re-broadcast repeats every ROSTER_RETRY_MS.
//
//   Role convention (stored in PlayerConfig::role via NVS):
//     0        = shooter / player
//     1..N     = totem (role value - 1 = TotemRole index in game.totemRoles[])
//
// Phase 4 — Roster summary:
//   ┌──────────────────┐
//   │ Players: 3       │
//   │ BASES: 1,2       │   (one line per totem role, up to 2 roles)
//   │ FLAGS: 3         │
//   │ A:Start  B:Redo  │
//   └──────────────────┘
//   A commits the roster and returns Confirmed.
//   B repeats discovery (Phase 3 → 4).
//
// Receiving side (other devices handling MSG_CONFIG):
//   for (uint8_t i = 0; i < rad.count; i++) {
//       auto& ev = rad.events[i];
//       if (ev.type == RadioEventType::MessageReceived &&
//           ev.packet.msgType == GameDefaults::MSG_CONFIG)
//           game_apply_config(myGame, ev.packet.payload, ev.packet.payloadLen);
//   }
//
// Usage:
//   LightAir_GameConfigMenu menu(game, runner, rawDisplay, input,
//                                InputDefaults::KEYPAD_ID, radio);
//   if (menu.run() == MenuResult::Confirmed)
//       runner.begin(game, displayCtrl, input, radio);
// ----------------------------------------------------------------

class LightAir_GameConfigMenu {
public:
    LightAir_GameConfigMenu(const LightAir_Game&  game,
                             LightAir_GameRunner&  runner,
                             LightAir_Display&     display,
                             LightAir_InputCtrl&   input,
                             uint8_t               keypadId,
                             LightAir_Radio&       radio,
                             uint8_t               msgType = GameDefaults::MSG_CONFIG);

    // Blocking.  Returns Confirmed or Cancelled.
    MenuResult run();

private:
    const LightAir_Game& _game;
    LightAir_GameRunner& _runner;
    LightAir_Display&    _display;
    LightAir_InputCtrl&  _input;
    uint8_t              _keypadId;
    LightAir_Radio&      _radio;
    uint8_t              _msgType;

    // Number of config vars shown in the menu (capped at MAX_BINDINGS).
    uint8_t _configCount = 0;

    // Roster data collected during discovery (Phase 3).
    static constexpr uint8_t MAX_DISC = PlayerDefs::MAX_PLAYER_ID;
    uint8_t _players[MAX_DISC];        // discovered shooter IDs
    uint8_t _playerCount = 0;
    struct DiscTotem { uint8_t id; uint8_t roleIdx; };
    DiscTotem _totems[MAX_DISC];       // discovered totem IDs + role indices
    uint8_t   _totemCount = 0;

    // --- Phase 1 ---
    void renderMenu(uint8_t varIdx);

    // --- Phase 2 ---
    bool promptShare();
    void shareConfig();

    // --- Phase 3 ---
    void runDiscovery();                          // blocking ROSTER_WINDOW_MS window
    void recordParticipant(uint8_t id, uint8_t role);

    // --- Phase 4 ---
    void renderSummary();
    void commitToRunner();

    // --- Shared ---
    char waitForKey();   // blocks until RELEASED/RELEASED_HELD from _keypadId
};
