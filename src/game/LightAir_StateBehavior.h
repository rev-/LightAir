#pragma once
#include <stdint.h>
#include "../input/LightAir_InputTypes.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "LightAir_GameOutput.h"

// ----------------------------------------------------------------
// StateBehavior — per-state loop body, called every cycle while
// the game is in `state`.
//
// onUpdate runs AFTER transition rules have been evaluated and
// the display binding set has been switched.  It always sees the
// state the game settled into during this cycle.
//
// Use onUpdate to:
//   - React to trigger buttons (e.g. call Enlight::run())
//   - Handle radio messages (e.g. process incoming hits)
//   - Queue radio messages    via out.radio
//   - Queue UI events         via out.ui
//   - Update display tray     via disp.showMessage(...)
//   - Modify game variables   (lives, ammo, score...)
//
// nullptr = no per-cycle action for this state.
//
// Example:
//
//   extern Enlight enlight;   // file-scope global, no capture needed
//
//   static void doInGame(const InputReport& inp,
//                        const RadioReport&  rad,
//                        LightAir_DisplayCtrl& disp,
//                        GameOutput& out) {
//       // Fire on TRIG_1 release
//       for (uint8_t i = 0; i < inp.buttonCount; i++) {
//           if (inp.buttons[i].id    == InputDefaults::TRIG_1_ID &&
//               inp.buttons[i].state == ButtonState::RELEASED) {
//               enlight.run();
//               out.radio.broadcast(MSG_SHOOT);
//               out.ui.triggerEnlight(300);
//           }
//       }
//       // Process incoming hits
//       for (uint8_t i = 0; i < rad.count; i++) {
//           if (rad.events[i].type == RadioEventType::MessageReceived &&
//               rad.events[i].packet.msgType == MSG_HIT) {
//               lives--;
//               out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
//           }
//       }
//   }
//
//   static const StateBehavior behaviors[] = {
//       { S_ALIVE, doInGame },
//       { S_DEAD,  nullptr  },
//   };
// ----------------------------------------------------------------
struct StateBehavior {
    uint8_t  state;

    void   (*onUpdate)(const InputReport&, const RadioReport&,
                       LightAir_DisplayCtrl&, GameOutput&);
};
