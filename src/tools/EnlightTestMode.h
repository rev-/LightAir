#pragma once
#include "../enlight/Enlight.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

class EnlightTestMode {
public:
    EnlightTestMode(Enlight&            e,
                    LightAir_UICtrl&    ui,
                    LightAir_Display&   disp,
                    LightAir_InputCtrl& input,
                    uint8_t             keypadId);

    void run();  // blocking; interactive Enlight test mode

private:
    // Input polling helper: returns {key, state} with rise-edge and held-repeat detection
    struct KeyEvent {
        char key;
        uint8_t state;
    };

    KeyEvent pollKey();

    Enlight&            _e;
    LightAir_UICtrl&    _ui;
    LightAir_Display&   _disp;
    LightAir_InputCtrl& _input;
    uint8_t             _keypadId;

    // Input state tracking (mimics GameSetupMenu's gPrevKeyState / gPrevButtonState)
    uint8_t    _prevKeyState[256]    = {};
    uint32_t   _lastHeldReturn[256]  = {};
    uint8_t    _prevButtonState[2]   = {};
    uint32_t   _lastButtonHeld[2]    = {};
};
