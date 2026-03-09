#pragma once
#include <stdint.h>
#include "../input/LightAir_InputTypes.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/display/LightAir_DisplayCtrl.h"
#include "LightAir_GameOutput.h"

// ----------------------------------------------------------------
// StateRule — one row in a game's state-transition table.
//
// GameRunner evaluates rules in order for the current state.
// The first rule whose condition returns true fires:
//   1. currentState is set to toState.
//   2. GameRunner activates the new state's display binding set.
//   3. onTransition is called (if non-null).
// Only one rule fires per loop cycle.
//
// condition    : nullptr means the rule fires unconditionally.
//
// onTransition : called after the display binding set is switched.
//                Queue radio messages via out.radio and UI events
//                via out.ui.  nullptr = no extra action.
//
// Note: callbacks are plain function pointers (no captures).
// Stateless lambdas decay to function pointers automatically.
//
// Example:
//
//   static bool hitReceived(const InputReport&, const RadioReport& r) {
//       for (uint8_t i = 0; i < r.count; i++)
//           if (r.events[i].type == RadioEventType::MessageReceived &&
//               r.events[i].packet.msgType == MSG_HIT) return true;
//       return false;
//   }
//
//   static void onKilled(LightAir_DisplayCtrl& disp, GameOutput& out) {
//       disp.showMessage("Eliminated!", 2000);
//       out.radio.broadcast(MSG_KILLED);
//       out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
//   }
//
//   static const StateRule rules[] = {
//       { S_ALIVE, hitReceived, S_DEAD,  onKilled },
//       { S_DEAD,  nullptr,     S_ALIVE, nullptr  },
//   };
// ----------------------------------------------------------------
struct StateRule {
    uint8_t  fromState;

    bool   (*condition)(const InputReport&, const RadioReport&);

    uint8_t  toState;

    // Called after display is switched; nullptr = skip.
    void   (*onTransition)(LightAir_DisplayCtrl&, GameOutput&);
};
