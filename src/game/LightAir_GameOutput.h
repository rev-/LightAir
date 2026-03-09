#pragma once
#include "LightAir_RadioOutput.h"
#include "LightAir_UIOutput.h"

// ----------------------------------------------------------------
// GameOutput — unified output bundle passed to all game callbacks.
//
// Bundles RadioOutput (queued outgoing radio messages) and
// UIOutput (queued UI events) into one argument so callback
// signatures stay concise and remain extensible.
//
// Both queues are flushed by GameRunner in step 3 (OUTPUT phase)
// after all rules and behaviors have run.
//
// Usage in a StateBehavior::onUpdate:
//
//   static void doInGame(const InputReport& inp,
//                        const RadioReport&  rad,
//                        LightAir_DisplayCtrl& disp,
//                        GameOutput& out) {
//       for (uint8_t i = 0; i < inp.buttonCount; i++) {
//           if (inp.buttons[i].id    == InputDefaults::TRIG_1_ID &&
//               inp.buttons[i].state == ButtonState::RELEASED) {
//               enlight.run();
//               out.radio.broadcast(MSG_SHOOT);
//               out.ui.triggerEnlight(300);
//           }
//       }
//   }
//
// Usage in a StateRule::onTransition:
//
//   static void onKilled(LightAir_DisplayCtrl& disp, GameOutput& out) {
//       disp.showMessage("Eliminated!", 2000);
//       out.radio.broadcast(MSG_KILLED);
//       out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
//   }
// ----------------------------------------------------------------
struct GameOutput {
    RadioOutput radio;
    UIOutput    ui;
};
