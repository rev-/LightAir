#pragma once
#include "LightAir_TotemUIOutput.h"
#include "rgb/LightAir_TotemRGB.h"
#include "strip/LightAir_LEDStrip.h"

// ----------------------------------------------------------------
// LightAir_TotemUICtrl — drives the totem's RGB LED and LED strip
// from a TotemUIOutput queue.
//
// Hardware owned:
//   • LightAir_TotemRGB  — simple 4-GPIO RGB LED (colour indicator)
//   • LightAir_LEDStrip  — WS2812B addressable strip (animations)
//
// Each TotemUIEvent maps to:
//   strip  — a StripAnimation (one-shot or looping)
//   rgb    — an RGB colour (on/off)
//
// Looping background states (Idle, FlagMissing, ControlO/X,
// ControlContest) call strip.loop() and set a persistent RGB colour.
// One-shot events (Respawn, FlagTaken, etc.) call strip.play() which
// pre-empts the background for its duration, then the background resumes.
//
// Usage:
//   LightAir_TotemUICtrl ui;
//   ui.begin(pinComm, pinR, pinG, pinB, dataPin, numLeds);
//   // in each loop iteration after game logic:
//   ui.apply(out.ui);
//   ui.update();
// ----------------------------------------------------------------
class LightAir_TotemUICtrl {
public:
    void begin(int pinComm, int pinR, int pinG, int pinB,
               int dataPin, uint8_t numLeds);

    // Process all queued commands for this loop iteration.
    void apply(const TotemUIOutput& output);

    // Advance strip animation state.  Call every loop tick.
    void update();

private:
    LightAir_TotemRGB _rgb;
    LightAir_LEDStrip _strip;

    // Dispatch helpers
    void dispatchOneShot(const TotemUICmd& cmd);
    void dispatchBackground(const TotemUICmd& cmd);
    bool isBackground(TotemUIEvent ev) const;
};
