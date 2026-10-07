#pragma once
#include "LightAir_RadioOutput.h"
#include "LightAir_UIOutput.h"

// ----------------------------------------------------------------
// OpticsOutput — the optical settings the active projector wants, queued
// for the OUTPUT phase.
//
// Reconfiguring Enlight while a measurement is in flight corrupts it, and
// game logic runs at an arbitrary point inside the LOGIC phase, so the
// change is applied in phase 3 like every other effect.  A game that
// switches projector and fires in the same tick therefore fires with the
// optics it had — which is what a profile's ready_ms models anyway.
//
// Note the asymmetry with la.shine(), which is a DIRECT call: starting a
// measurement must happen on this tick, because its return value decides
// whether energy is spent.  Configuration is deferred; firing is not.
// ----------------------------------------------------------------
struct OpticsOutput {
    bool     hasCycles   = false;
    bool     hasCooldown = false;
    uint16_t cycles      = 0;
    uint16_t cooldownMs  = 0;

    void setCycles(uint16_t v)   { cycles     = v; hasCycles   = true; }
    void setCooldown(uint16_t v) { cooldownMs = v; hasCooldown = true; }
};

// ----------------------------------------------------------------
// GameOutput — unified output bundle passed to all game callbacks.
//
// Bundles RadioOutput (queued outgoing radio messages), UIOutput
// (queued UI events) and OpticsOutput (the active projector's Enlight
// settings) into one argument so callback signatures stay concise and
// remain extensible.
//
// All three are flushed by GameRunner in step 3 (OUTPUT phase)
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
    RadioOutput  radio;
    UIOutput     ui;
    OpticsOutput optics;
};
