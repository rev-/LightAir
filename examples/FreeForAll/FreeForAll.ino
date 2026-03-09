#include <LightAir.h>

// ================================================================
// FreeForAll — complete sketch for LightAir Free for All game.
//
// Pin assignments are for V6R2 hardware.
// ================================================================

// ---- Player identity (loaded from NVS in setup) ----
static PlayerConfig identity;

// ---- Enlight (needs NVS calib; pointer set in setup after NVS is read) ----
// Game files access Enlight via: extern Enlight* enlightPtr;
Enlight* enlightPtr = nullptr;

// ---- Display ----
LightAir_SSD1306Display rawDisplay(/* sda */ 4, /* scl */ 3, /* addr */ 0x3C);
LightAir_DisplayCtrl    displayCtrl(rawDisplay);

// ---- Audio / vibration / RGB ----
LightAir_BuzzerAudio    audio(/* pin */ 5);
LightAir_MotorVibration vib  (/* pin */ 6);
LightAir_RGB_HW         rgb  (/* pin */ 8, /* count */ 1);
LightAir_UICtrl         uiCtrl(audio, vib, rgb);

// ---- Radio (constructed in setup after identity is loaded) ----
LightAir_RadioESPNow transport;
LightAir_Radio*      radioPtr = nullptr;

// ---- Input ----
LightAir_HWButton trig1(/* pin */ 10, /* activeLow */ true);
LightAir_HWButton trig2(/* pin */ 11, /* activeLow */ true);

static const char    kKeys[]    = { '<', '^', '>', 'A', 'V', 'B' };
static const uint8_t kRowPins[] = { 7, 17 };
static const uint8_t kColPins[] = { 16, 15, 18 };
LightAir_HWKeypad keypad(kKeys, kRowPins, 2, kColPins, 3);

LightAir_InputCtrl inputCtrl;

// ---- Game framework ----
LightAir_GameManager manager;
LightAir_GameRunner  runner;

extern void registerAllGames(LightAir_GameManager&);

// ================================================================
void setup() {
    Serial.begin(115200);

    // -- NVS: load player identity and calibration --
    player_config_load(identity);

    EnlightCalib cal;
    enlight_calib_load(cal);

    // -- Enlight: construct with loaded calib --
    static Enlight enlightInst(EnlightConfig{
        .adcHost    = EnlightDefaults::ADC_HOST,
        .adcClk     = EnlightDefaults::ADC_CLK,
        .adcSdo     = EnlightDefaults::ADC_SDO,
        .adcSdi     = EnlightDefaults::ADC_SDI,
        .adcCs      = EnlightDefaults::ADC_CS,
        .adcClockHz = EnlightDefaults::ADC_CLOCK_HZ,
        .adcCmdR    = EnlightDefaults::ADC_CMD_R,
        .adcCmdG    = EnlightDefaults::ADC_CMD_G,
        .adcCmdB    = EnlightDefaults::ADC_CMD_B,
        .ledHost    = EnlightDefaults::LED_HOST,
        .ledSdo     = EnlightDefaults::LED_SDO,
        .ledSdiOut  = EnlightDefaults::LED_SDI_OUT,
        .ledClockHz = EnlightDefaults::LED_CLOCK_HZ,
        .ledFreqHz  = EnlightDefaults::LED_FREQ_HZ,
        .pdmAmpOffset = EnlightDefaults::PDM_AMP_OFFSET,
        .afeOn      = EnlightDefaults::AFE_ON,
        .taskCore   = EnlightDefaults::TASK_CORE,
    }, cal);
    enlightPtr = &enlightInst;
    enlightInst.begin();

    // -- Radio --
    static LightAir_Radio radioInst(transport,
        identity.id,
        /* sessionToken */ 0x01,
        /* role         */ 0x00,
        identity.team);
    radioPtr = &radioInst;
    radioPtr->begin();

    // -- Display --
    rawDisplay.begin();
    displayCtrl.begin();
    uiCtrl.setLcd(&displayCtrl);

    // -- Input --
    trig1.begin();
    trig2.begin();
    inputCtrl.registerButton(InputDefaults::TRIG_1_ID, trig1);
    inputCtrl.registerButton(InputDefaults::TRIG_2_ID, trig2);
    inputCtrl.registerKeypad(InputDefaults::KEYPAD_ID, keypad);

    // -- Select game --
    registerAllGames(manager);
    const LightAir_Game& sel =
        manager.selectGame(rawDisplay, inputCtrl, InputDefaults::KEYPAD_ID);

    // -- Config menu (optional; B skips) --
    LightAir_GameConfigMenu menu(
        sel, rawDisplay, inputCtrl, InputDefaults::KEYPAD_ID, *radioPtr);
    menu.run();

    // -- Start game runner --
    runner.begin(sel, displayCtrl, inputCtrl, *radioPtr, &uiCtrl);
}

// ================================================================
void loop() {
    runner.update();   // read → logic → output, timing-enforced
}
