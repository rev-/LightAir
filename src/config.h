#pragma once
#include <stdint.h>
#include "player_pins.h"
#include "totem_pins.h"

// ================================================================
// RadioMsg — central registry of all game and infrastructure
// radio message type bytes.
//
// Layout
// ──────
// 0x10 block  player game messages (shared by every game ruleset)
// 0x50 block  totem-mediated game messages
// 0xA0 block  infrastructure (config, roster discovery, end-game)
// 0xF0 block  totem protocol (beacon, roster)
//
// Convention: even = request, odd = reply (same across all blocks).
// typeId + sessionToken keep game sessions isolated on the wire, so
// each game reuses the same byte values rather than carving out its
// own private range.
//
// Adding a new message
// ────────────────────
// 1. Pick the next even slot in the appropriate block.
// 2. Add the constant here with a short description.
// 3. If the message is game-specific, note which ruleset(s) use it.
// 4. Never reuse a retired value; leave a comment in its place.
// ================================================================
namespace RadioMsg {

// ── 0x10 block: player game messages ───────────────────────────
// Used by every game that has direct player-to-player hits.

// Unicast hit notification sent by shooter to target.
// Reply (0x11) payload[0] = ReplySubType (TAKEN / SHONE / DOWN / FRIEND).
constexpr uint8_t MSG_LIT           = 0x10;

// End-game score broadcast, one packet per player.
constexpr uint8_t MSG_SCORE_COLLECT = 0x12;

// Periodic team-score update so teammates track aggregate points (Teams).
constexpr uint8_t MSG_POINT_REPORT  = 0x14;

// Next available in 0x10 block: 0x16

// ── 0x50 block: totem-mediated game messages ────────────────────
// Messages that travel between a player and a totem (not player→player).

// Flag state change broadcast: pickup / capture / drop (Flag game).
// payload[0] = FlagEventType; no meaningful reply expected.
constexpr uint8_t MSG_FLAG_EVENT    = 0x50;

// Control-point beacon broadcast by CP totem every 2 s (Upkeep, KingOfHill).
// payload[0] = cpTeam: 0–15 = owner team/player index; 0xFF = neutral.
//   In two-team games: 0=O, 1=X.  In KingOfHill: 0–15 = player index (cfg.id-1).
// Reply (0x53) subType = myTeam+1 (1–16) to declare presence near this CP.
constexpr uint8_t MSG_CP_BEACON     = 0x52;

// Control-point score award broadcast by CP totem (Upkeep, KingOfHill).
// payload[0] = team/player index (0–15) receiving the point.
//   In two-team games: 0=O, 1=X.  In KingOfHill: 0–15 = player index (cfg.id-1).
constexpr uint8_t MSG_CP_SCORE      = 0x54;

// BASE totem beacon (new role-based architecture).
// payload[0] = team (0=O, 1=X).
// Reply (0x57): subType = myTeam+1 (1=teamO, 2=teamX) — player near base, requesting respawn.
constexpr uint8_t MSG_BASE_BEACON   = 0x56;

// FLAG totem beacon (new role-based architecture).
// payload[0] = state (0=FLAG_IN, 1=FLAG_OUT); payload[1] = team (0=O, 1=X).
// Reply (0x59): enemy-team player picking up the flag.
constexpr uint8_t MSG_FLAG_BEACON   = 0x58;

// Broadcast flood by flag carrier when shot — flag returns to its totem.
// payload[0] = flagTeam (0=O, 1=X).  No reply expected.
constexpr uint8_t MSG_FLAG_RETURN   = 0x5A;

// Broadcast flood by flag carrier when scoring — flag returns to its totem.
// payload[0] = flagTeam (0=O, 1=X).  No reply expected.
constexpr uint8_t MSG_FLAG_SCORE    = 0x5C;

// BONUS totem beacon (new role-based architecture). payload[0] = 0 when ready.
// Reply (0x5F): player claims bonus.
constexpr uint8_t MSG_BONUS_BEACON  = 0x5E;

// MALUS totem beacon (new role-based architecture). payload[0] = 0 when ready.
// Reply (0x61): player claims malus.
constexpr uint8_t MSG_MALUS_BEACON  = 0x60;

// Next available in 0x50 block: 0x62

// ── 0xA0 block: infrastructure ──────────────────────────────────
// Sent with typeId == UNIVERSAL (0x0000); not game-scoped.

// Game configuration broadcast from host to all players.
constexpr uint8_t MSG_CONFIG        = 0xA0;

// Roster presence broadcast; players announce themselves during discovery.
constexpr uint8_t MSG_ROSTER        = 0xA2;

// End-of-game signal; forces any device still in-game into scoringState.
constexpr uint8_t MSG_END_GAME      = 0xAE;

// Next available in 0xA0 block: 0xA4 (before 0xAE) or 0xB0 (after)

// ── 0xF0 block: totem protocol ───────────────────────────────────
// All unactivated totems broadcast MSG_TOTEM_BEACON regardless of role.
// The GameRunner infrastructure intercept (host device only) replies with
// 0xF1 carrying the totem's assigned roleId (TotemRoleId constant) in payload[0].
// No reply is sent to unconfigured totems.  Once activated a totem
// switches to role-specific beacons so 0xF1 is never ambiguous.
// 0xF0/0xF1 must not be re-used by any per-game message table.

// Even: totem → broadcast beacon (IDLE state only).
// Odd (0xF1): host activation reply carrying roleId in payload[0].
constexpr uint8_t MSG_TOTEM_BEACON  = 0xF0;

// Universal end-of-game roster broadcast (typeId == UNIVERSAL).
// Sent by the host to every totem at the end of the game.
// TotemDriver calls runner->onRoster(), then reset() on receipt.
constexpr uint8_t MSG_TOTEM_ROSTER  = 0xF2;

} // namespace RadioMsg

// ---------------------------------------------------------------
// Hardware identity — stored in NVS to select player vs totem
// firmware path at boot time.
// ---------------------------------------------------------------
enum class DeviceHardware : uint8_t {
    PLAYER = 0,   // gun / player device (default when key absent)
    TOTEM  = 1,   // static game object (base, flag, control point)
};

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
    constexpr int      ADC_CLK       = PLAYER_ADC_CLK;
    constexpr int      ADC_SDO       = PLAYER_ADC_SDO;
    constexpr int      ADC_SDI       = PLAYER_ADC_SDI;
    constexpr int      ADC_CS        = PLAYER_ADC_CS;
    constexpr uint32_t ADC_CLOCK_HZ  = 16000000;
    constexpr uint8_t  ADC_CMD_R     = 24;
    constexpr uint8_t  ADC_CMD_G     = 32;
    constexpr uint8_t  ADC_CMD_B     = 40;
    constexpr uint8_t  LED_HOST      = 2;        // SPI3_HOST
    constexpr int      LED_SDO       = PLAYER_LED_SDO;
    constexpr int      LED_SDI_OUT   = PLAYER_LED_SDI_OUT;
    constexpr uint32_t LED_CLOCK_HZ  = 16000000;
    constexpr uint32_t LED_FREQ_HZ   = 1667;
    constexpr float    PDM_AMP_OFFSET = 0.0f;
    constexpr int      AFE_ON        = PLAYER_AFE_ON;
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
    constexpr uint8_t  MSG_CONFIG        = RadioMsg::MSG_CONFIG;
    constexpr uint8_t  MSG_ROSTER        = RadioMsg::MSG_ROSTER;
    constexpr uint32_t ROSTER_WINDOW_MS  = 3000; // ms to collect presence broadcasts during discovery
    constexpr uint32_t ROSTER_RETRY_MS        = 1000; // ms between own re-broadcasts during discovery
    constexpr uint32_t PRESTART_BROADCAST_MS  = 2000; // ms between MSG_ROSTER broadcasts on pre-start screen
    constexpr uint32_t LOOP_MS           = 10;   // target game-loop duration in ms
    constexpr uint8_t  RADIO_OUT_MAX     = 4;    // max queued outgoing messages per loop
    constexpr uint8_t  RADIO_OUT_PAYLOAD = 237;  // max payload bytes per queued message (= RADIO_MAX_PAYLOAD)
    constexpr uint8_t  MAX_GAMES         = 8;    // max games registered in GameManager
    constexpr uint8_t  RADIO_REPLY_MAX   = 4;    // max queued reply messages per loop
    constexpr uint8_t  MAX_WINNER_VARS   = 2;    // max entries in a winnerVars[] table (primary + tie-breaker)
    constexpr uint32_t SCORE_RETRY_MS           = 2000; // ms between score re-broadcasts during scoringState
    constexpr uint8_t  MAX_PARTICIPANTS         = 28;   // max roster entries (players + totems); mask must be uint32_t
    constexpr uint32_t TOTEM_BEACON_INTERVAL_MS = 500;  // ms between MSG_TOTEM_BEACON broadcasts
    constexpr uint8_t  MSG_END_GAME             = RadioMsg::MSG_END_GAME;
}
// Worst-case fused score payload: 4-byte mask + MAX_PARTICIPANTS slots of MAX_WINNER_VARS × int32_t.
// Must fit in a single radio packet.  Reduce MAX_PARTICIPANTS or MAX_WINNER_VARS if this fires.
static_assert(4u + (uint32_t)GameDefaults::MAX_PARTICIPANTS
                  * GameDefaults::MAX_WINNER_VARS * 4u
              <= GameDefaults::RADIO_OUT_PAYLOAD,
              "Score payload exceeds radio MTU — reduce MAX_PARTICIPANTS or MAX_WINNER_VARS");

// ---------------------------------------------------------------
// Totem identity tables
//
// Totem IDs go downward from 254 (totem01=254, totem02=253, …).
// Up to MAX_TOTEMS=16 totems are supported (IDs 239–254).
// ---------------------------------------------------------------
namespace TotemDefs {
    constexpr uint8_t MAX_TOTEM_ID    = 254;
    constexpr uint8_t MAX_TOTEMS      = 16;   // IDs 239–254
    constexpr uint8_t MAX_TOTEM_ROLES = 32;   // max entries in LightAir_TotemRoleManager

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
// Player identity tables
//
// Player IDs are uint8_t values 0-15 used both as the logical
// radio address (last byte of the spoofed MAC) and as an index
// into these tables for display and winner announcements.
// ---------------------------------------------------------------
namespace PlayerDefs {
    constexpr uint8_t MAX_PLAYER_ID = 17;  // IDs 0 (reserved) + 1–16 (players)

    // Long readable names (≤11 chars + null).
    constexpr char playerNames[MAX_PLAYER_ID][12] = {
        "00-None",   "01-Clear",   "02-Green",   "03-Yellow",
        "04-Blue",   "05-Orange",  "06-Red",     "07-Lime",
        "08-Magenta","09-Purple",  "10-Unknown", "11-Unknown",
        "12-Unknown","13-Unknown", "14-Unknown", "15-Unknown",
        "16-Unknown",
    };

    // Short 3-capital-letter labels (3 chars + null).
    constexpr char playerShort[MAX_PLAYER_ID][4] = {
        "NON","CLR","GRN","YLW","BLU","ORG","RED","LME",
        "MAG","PUR","UN0","UN1","UN2","UN3","UN4","UN5",
        "UN6",
    };
}

// ---------------------------------------------------------------
// Colour tables
//
// TeamColors  — one RGB entry per team (index 0=O, 1=X).
// PlayerColors — one RGB entry per player ID (index 0–16, matching
//                PlayerDefs::playerNames / playerShort).
//
// Used by the totem UI layer (LightAir_TotemUICtrl) and any runner
// that needs to map a team or player ID to a display colour.
// ---------------------------------------------------------------
namespace TeamColors {
    // [team][channel]  0=R, 1=G, 2=B  — up to 8 teams supported
    static constexpr uint8_t kCount = 8;
    constexpr uint8_t kColors[kCount][3] = {
        {   0, 255, 255 },  // team 0 : cyan
        { 255,   0, 255 },  // team 1 : magenta
        { 255, 255,   0 },  // team 2 : yellow
        { 255,   0,   0 },  // team 3 : red
        {   0, 255,   0 },  // team 4 : green
        { 255, 128,   0 },  // team 5 : orange
        {   0,   0, 255 },  // team 6 : blue
        { 128,   0, 255 },  // team 7 : purple
    };
}

namespace PlayerColors {
    // [playerID][channel]  0=R, 1=G, 2=B  — mirrors PlayerDefs::playerNames
    constexpr uint8_t kColors[PlayerDefs::MAX_PLAYER_ID][3] = {
        {   0,   0,   0 },  // 00-None    : off
        { 255, 255, 255 },  // 01-Clear   : white
        {   0, 255,   0 },  // 02-Green
        { 255, 255,   0 },  // 03-Yellow
        {   0,   0, 255 },  // 04-Blue
        { 255, 128,   0 },  // 05-Orange
        { 255,   0,   0 },  // 06-Red
        { 128, 255,   0 },  // 07-Lime
        { 255,   0, 255 },  // 08-Magenta
        { 128,   0, 255 },  // 09-Purple
        { 128, 128, 128 },  // 10-Unknown
        { 128, 128, 128 },  // 11-Unknown
        { 128, 128, 128 },  // 12-Unknown
        { 128, 128, 128 },  // 13-Unknown
        { 128, 128, 128 },  // 14-Unknown
        { 128, 128, 128 },  // 15-Unknown
        { 128, 128, 128 },  // 16-Unknown
    };
}
