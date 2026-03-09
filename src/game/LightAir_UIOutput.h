#pragma once
#include <stdint.h>
#include "../ui/LightAir_UICtrl.h"

// ----------------------------------------------------------------
// UIOutput — outgoing UI event queue for the game loop.
//
// Game rules and behaviors call trigger() / triggerEnlight() during
// the LOGIC phase (step 2).  GameRunner flushes the queue to
// LightAir_UICtrl in the OUTPUT phase (step 3), after all game
// logic for this cycle is complete.
//
// Available events mirror LightAir_UICtrl::UIEvent:
//   Down, Up, Lit, Enlight, EndGame, FlagGain, FlagTaken,
//   FlagReturn, ControlGain, ControlLoss, RoleChange, Stop,
//   Bonus, Malus, Special1, Special2, Custom1..Custom4
//
// Example:
//
//   out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
//   out.ui.triggerEnlight(300);   // 300 ms Enlight burst
// ----------------------------------------------------------------

constexpr uint8_t UI_OUT_MAX = 8;

struct UIOutMsg {
    LightAir_UICtrl::UIEvent event;
    uint16_t                 enlightMs;  // only used when event == Enlight
};

struct UIOutput {
    UIOutMsg msgs[UI_OUT_MAX];
    uint8_t  count = 0;

    void trigger(LightAir_UICtrl::UIEvent event) {
        if (count >= UI_OUT_MAX) return;
        msgs[count++] = { event, 0 };
    }

    // Convenience: queue the Enlight event with a specific burst duration.
    void triggerEnlight(uint16_t ms) {
        if (count >= UI_OUT_MAX) return;
        msgs[count++] = { LightAir_UICtrl::UIEvent::Enlight, ms };
    }
};
