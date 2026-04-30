#pragma once
#include "Enlight.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../input/LightAir_InputCtrl.h"

// ---------------------------------------------------------------
// EnlightTestMode — test mode for verifying projector capability
// without starting a game.
//
// Features:
//   1. TRIG_1 press/hold calls Enlight(repetitions)
//   2. When a player color is recognized, plays LIT event and
//      displays the color name (e.g., "LIT RED")
//   3. < and > keys adjust repetitions (1-100, default 5)
//      - PRESSED: increment/decrement by 1
//      - HELD: continuous increment/decrement
//   4. A and B buttons return to the menu
// ---------------------------------------------------------------
class EnlightTestMode {
public:
    EnlightTestMode(Enlight&            e,
                    LightAir_Display&   disp,
                    LightAir_InputCtrl& input,
                    LightAir_UICtrl&    uiCtrl,
                    uint8_t             keypadId);

    void run();  // blocking test mode loop

private:
    Enlight&            _e;
    LightAir_Display&   _disp;
    LightAir_InputCtrl& _input;
    LightAir_UICtrl&    _uiCtrl;
    uint8_t             _keypadId;

    uint32_t _repetitions = 5;  // default 5, range 1-100
    uint32_t _lastHeldTime = 0; // track timing for held key repeat

    void renderDisplay(bool showResult, const char* resultText);
    void processInput();
    bool runEnlight();  // Returns true if a hit was detected
    const char* getColorShortName(uint8_t playerId) const;
};
