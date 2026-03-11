#pragma once
#include "LightAir_Game.h"
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
// LightAir_GameConfigMenu — blocking pre-game config editor.
//
// Shows all configVars one at a time.  The player adjusts
// values with the keypad, then either confirms (A) or cancels (B).
// On confirmation the menu optionally broadcasts the config blob
// to all other players via radio.
//
// Receiving side:
//   Other devices call game_apply_config() when they receive a
//   packet with msgType == msgType.  The applyConfig function
//   verifies the typeId header before writing any values.
//
// Menu layout (4 rows):
//
//   ┌──────────────────┐
//   │ Config  1 / 2    │   header: var index / total config vars
//   │ Lives            │   variable name
//   │ <    3    >      │   current value with adjust arrows
//   │ A:OK  B:Back     │   controls
//   └──────────────────┘
//
// Prompt after A (share option):
//
//   ┌──────────────────┐
//   │ Share config?    │
//   │ A:YES   B:Skip   │
//   └──────────────────┘
//
// Usage:
//
//   enum Result { Confirmed, Cancelled };
//
//   // Before starting the game:
//   LightAir_GameConfigMenu menu(game, rawDisplay, input,
//                                InputDefaults::KEYPAD_ID,
//                                radio, GameDefaults::MSG_CONFIG);
//   auto result = menu.run();
//
//   // On receiving devices (inside a StateBehavior or loop):
//   for (uint8_t i = 0; i < rad.count; i++) {
//       auto& ev = rad.events[i];
//       if (ev.type == RadioEventType::MessageReceived &&
//           ev.packet.msgType == GameDefaults::MSG_CONFIG)
//           game_apply_config(myGame, ev.packet.payload, ev.packet.payloadLen);
//   }
// ----------------------------------------------------------------

class LightAir_GameConfigMenu {
public:
    LightAir_GameConfigMenu(const LightAir_Game& game,
                             LightAir_Display&   display,
                             LightAir_InputCtrl& input,
                             uint8_t             keypadId,
                             LightAir_Radio&     radio,
                             uint8_t             msgType = GameDefaults::MSG_CONFIG);

    // Blocking.  Returns Confirmed or Cancelled.
    MenuResult run();

private:
    const LightAir_Game& _game;
    LightAir_Display&    _display;
    LightAir_InputCtrl&  _input;
    uint8_t              _keypadId;
    LightAir_Radio&      _radio;
    uint8_t              _msgType;

    // Number of config vars shown in the menu (capped at MAX_BINDINGS).
    uint8_t _configCount = 0;

    void renderMenu(uint8_t varIdx);
    bool promptShare();
    void shareConfig();
    char waitForKey();   // blocks until RELEASED/RELEASED_HELD from _keypadId
};
