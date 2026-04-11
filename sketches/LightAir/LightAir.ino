// ================================================================
// LightAir.ino — unified firmware for LightAir player and totem
// devices.
//
// At boot, the sketch reads the NVS key "hw" (DeviceHardware enum,
// written by the provisioning sketch) and follows one of two paths:
//
//   DeviceHardware::PLAYER  — initialises Enlight photodiode, OLED
//     display, buzzer, vibration, keypad, trigger buttons, and radio;
//     then runs the game setup menu and enters the game loop.
//
//   DeviceHardware::TOTEM   — initialises WS2812B LED strip, discrete
//     RGB indicator, and radio; then runs the totem driver loop.
//
// A single compiled binary can be flashed to either board type; the
// correct path is selected at runtime, which simplifies OTA updates.
//
// Pin definitions are in src/player_pins.h and src/totem_pins.h.
// ================================================================

#include <LightAir.h>
#include "../src/enlight/EnlightCalibRoutine.h"

// ----------------------------------------------------------------
// Enlight global pointer
// Required by every ruleset translation unit.  Set to the real
// Enlight instance on the player path; left nullptr on the totem
// path (player ruleset code is compiled in but never called).
// ----------------------------------------------------------------
Enlight* enlightPtr = nullptr;

// ----------------------------------------------------------------
// Shared transport + radio (constructed after NVS load)
// ----------------------------------------------------------------
static LightAir_RadioESPNow transport;
static LightAir_Radio*      radio = nullptr;

// ================================================================
// TOTEM PATH — objects are trivially constructed at global scope;
// hardware init (begin() calls) only runs when hw == TOTEM.
// ================================================================
static LightAir_TotemRGB_HW      totemRgb;
static LightAir_LEDStrip_HW      totemStrip;
static LightAir_TotemUICtrl      totemUi(totemRgb, totemStrip);
static LightAir_TotemRoleManager roleMgr;
static LightAir_TotemDriver*     driver = nullptr;

// ================================================================
// PLAYER PATH — objects are trivially constructed at global scope;
// hardware init only runs when hw == PLAYER.
// ================================================================

// ---- Enlight (heap-allocated after NVS calibration is loaded) ----
static EnlightCalib       enlightCalib;
static Enlight*           enlight      = nullptr;
static EnlightCalibRoutine* calibRoutine = nullptr;

// EnlightConfig: pin values come from player_pins.h;
// timing/frequency constants come from EnlightDefaults (src/config.h).
static const EnlightConfig enlightCfg = {
    /* adcHost     */ EnlightDefaults::ADC_HOST,
    /* adcClk      */ PLAYER_ADC_CLK,
    /* adcSdo      */ PLAYER_ADC_SDO,
    /* adcSdi      */ PLAYER_ADC_SDI,
    /* adcCs       */ PLAYER_ADC_CS,
    /* adcClockHz  */ EnlightDefaults::ADC_CLOCK_HZ,
    /* adcCmdR     */ EnlightDefaults::ADC_CMD_R,
    /* adcCmdG     */ EnlightDefaults::ADC_CMD_G,
    /* adcCmdB     */ EnlightDefaults::ADC_CMD_B,
    /* ledHost     */ EnlightDefaults::LED_HOST,
    /* ledSdo      */ PLAYER_LED_SDO,
    /* ledSdiOut   */ PLAYER_LED_SDI_OUT,
    /* ledClockHz  */ EnlightDefaults::LED_CLOCK_HZ,
    /* ledFreqHz   */ EnlightDefaults::LED_FREQ_HZ,
    /* pdmAmpOff   */ EnlightDefaults::PDM_AMP_OFFSET,
    /* afeOn         */ PLAYER_AFE_ON,
    /* taskCore      */ EnlightDefaults::TASK_CORE,
    /* afeStartupUs  */ EnlightDefaults::AFE_STARTUP_MICROS,
    /* satHigh       */ EnlightDefaults::SAT_HIGH,
    /* satLow        */ EnlightDefaults::SAT_LOW,
};

// ---- Display ----
static LightAir_SSD1306Display rawDisplay(PLAYER_I2C_SDA, PLAYER_I2C_SCL);
static LightAir_DisplayCtrl    displayCtrl(rawDisplay);

// ---- Audio / Vibration / RGB ----
static LightAir_BuzzerAudio    audio(PLAYER_SPK);
static LightAir_MotorVibration vib(PLAYER_VIB);
static LightAir_RGB_HW         rgb;  // no RGB LED on V6R2; pins default to -1 (disabled)

// ---- Player UI ----
static LightAir_UICtrl playerUi(audio, vib, rgb);

// ---- Input ----
static const char    kKeys[]    = { '<', '^', '>', 'A', 'V', 'B' };
static const uint8_t kRowPins[] = { PLAYER_SW_R1, PLAYER_SW_R2 };
static const uint8_t kColPins[] = { PLAYER_SW_C1, PLAYER_SW_C2, PLAYER_SW_C3 };
static LightAir_HWKeypad keypad(kKeys, kRowPins, 2, kColPins, 3);
static LightAir_HWButton trig1(PLAYER_TRIG_1);
static LightAir_HWButton trig2(PLAYER_TRIG_2);
static LightAir_InputCtrl input;

// ---- Game ----
static LightAir_GameManager manager;
static LightAir_GameRunner  runner;

// ================================================================
// Runtime path flag (set in setup(), read in loop())
// ================================================================
static DeviceHardware hw;

// ----------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // Load device identity from NVS.
    PlayerConfig cfg;
    player_config_load(cfg);
    hw = cfg.hardware;

    Serial.printf("LightAir id=%u hw=%s\n",
                  cfg.id,
                  hw == DeviceHardware::TOTEM ? "TOTEM" : "PLAYER");

    if (hw == DeviceHardware::TOTEM) {
        // ------------------------------------------------------------
        // TOTEM PATH
        // ------------------------------------------------------------
        registerAllTotems(roleMgr);

        static RadioConfig radioCfg;
        radio  = new LightAir_Radio(transport, cfg.id,
                                    RadioToken::UNSET, 0, 0, radioCfg);
        driver = new LightAir_TotemDriver(*radio, totemUi, roleMgr);

        totemRgb.begin(TOTEM_PIN_COMM, TOTEM_PIN_R, TOTEM_PIN_G, TOTEM_PIN_B);
        totemStrip.begin(TOTEM_PIN_DATA, TOTEM_NUM_LEDS);
        totemUi.begin();

        if (!driver->begin()) {
            Serial.println("Totem radio init FAILED — halting");
            while (true) delay(1000);
        }

        Serial.println("Totem ready.");

    } else {
        // ------------------------------------------------------------
        // PLAYER PATH
        // ------------------------------------------------------------

        // Enlight
        enlight_calib_load(enlightCalib);
        enlight      = new Enlight(enlightCfg, enlightCalib);
        enlightPtr   = enlight;
        calibRoutine = new EnlightCalibRoutine(*enlight, rawDisplay, input,
                                               InputDefaults::KEYPAD_ID);
        if (!enlight->begin()) {
            Serial.println("Enlight init FAILED — halting");
            while (true) delay(1000);
        }

        // Display
        rawDisplay.begin();
        displayCtrl.begin();

        // Input
        keypad.begin();
        trig1.begin();
        trig2.begin();
        input.registerKeypad(InputDefaults::KEYPAD_ID, keypad);
        input.registerButton(InputDefaults::TRIG_1_ID, trig1);
        input.registerButton(InputDefaults::TRIG_2_ID, trig2);

        // Radio
        static RadioConfig radioCfg;
        radio = new LightAir_Radio(transport, cfg.id,
                                   RadioToken::UNSET, 0, 0, radioCfg);
        if (!radio->begin()) {
            Serial.println("Radio init FAILED — halting");
            while (true) delay(1000);
        }

        // Game setup menu (blocking)
        registerAllGames(manager);
        LightAir_GameSetupMenu menu(manager, runner,
                                    rawDisplay, input,
                                    InputDefaults::KEYPAD_ID,
                                    *radio);
        menu.setCalibRoutine(*calibRoutine);
        if (menu.run() != MenuResult::Confirmed) {
            Serial.println("Setup menu cancelled — rebooting");
            ESP.restart();
        }

        // Start game
        runner.begin(menu.selectedGame(), displayCtrl, input, *radio, &playerUi);

        Serial.println("Player ready.");
    }
}

// ----------------------------------------------------------------
void loop() {
    if (hw == DeviceHardware::TOTEM) {
        driver->loop();
    } else {
        runner.update();
    }
}
