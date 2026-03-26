#pragma once
#include "LightAir_TotemUIOutput.h"
#include "rgb/LightAir_TotemRGB.h"
#include "strip/LightAir_LEDStrip.h"

// ----------------------------------------------------------------
// LightAir_TotemUICtrl — drives the totem's RGB LED and LED strip
// from a TotemUIOutput queue.
//
// Dependencies are injected at construction time (matching the
// pattern used by LightAir_UICtrl on the player side).
//
// Each TotemUIEvent maps to:
//   strip  — a StripAnimation (one-shot or looping)
//   rgb    — an RGB colour (on/off)
//
// Looping background states (Idle, FlagMissing, Control,
// ControlContest) call strip.loop() and set a persistent RGB colour.
// One-shot events (Respawn, FlagTaken, etc.) call strip.play() which
// pre-empts the background for its duration, then the background resumes.
//
// Usage:
//   LightAir_TotemRGB_HW  rgb;
//   LightAir_LEDStrip_HW  strip;
//   LightAir_TotemUICtrl  ui(rgb, strip);
//   // in setup():
//   rgb.begin(pinComm, pinR, pinG, pinB);
//   strip.begin(dataPin, numLeds);
//   ui.begin();
//   // in each loop iteration after game logic:
//   ui.apply(out.ui);
//   ui.update();
// ----------------------------------------------------------------
class LightAir_TotemUICtrl {
public:
    LightAir_TotemUICtrl(LightAir_TotemRGB& rgb, LightAir_LEDStrip& strip);

    // Start the idle background animation.  Call after hardware begin().
    void begin();

    // Process all queued commands for this loop iteration.
    void apply(const TotemUIOutput& output);

    // Advance strip animation state.  Call every loop tick.
    void update();

private:
    LightAir_TotemRGB&  _rgb;
    LightAir_LEDStrip&  _strip;

    // Dispatch helpers
    void dispatchOneShot(const TotemUICmd& cmd);
    void dispatchBackground(const TotemUICmd& cmd);
    bool isBackground(TotemUIEvent ev) const;
};
