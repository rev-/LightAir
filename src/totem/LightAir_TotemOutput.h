#pragma once
#include "../game/LightAir_RadioOutput.h"
#include "../ui/totem/LightAir_TotemUIOutput.h"

// ----------------------------------------------------------------
// LightAir_TotemOutput — unified output bundle for TotemRunner
// callbacks.  Mirrors GameOutput but uses TotemUIOutput instead
// of UIOutput (no LCD, no audio, no vibration).
//
// Both queues are flushed by LightAir_TotemDriver in the OUTPUT
// phase after all runner logic has run.
//
// Usage inside a TotemRunner::onMessage():
//
//   void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) {
//       // Queue an animation
//       out.ui.trigger(TotemUIEvent::Respawn, playerR, playerG, playerB);
//       // Queue a radio reply
//       out.radio.reply(msg);
//   }
// ----------------------------------------------------------------
struct LightAir_TotemOutput {
    RadioOutput    radio;
    TotemUIOutput  ui;
};
