#pragma once
#include <stdint.h>

// ---------------------------------------------------------------
// Enlight hardware configuration
//
// SDO (FSPI MOSI)             -> FAR  LED, sine   waveform, focal-point emitter.
// SDI_out (FSPI MISO repurp.) -> NEAR LED, cosine waveform, wide-cone emitter.
// Sine/cosine orthogonality lets a single photodiode separate both channels:
//   correlate ADC with sintab              -> far  (rout/gout/bout)
//   correlate ADC with sintab + _cosOffset -> near (rnear/gnear/bnear)
// ---------------------------------------------------------------
struct EnlightConfig {
    uint8_t  adcHost;       // spi_host_device_t; cast when used
    int      adcClk, adcSdo, adcSdi, adcCs;
    uint32_t adcClockHz;
    uint8_t  adcCmdR, adcCmdG, adcCmdB;
    uint8_t  ledHost;       // spi_host_device_t; cast when used
    int      ledSdo;        // sine  / FAR  LED (FSPI MOSI)
    int      ledSdiOut;     // cosine / NEAR LED (FSPI MISO repurposed)
    uint32_t ledClockHz, ledFreqHz;
    float    pdmAmpOffset;  // [0.0, 0.5)
    int      afeOn;
    uint8_t  taskCore;
};

namespace EnlightDefaults {
    constexpr uint8_t  ADC_HOST      = 1;        // SPI2_HOST
    constexpr int      ADC_CLK       = 47;
    constexpr int      ADC_SDO       = 14;
    constexpr int      ADC_SDI       = 21;
    constexpr int      ADC_CS        = 13;
    constexpr uint32_t ADC_CLOCK_HZ  = 16000000;
    constexpr uint8_t  ADC_CMD_R     = 24;
    constexpr uint8_t  ADC_CMD_G     = 32;
    constexpr uint8_t  ADC_CMD_B     = 40;
    constexpr uint8_t  LED_HOST      = 2;        // SPI3_HOST
    constexpr int      LED_SDO       = 38;
    constexpr int      LED_SDI_OUT   = 36;
    constexpr uint32_t LED_CLOCK_HZ  = 16000000;
    constexpr uint32_t LED_FREQ_HZ   = 1667;
    constexpr float    PDM_AMP_OFFSET = 0.0f;
    constexpr int      AFE_ON        = 9;
    constexpr uint8_t  TASK_CORE        = 0;
    constexpr uint16_t MS_PER_REP       = 8;   // hardware constant: ms per enlight.run() repetition
}

// ---------------------------------------------------------------
// Radio configuration
// ---------------------------------------------------------------
struct RadioConfig {
    uint16_t replyTimeoutMs = 2000;
    uint8_t  espNowChannel  = 1;
};

namespace RadioDefaults {
    constexpr uint16_t REPLY_TIMEOUT_MS = 2000;
    constexpr uint8_t  CHANNEL          = 1;
}

// ---------------------------------------------------------------
// Input configuration
// ---------------------------------------------------------------
namespace InputDefaults {
    constexpr uint32_t LONG_PRESS_MS        = 500;   // ms before PRESSED transitions to HELD
    constexpr uint32_t DEBOUNCE_MS          = 50;    // ms keypad signal must be stable
    constexpr uint8_t  MAX_BUTTONS          = 2;     // max registered buttons
    constexpr uint8_t  MAX_KEYPADS          = 1;     // max registered keypads
    constexpr uint8_t  MAX_KEYPAD_KEYS      = 6;     // max keys per keypad (rows × cols)
    constexpr uint8_t  MAX_KEYPAD_EVENTS    = 6;     // max active key entries per poll()
    // Default button/keypad IDs the game framework expects in InputReport.
    // Assign these IDs when calling InputCtrl::registerButton / registerKeypad.
    constexpr uint8_t  TRIG_1_ID           = 0;     // primary trigger
    constexpr uint8_t  TRIG_2_ID           = 1;     // secondary trigger
    constexpr uint8_t  KEYPAD_ID           = 0;     // 6-key keypad
}

// ---------------------------------------------------------------
// Display configuration
// ---------------------------------------------------------------
namespace DisplayDefaults {
    constexpr uint8_t MAX_SETS          = 32;
    constexpr uint8_t MAX_BINDINGS      = 8;
    constexpr uint8_t SCREEN_WIDTH      = 128;
    constexpr uint8_t SCREEN_HEIGHT     = 64;
    constexpr uint8_t TRAY_HEIGHT       = 21;
    constexpr uint8_t FONT_HEIGHT       = 10;
    constexpr uint8_t CONTENT_HEIGHT    = SCREEN_HEIGHT - TRAY_HEIGHT;
    constexpr uint8_t TRAY_MAX_MESSAGES = TRAY_HEIGHT / FONT_HEIGHT;
}

// ---------------------------------------------------------------
// Game configuration
// ---------------------------------------------------------------
namespace GameDefaults {
    constexpr uint8_t  MSG_CONFIG        = 0x20; // radio msgType for config broadcast (even)
    constexpr uint8_t  MSG_ROSTER        = 0x22; // radio msgType for roster presence broadcast (even)
    constexpr uint32_t ROSTER_WINDOW_MS  = 3000; // ms to collect presence broadcasts during discovery
    constexpr uint32_t ROSTER_RETRY_MS   = 1000; // ms between own re-broadcasts during discovery
    constexpr uint32_t LOOP_MS           = 10;   // target game-loop duration in ms
    constexpr uint8_t  RADIO_OUT_MAX     = 4;    // max queued outgoing messages per loop
    constexpr uint8_t  RADIO_OUT_PAYLOAD = 239;  // max payload bytes per queued message (= RADIO_MAX_PAYLOAD)
    constexpr uint8_t  MAX_GAMES         = 8;    // max games registered in GameManager
    constexpr uint8_t  RADIO_REPLY_MAX   = 4;    // max queued reply messages per loop
    constexpr uint8_t  MAX_WINNER_VARS   = 8;    // max entries in a winnerVars[] table
    constexpr uint32_t SCORE_RETRY_MS    = 2000; // ms between score re-broadcasts during scoringState
    constexpr uint8_t  MAX_PARTICIPANTS  = 32;   // max roster entries (players + totems); mask must be uint32_t
}

// ---------------------------------------------------------------
// Totem identity tables
//
// Totem IDs go downward from 254 (totem01=254, totem02=253, …).
// Up to MAX_TOTEMS=16 totems are supported (IDs 239–254).
// ---------------------------------------------------------------
namespace TotemDefs {
    constexpr uint8_t MAX_TOTEM_ID = 254;
    constexpr uint8_t MAX_TOTEMS   = 16;   // IDs 239–254

    constexpr uint8_t totemIndex(uint8_t id)   { return MAX_TOTEM_ID - id; }
    constexpr uint8_t idFromIndex(uint8_t idx) { return MAX_TOTEM_ID - idx; }
    constexpr bool    isTotemId(uint8_t id) {
        return id >= (MAX_TOTEM_ID - MAX_TOTEMS + 1) && id <= MAX_TOTEM_ID;
    }

    // Short numeric labels (3 chars + null) — kept as char array for future naming schemes.
    constexpr char totemShort[MAX_TOTEMS][4] = {
        "01","02","03","04","05","06","07","08",
        "09","10","11","12","13","14","15","16",
    };
    // Long readable names (≤11 chars + null).
    constexpr char totemNames[MAX_TOTEMS][12] = {
        "Totem 01","Totem 02","Totem 03","Totem 04",
        "Totem 05","Totem 06","Totem 07","Totem 08",
        "Totem 09","Totem 10","Totem 11","Totem 12",
        "Totem 13","Totem 14","Totem 15","Totem 16",
    };
}

// ---------------------------------------------------------------
// Generic totem roles
//
// Used by LightAir_GameSetupMenu to assign freeform roles to
// totem devices not covered by named TotemVar entries.
// ---------------------------------------------------------------
namespace GenericTotemRoles {
    constexpr uint8_t NONE  = 0;
    constexpr uint8_t BONUS = 1;
    constexpr uint8_t MALUS = 2;
    constexpr uint8_t COUNT = 3;
    constexpr char    names[COUNT][8] = {
        "----", "BONUS", "MALUS"
    };
}

// ---------------------------------------------------------------
// Player identity tables
//
// Player IDs are uint8_t values 0-15 used both as the logical
// radio address (last byte of the spoofed MAC) and as an index
// into these tables for display and winner announcements.
// ---------------------------------------------------------------
namespace PlayerDefs {
    constexpr uint8_t MAX_PLAYER_ID = 16;

    // Long readable names (≤11 chars + null).
    constexpr char playerNames[MAX_PLAYER_ID][12] = {
        "00-None",   "01-Clear",   "02-Green",   "03-Yellow",
        "04-Blue",   "05-Orange",  "06-Red",     "07-Lime",
        "08-Magenta","09-Purple",  "10-Unknown", "11-Unknown",
        "12-Unknown","13-Unknown", "14-Unknown", "15-Unknown",
    };

    // Short 3-capital-letter labels (3 chars + null).
    constexpr char playerShort[MAX_PLAYER_ID][4] = {
        "NON","CLR","GRN","YLW","BLU","ORG","RED","LME",
        "MAG","PUR","UN0","UN1","UN2","UN3","UN4","UN5",
    };
}
