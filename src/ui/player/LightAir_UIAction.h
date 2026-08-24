#ifndef LIGHTAIR_UIACTION_H
#define LIGHTAIR_UIACTION_H

#include <stdint.h>

// ----------------------------------------------------------------
// LightAir_UIAction — one multi-sensory feedback pattern.
//
// Up to 4 steps; each step drives the buzzer, the vibration motor and
// the RGB LED for durations[i] ms.  priority orders the UICtrl queue
// (0 = never plays, 1 = coalescing, 5 = interrupts everything).
//
// Split out of LightAir_UICtrl so value types that merely *carry* an
// action — a Projector's shot feedback, for instance — can include this
// header alone instead of pulling in Arduino.h, Ticker.h and the whole
// UI controller.  LightAir_UICtrl::UIAction remains a valid spelling.
// ----------------------------------------------------------------
struct LightAir_UIAction {
    uint16_t durations[4];
    uint8_t  stepCount;
    uint16_t soundFreqs[4];
    uint8_t  vibIntensity[4];
    uint8_t  rgbColors[4][3];
    uint8_t  priority;
};

#endif // LIGHTAIR_UIACTION_H
