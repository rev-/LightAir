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

#include <Arduino.h>
#include <ArduinoLog.h>
#include "src/LightAir.h"
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif 

#include "src/tools/EnlightCalibRoutine.h"
#include "src/tools/EnlightTestMode.h"
#include "src/tools/GameFileServer.h"
#include "src/lua/LightAir_GameStore.h"

// ----------------------------------------------------------------
// Enlight global pointer
// Required by the Lua game binding (la.shine verbs).  Set to the
// real Enlight instance on the player path; left nullptr on the
// totem path (the verbs guard against it).
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
static LightAir_TotemRGB_HW  totemRgb;
static LightAir_LEDStrip_HW  totemStrip;
static LightAir_TotemUICtrl  totemUi(totemRgb, totemStrip);
static LightAir_TotemDriver* driver = nullptr;

// ================================================================
// PLAYER PATH — objects are trivially constructed at global scope;
// hardware init only runs when hw == PLAYER.
// ================================================================

// ---- Enlight (heap-allocated after NVS calibration is loaded) ----
static EnlightCalib       enlightCalib;
static Enlight*           enlight      = nullptr;
static EnlightCalibRoutine* calibRoutine = nullptr;
static EnlightTestMode*   testMode     = nullptr;

// ---- SPI ADC bus and sensors ----
static SpiAdcBus   adcBus;
static float       battVolts = SensorDefaults::ADC_VREF;
static VDivSensor  battSensor(SensorDefaults::BATT_R_TOP, SensorDefaults::BATT_R_BOTTOM);
static NtcSensor   ledTempSensor(SensorDefaults::LED_NTC_R_FIXED, SensorDefaults::LED_NTC_R0,
                                  SensorDefaults::LED_NTC_BETA, &battVolts);
static NtcSensor   pdTempSensor (SensorDefaults::PD_NTC_R_FIXED,  SensorDefaults::PD_NTC_R0,
                                  SensorDefaults::PD_NTC_BETA,  &battVolts);
static SpiExternal extSpi;


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
static LightAir_GameStore   gameStore;   // LittleFS-backed .lua games
static GameFileServer       shareServer; // Settings → Share games (WiFi AP)

// ================================================================
// Runtime path flag (set in setup(), read in loop())
// ================================================================
static DeviceHardware hw;

// ----------------------------------------------------------------


#ifdef TEST_UNIT
#include "src/test/LightAir_test.h"

void _setup() {
    // write as needed  
}

#include <AUnit.h>
void loop() {
    aunit::TestRunner::run();
}

#else
void _setup() {
    // Load device identity from NVS.
    PlayerConfig cfg;
    player_config_load(cfg);
    hw = cfg.hardware;

    Log.infoln("LightAir id=%u hw=%s\n",
                  cfg.id,
                  hw == DeviceHardware::TOTEM ? "TOTEM" : "PLAYER");

    if (hw == DeviceHardware::TOTEM) {
        // ------------------------------------------------------------
        // TOTEM PATH — no game files, no role registry: behaviour
        // arrives as a TotemVM program in the activation handshake.
        // ------------------------------------------------------------
        static RadioConfig radioCfg;
        radio  = new LightAir_Radio(transport, cfg.id,
                                    RadioToken::UNSET, 0, 0, radioCfg);
        driver = new LightAir_TotemDriver(*radio, totemUi);

        totemRgb.begin(TOTEM_PIN_COMM, TOTEM_PIN_R, TOTEM_PIN_G, TOTEM_PIN_B);
        totemStrip.begin(TOTEM_PIN_DATA, TOTEM_NUM_LEDS);
        totemUi.begin();

        if (!driver->begin()) {
            Log.infoln("Totem radio init FAILED — halting");
            while (true) delay(1000);
        }

        Log.infoln("Totem ready.");

    } else {
        // ------------------------------------------------------------
        // PLAYER PATH
        // ------------------------------------------------------------

        // SPI ADC bus — must be initialised before Enlight and sensors.
        if (!adcBus.begin((spi_host_device_t)EnlightDefaults::ADC_HOST,
                          EnlightDefaults::ADC_SDO, EnlightDefaults::ADC_SDI,
                          EnlightDefaults::ADC_CLK, EnlightDefaults::ADC_CS,
                          (int)EnlightDefaults::ADC_CLOCK_HZ,
                          ENLIGHT_SPI_MAX_DMA_LEN)) {
            Serial.println("SPI ADC bus init FAILED — halting");
            while (true) delay(1000);
        }
        battSensor   .begin(adcBus, SensorDefaults::CMD_BATT_VOLT, "V");
        ledTempSensor.begin(adcBus, SensorDefaults::CMD_LED_TEMP,  "C");
        pdTempSensor .begin(adcBus, SensorDefaults::CMD_PD_TEMP,   "C");
        extSpi.begin((spi_host_device_t)EnlightDefaults::ADC_HOST,
                     SPI_EXT_CS, (int)EnlightDefaults::ADC_CLOCK_HZ);

        // Enlight
        enlight_calib_load(enlightCalib);
        enlight      = new Enlight(enlightCalib);
        enlightPtr   = enlight;
        calibRoutine = new EnlightCalibRoutine(*enlight, rawDisplay, input,
                                               InputDefaults::KEYPAD_ID);
        testMode = new EnlightTestMode(*enlight, playerUi, rawDisplay, input,
                                       InputDefaults::KEYPAD_ID,
                                       &battSensor, &ledTempSensor, &pdTempSensor);
        if (!enlight->begin(adcBus.getHandle())) {
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
            Log.infoln("Radio init FAILED — halting");
            while (true) delay(1000);
        }

        // Games are .lua files on LittleFS (seeded from the embedded
        // bundle on first boot); there are no firmware-coded games.
        // Zero registered games is survivable — the menu says so instead
        // of starting a game — but it is always a fault worth logging.
        if (!gameStore.begin())
            Log.errorln("GameStore: LittleFS unavailable — no games installed");
        else if (gameStore.registerLuaGames(manager) == 0)
            Log.errorln("GameStore: no playable game files in %s", LuaDefaults::GAMES_DIR);
        LightAir_GameSetupMenu menu(manager, runner,
                                    rawDisplay, input,
                                    InputDefaults::KEYPAD_ID,
                                    *radio);
        menu.setCalibTool(*calibRoutine);
        menu.setTestTool(*testMode);
        menu.setShareTool(shareServer);
        if (menu.run() != MenuResult::Confirmed) {
            Log.infoln("Setup menu cancelled — rebooting");
            ESP.restart();
        }

        // Start game
        static SpiAdcSensor* gameSensors[] = { &battSensor, &ledTempSensor, &pdTempSensor };
        runner.begin(menu.selectedGame(), displayCtrl, input, *radio, &playerUi,
                     enlight, gameSensors, 3, &battVolts);

        Log.infoln("Player ready.");
    }
}

void loop() {
    if (hw == DeviceHardware::TOTEM) {
        driver->loop();
    } else {
        runner.update();
    }
}
#endif

void _setup_logging(){
    
    Serial.begin(115200);
    Log.begin(LOG_LEVEL, &Serial);

    Log.infoln("Log initialized with level %d", LOG_LEVEL);
}

void setup() {

    _setup_logging();
    _setup();
}
