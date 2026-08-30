#ifndef CONFIG_H
#define CONFIG_H

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
// Used by every game where players shine each other directly.

// Unicast lit notification, sent by the shining player to the lit target.
// Reply (0x11) payload[0] = ReplySubType (TAKEN / SHONE / DOWN / FRIEND).
constexpr uint8_t MSG_LIT           = 0x10;

// End-game score broadcast, one packet per player.
constexpr uint8_t MSG_SCORE_COLLECT = 0x12;

// Periodic team-score update so teammates track aggregate points (Teams).
constexpr uint8_t MSG_POINT_REPORT  = 0x14;

// 0x16 is used by games/virus.lua (Lua-declared infection broadcast;
// game files may claim even msgTypes outside the 0xA0/0xF0 blocks —
// typeId + sessionToken isolate games on the wire).

// Splash broadcast, sent by a player who has just absorbed a LIT, so that
// bystanders can take graded splash damage from the same beam.  Used by
// games/lib/projector.lua for any profile that declares a splash.
//   payload[0] = splash strength, in standard hits
//   payload[1] = RSSI gate, as a positive magnitude (50 means -50 dBm):
//                the ATTACKER's projector declares it; a bystander compares
//                its own reading of this packet against it.
//   payload[2] = origin: 1 = a direct optical LIT.  A splash-induced hit
//                never re-broadcasts, which is what stops a chain reaction.
// Single-hop only (resend 0) — a flooded splash would reach the whole field.
// No reply expected.
constexpr uint8_t MSG_SPLASH        = 0x18;
// Next available in 0x10 block: 0x1A

// ── 0x50 block: totem-mediated game messages ────────────────────
// Messages that travel between a player and a totem (not player→player).

// Flag state change broadcast: pickup / capture / drop (Flag game).
// Flooded broadcast that serves BOTH duties for one event (one flood, not two):
//   payload[0] = FlagEventType (TAKEN/DROPPED/SCORED).
//   payload[1] = flag's owning team (0=O, 1=X) — players use [0]/[1] to sync
//                carrier state and team points.
//   payload[2] = target flag totem's id (the carrier remembers it from the
//                beacon it took). The owning FLAG totem animates only when
//                [2] == its own id, so the right flag reacts (multiple flags
//                per team work) and the hopped broadcast still reaches it when
//                the carrier drops/scores out of the totem's direct range.
// No meaningful reply expected.
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
// Unanswered: a pickup is announced to everyone with MSG_FLAG_EVENT, which
// is what the FLAG totem itself listens on.
constexpr uint8_t MSG_FLAG_BEACON   = 0x58;

// Reserved (retired): flag drop/score is now carried by MSG_FLAG_EVENT
// sub-types FlagEvent::DROPPED / FlagEvent::SCORED, which both the Flag
// ruleset and FlagTotem already use.  Do not reuse these byte values.
constexpr uint8_t MSG_FLAG_RETURN   = 0x5A;
constexpr uint8_t MSG_FLAG_SCORE    = 0x5C;

// BONUS totem beacon (new role-based architecture). payload[0] = 0 when ready.
// Reply (0x5F): player claims bonus.
constexpr uint8_t MSG_BONUS_BEACON  = 0x5E;

// MALUS totem beacon (new role-based architecture). payload[0] = 0 when ready.
// Reply (0x61): player claims malus.
constexpr uint8_t MSG_MALUS_BEACON  = 0x60;

// Reserved (retired): player → totem unicasts that once carried respawn and
// CP presence.  They existed because a broadcast beacon's replies were being
// dropped — the radio released a broadcast's pending slot on the first reply,
// so only one of the many answers to a beacon ever arrived.  That is fixed in
// LightAir_Radio (a broadcast keeps its slot for the whole reply window), so
// the odd-reply signal carries both again and there is one mechanism instead
// of two.  Do not reuse these byte values.
constexpr uint8_t MSG_RESPAWN_NOTIFY = 0x62;
constexpr uint8_t MSG_CP_NOTIFY      = 0x64;

// Next available in 0x50 block: 0x66

// ── 0xA0 block: infrastructure ──────────────────────────────────
// Sent with typeId == UNIVERSAL (0x0000); not game-scoped.

// Game configuration broadcast from host to all players.
constexpr uint8_t MSG_CONFIG        = 0xA0;

// Roster presence broadcast; players announce themselves during discovery.
constexpr uint8_t MSG_ROSTER        = 0xA2;

// Join confirmation sent by a non-DM when the player presses "O" to join.
// No payload; senderId identifies the joining player.
constexpr uint8_t MSG_JOIN              = 0xA4;

// Countdown-start broadcast from DM to all joined players.
// payload[0] = countdown_secs / 10 (multiply by 10 to recover; 0 = no delay).
constexpr uint8_t MSG_START_COUNTDOWN   = 0xA6;

// End-of-game signal; forces any device still in-game into scoringState.
constexpr uint8_t MSG_END_GAME      = 0xAE;

// Next available in 0xA0 block: 0xA8 (before 0xAE) or 0xB0 (after)

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
// FlagEvent — payload[0] sub-types of MSG_FLAG_EVENT (0x50).
// Shared by the Flag ruleset (broadcaster) and FlagTotem (listener)
// so both agree on the flag pickup/score/drop protocol.
//   payload[0] = FlagEvent sub-type; payload[1] = flag's owning team (0=O,1=X).
// ---------------------------------------------------------------
namespace FlagEvent {
    constexpr uint8_t TAKEN   = 1;  // a player picked up the flag
    constexpr uint8_t DROPPED = 2;  // carrier was shone; flag returns home
    constexpr uint8_t SCORED  = 3;  // flag captured at a base; flag returns home
}

// ---------------------------------------------------------------
// Hardware identity — stored in NVS to select player vs totem
// firmware path at boot time.
// ---------------------------------------------------------------
enum class DeviceHardware : uint8_t {
    PLAYER = 0,   // gun / player device (default when key absent)
    TOTEM  = 1,   // static game object (base, flag, control point)
};


namespace EnlightDefaults {
    constexpr uint8_t  ADC_HOST      = 1;        // SPI2_HOST
    constexpr int      ADC_CLK       = PLAYER_ADC_CLK;
    constexpr int      ADC_SDO       = PLAYER_ADC_SDO;
    constexpr int      ADC_SDI       = PLAYER_ADC_SDI;
    constexpr int      ADC_CS        = PLAYER_ADC_CS;
    constexpr uint32_t ADC_CLOCK_HZ  = 16000000;
    // ADC128S102 channel-select bytes — channel << 3 (ADD2..ADD0 in bits 5:3).
    constexpr uint8_t  ADC_CMD_R     = 24;       // channel 3
    constexpr uint8_t  ADC_CMD_G     = 32;       // channel 4
    constexpr uint8_t  ADC_CMD_B     = 40;       // channel 5
    constexpr uint8_t  LED_HOST      = 2;        // SPI3_HOST
    constexpr int      LED_SDO       = PLAYER_LED_SDO;
    constexpr int      LED_SDI_OUT   = PLAYER_LED_SDI_OUT;
    constexpr uint32_t LED_CLOCK_HZ  = 16000000;
    constexpr uint32_t LED_FREQ_HZ   = 1667;
    constexpr float    PDM_AMP_OFFSET = 0.1f;
    constexpr int      AFE_ON           = PLAYER_AFE_ON;
    constexpr uint8_t  TASK_CORE        = 0;
    constexpr uint16_t MS_PER_REP       = 8;    // tested ms per enlight.run() repetition
    // AFE needs ~3 sine periods to settle after power-on.
    // 3 × (1 / 1667 Hz) ≈ 1800 µs; 2000 µs gives a small margin.
    constexpr uint32_t AFE_STARTUP_MICROS = 2000;
    constexpr uint16_t SAT_HIGH      = 4085;
    constexpr uint16_t SAT_LOW       = 10;
    constexpr float    SAT_DITCH_FRAC  = 0.95f; // ditch period if any channel has >95% saturated samples
    constexpr float    SAT_SWITCH_FRAC = 0.02f; // switch to low-power PDM if >2% of a cycle's active samples saturated
    constexpr float    LOW_POWER_FACTOR = 0.1f; // amplitude scale for the dim PDM buffer
    // Retroreflector return falls as 1/x^RANGE_FALLOFF_EXP.  One reference
    // measurement at a known distance therefore fixes the whole curve, which
    // is what lets classify() report a distance estimate in metres.
    // Re-fit against measurement by editing this constant alone.
    constexpr float    RANGE_FALLOFF_EXP = 3.0f;
    // Distance, in metres, at which calibration step 1 captures the reference
    // return.  Changing it invalidates stored refFar*/refDistM pairs only if
    // the operator does not re-calibrate — refDistM travels with the values.
    constexpr uint8_t  CAL_REF_DIST_M  = 5;
}

// ---------------------------------------------------------------
// Projector optical limits
//
// The projector object lives in games/lib/projector.lua and clamps its own
// balance values at load.  Only the numbers that reach the hardware are
// bounded here, because only these can put Enlight into a bad state; they
// are re-applied by the la.shine_config verb whatever a game file asks for.
//
// MAX_CYCLES is not a hardware limit — nothing in Enlight binds below
// ~8000 (the uint16_t millisecond cap in triggerEnlight()).  100 matches
// the ceiling EnlightTestMode already uses and is a typo guard, not a
// recommendation: the playable range is far lower, bounded by AFE on-time
// and by how long a player can hold a target steady.
// ---------------------------------------------------------------
namespace ProjectorLimits {
    constexpr uint16_t MIN_CYCLES      = 1,   MAX_CYCLES      = 100;
    constexpr uint16_t MIN_COOLDOWN_MS = 0,   MAX_COOLDOWN_MS = 10000;
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
    constexpr uint32_t HELD_REPEAT_MS       = 100;   // ms between HELD key repeat events in menu
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
    constexpr uint8_t TRAY_HEIGHT       = 30;
    constexpr uint8_t FONT_HEIGHT       = 10;
    // ArialMT_Plain_10 glyphs start 3 px below the cell top (font leading).
    // Add this offset when aligning icons to text rendered at the same y.
    constexpr uint8_t FONT_TOP_PADDING  = 3;
    constexpr uint8_t CONTENT_HEIGHT    = SCREEN_HEIGHT - TRAY_HEIGHT;
    constexpr uint8_t TRAY_MAX_MESSAGES = TRAY_HEIGHT / FONT_HEIGHT;
    // Grid cell dimensions for MonitorVar layout (col/row → pixel coords).
    // 2 columns fit in SCREEN_WIDTH; CELL_HEIGHT includes 2 px padding below text.
    constexpr uint8_t CELL_COLS         = 2;
    constexpr uint8_t CELL_WIDTH        = SCREEN_WIDTH / CELL_COLS;   // 64
    constexpr uint8_t CELL_HEIGHT       = FONT_HEIGHT + 2;
    // Y coordinate of the last text row — always pinned to the screen bottom.
    constexpr uint8_t BOTTOM_LINE_Y     = SCREEN_HEIGHT - FONT_HEIGHT - FONT_TOP_PADDING;
    // A bar binding draws to the right of its 8 px icon (+2 px gap) and must
    // stay inside its own cell, so this is also the widest a bar may be.
    constexpr uint8_t ICON_GUTTER       = 10;
    constexpr uint8_t BAR_WIDTH         = CELL_WIDTH - ICON_GUTTER - 4;
    constexpr uint8_t BAR_HEIGHT        = 6;
}

// ---------------------------------------------------------------
// Game configuration
// ---------------------------------------------------------------
namespace GameDefaults {
    constexpr uint8_t  MSG_CONFIG             = RadioMsg::MSG_CONFIG;
    constexpr uint8_t  MSG_ROSTER             = RadioMsg::MSG_ROSTER;
    constexpr uint8_t  MSG_JOIN               = RadioMsg::MSG_JOIN;
    constexpr uint8_t  MSG_START_COUNTDOWN    = RadioMsg::MSG_START_COUNTDOWN;
    constexpr uint8_t  COUNTDOWN_DEFAULT_S    = 20;   // default pre-game countdown in seconds
    constexpr uint32_t ROSTER_WINDOW_MS  = 3000; // ms to collect presence broadcasts during discovery
    constexpr uint32_t ROSTER_RETRY_MS        = 1000; // ms between own re-broadcasts during discovery
    constexpr uint32_t PRESTART_BROADCAST_MS  = 2000; // ms between MSG_ROSTER broadcasts on pre-start screen
    constexpr uint32_t LOOP_MS           = 10;   // target game-loop duration in ms
    constexpr uint8_t  RADIO_OUT_MAX     = 4;    // max queued outgoing messages per loop
    constexpr uint8_t  RADIO_OUT_PAYLOAD = 237;  // max payload bytes per queued message (= RADIO_MAX_PAYLOAD)
    // Max games in the menu.  Each slot costs a manifest plus a
    // placeholder descriptor whether or not a file fills it, and the
    // projectors have no PSRAM — every byte of that table is internal RAM
    // taken from the one Lua state that has to fit beside it.  16 is twice
    // the stock catalogue, which is room to add without paying for 50.
    constexpr uint8_t  MAX_GAMES         = 16;
    constexpr uint8_t  RADIO_REPLY_MAX   = 4;    // max queued reply messages per loop
    constexpr uint8_t  RADIO_REPLY_PAYLOAD = 237; // max payload bytes per queued reply (0xF1 carries TotemVM programs)
    constexpr uint8_t  MAX_WINNER_VARS   = 2;    // max entries in a winnerVars[] table (primary + tie-breaker)
    constexpr uint32_t SCORE_RETRY_MS           = 2000; // ms between score re-broadcasts during scoringState
    constexpr uint32_t SCORE_TIMEOUT_MS         = 10000;// ms before winner shown despite missing scores
    constexpr uint8_t  MAX_PARTICIPANTS         = 28;   // max entries for totems; players use MAX_PLAYER_ID
    constexpr uint32_t TOTEM_BEACON_INTERVAL_MS = 500;  // ms between MSG_TOTEM_BEACON broadcasts
    constexpr uint8_t  MSG_END_GAME             = RadioMsg::MSG_END_GAME;
}
// ---------------------------------------------------------------
// Lua game engine configuration
// ---------------------------------------------------------------
namespace LuaDefaults {
    constexpr uint8_t  API_VERSION     = 1;      // game-file `api` contract version
    constexpr uint8_t  MAX_LUA_GAMES   = 4;      // fully-loaded instances (selected game + scratch); the
                                                 // menu lists lightweight manifests, loaded on selection
    constexpr uint8_t  MAX_VARS        = 24;     // int + text slots per game
    constexpr uint8_t  MAX_TEXT_LEN    = 16;     // capacity of one text slot (incl. NUL)
    constexpr uint8_t  MAX_VAR_ID      = 20;     // max chars of a var/config id
    constexpr uint8_t  MAX_CFG_NAME    = 13;     // menu label buffer (12 chars + NUL)
    constexpr uint8_t  MAX_RULES       = 16;     // state-transition rules per game
    constexpr uint8_t  MAX_MSG_RULES   = 24;     // (state, msgType) handler pairs
    constexpr uint8_t  MAX_MONITOR     = 16;     // monitor entries per game
    constexpr uint8_t  MAX_STATES      = 8;      // game states (mask fits uint32)
    constexpr uint8_t  MAX_COUNTDOWNS  = 4;      // vars with countdown_in per game
    constexpr uint8_t  MAX_GAME_NAME   = 16;     // display name buffer (15 + NUL)
    constexpr uint32_t INSTR_BUDGET    = 200000; // Lua instructions per callback
    constexpr const char* GAMES_DIR    = "/games";
    constexpr const char* LIB_DIR      = "/games/lib";
}

// ---------------------------------------------------------------
// Game share server (Settings -> Share games)
// ---------------------------------------------------------------
namespace ShareDefaults {
    constexpr const char* AP_SSID_PREFIX = "LightAir-";   // + player short name
    constexpr const char* AP_PASSWORD    = "lightair";    // WPA2 (>= 8 chars)
    constexpr uint16_t    HTTP_PORT      = 80;
    constexpr uint32_t    MAX_UPLOAD     = 64 * 1024;     // one .lua file
}

// ---------------------------------------------------------------
// TotemVM — fixed state-machine interpreter in totem firmware,
// configured by the program carried in the 0xF1 activation reply.
// See docs/totem-behavior-handshake.md for the normative encoding.
// ---------------------------------------------------------------
namespace TotemVMDefs {
    constexpr uint8_t VERSION        = 1;
    constexpr uint8_t MAX_STATES     = 8;
    constexpr uint8_t MAX_REGS       = 8;
    constexpr uint8_t MAX_TIMERS     = 4;
    constexpr uint8_t MAX_RULES      = 32;   // total across all states
    constexpr uint8_t MAX_PROG       = 225;  // program bytes inside the 0xF1 payload
    constexpr uint8_t MAX_BCAST_TPL  = 8;    // template bytes per bcast action
    constexpr uint8_t MAX_ENTER_DEPTH = 4;   // goto/enter recursion cap
}

// ---------------------------------------------------------------
// Totem identity tables
//
// Totem IDs go downward from 254 (totem01=254, totem02=253, …).
// Up to MAX_TOTEMS=16 totems are supported (IDs 239–254).
// ---------------------------------------------------------------
namespace TotemDefs {
    constexpr uint8_t MAX_TOTEM_ID    = 254;
    constexpr uint8_t MAX_TOTEMS      = 16;   // IDs 239–254
    constexpr uint8_t MAX_TOTEM_ROLES = 8;    // max totem roles one game declares

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
// Totem LED strip physical layout
//
// Groups the 13 strip LEDs into named zones / sides / scan-stations so
// strip animations can target a *shape* (which LEDs and how they move),
// not just a colour.  Edit these tables if the physical wiring/geometry
// of the totem strip changes; kNumLeds is the single source of truth for
// strip length (totem_pins.h::TOTEM_NUM_LEDS derives from it).
//
// Geometry (rectangle outline + center spine):
//   short1 = 0,1   long1 = 2,3,4   short2 = 5,6   long2 = 7,8,9
//   centerline (spine through the middle) = 10,11,12; LED 11 = exact center.
//   long2 runs antiparallel to long1, so long2's last LED (9) sits beside
//   long1's first LED (2).
// ---------------------------------------------------------------
namespace TotemLedLayout {
    constexpr uint8_t kPerimeter[]     = { 0,1,2,3,4,5,6,7,8,9 };
    constexpr uint8_t kPerimeterCount  = sizeof(kPerimeter) / sizeof(kPerimeter[0]);

    constexpr uint8_t kCenterLine[]    = { 10,11,12 };
    constexpr uint8_t kCenterLineCount = sizeof(kCenterLine) / sizeof(kCenterLine[0]);

    constexpr uint8_t kCenter          = 11;  // single LED, rectangle center

    // Named sides — kept as documentation of the wiring; no current effect
    // consumes them, but they make the geometry self-describing if a
    // side-anchored effect is added later.
    constexpr uint8_t kSideShort1[]    = { 0,1 };
    constexpr uint8_t kSideLong1[]     = { 2,3,4 };
    constexpr uint8_t kSideShort2[]    = { 5,6 };
    constexpr uint8_t kSideLong2[]     = { 7,8,9 };

    // "Rungs" level across the rectangle's width, short1-end to short2-end;
    // long2 is indexed in reverse because it runs antiparallel to long1.
    // Used by the VerticalScan effect (ping-pong sweep along the length).
    constexpr uint8_t kStation0[]      = { 0, 1 };
    constexpr uint8_t kStation1[]      = { 2, 9, 10 };
    constexpr uint8_t kStation2[]      = { 3, 8, 11 };
    constexpr uint8_t kStation3[]      = { 4, 7, 12 };
    constexpr uint8_t kStation4[]      = { 5, 6 };
    constexpr uint8_t kStationCount    = 5;

    // Strip length.  Derived from the hardware pin header (totem_pins.h,
    // included above) so there's one source of truth; the static_assert
    // guards against the zone tables drifting out of sync with it.
    constexpr uint8_t kNumLeds = TOTEM_NUM_LEDS;
    static_assert(kNumLeds == kPerimeterCount + kCenterLineCount,
                  "TotemLedLayout zones must cover exactly TOTEM_NUM_LEDS LEDs");
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

// Worst-case fused score payload: (id + MAX_WINNER_VARS × int32_t) × MAX_PLAYER_ID players.
// Must fit in a single radio packet.
static_assert((1u + GameDefaults::MAX_WINNER_VARS * 4u) * (PlayerDefs::MAX_PLAYER_ID - 1u)
              <= GameDefaults::RADIO_OUT_PAYLOAD,
              "Score payload exceeds radio MTU — reduce MAX_WINNER_VARS or MAX_PLAYER_ID");

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

// ---------------------------------------------------------------
// Team names
//
// One short label per team index, in step with TeamColors::kColors:
// team 0 is "O", team 1 is "X", and the extra slots keep going through
// the alphabet.  Everything the player sees — the pre-game roster, the
// team-assignment submenu, the start summary — names teams through
// forTeam(), and game files reach the same table through the la.team_short()
// kernel verb, so nothing anywhere spells "O"/"X" out for itself.
// ---------------------------------------------------------------
namespace TeamNames {
    constexpr const char* kNames[TeamColors::kCount] = {
        "O", "X", "C", "D", "E", "F", "G", "H",
    };
    // Safe lookup: clamps out-of-range team indices to entry 0.
    constexpr const char* forTeam(uint8_t team) {
        return kNames[(team < TeamColors::kCount) ? team : 0];
    }
}

// ---------------------------------------------------------------
// Per-team LED rhythm
//
// Layered onto a role's *idle* animation so two totems doing the same job
// for different teams are tellable apart by tempo (speed + beat count),
// not just colour — important for colour-blind players and for the up-to-8
// teams that share TeamColors::kColors.  periodMs = one motion cycle;
// pulseCount = how many cycles play before a silent beat, then repeat.
// ---------------------------------------------------------------
namespace TeamLedRhythm {
    struct Rhythm { uint16_t periodMs; uint8_t pulseCount; };
    constexpr uint8_t kCount = TeamColors::kCount;  // stays in sync with colours
    constexpr Rhythm kTable[kCount] = {
        { 1800, 1 },  // team 0 : slow single pulse
        {  900, 2 },  // team 1 : fast double pulse
        { 1300, 3 },  // team 2 : medium triple pulse
        {  500, 1 },  // team 3 : very fast single pulse
        { 1800, 2 },  // team 4
        {  900, 3 },  // team 5
        { 1300, 1 },  // team 6
        {  500, 2 },  // team 7
    };
    // Safe lookup: clamps out-of-range team indices to entry 0.
    constexpr Rhythm forTeam(uint8_t team) {
        return kTable[(team < kCount) ? team : 0];
    }
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

namespace colorBox {
    constexpr float colorBox[PlayerDefs::MAX_PLAYER_ID][4] = {
        { 0, 0, 0, 0 },              //0
        { 0.4, 0.25, 0.58, 0.42 },   //1 Clear
        { 0.1, 0.0, 0.76, 0.60 },    //2 Green
        { 0.65, 0.51, 1, 0.85 },     //3 Yellow
        { 0.05, 0, 0.33, 0.15 },     //4 Blue
        { 0.78, 0.62, 0.85, 0.70 },  //5 Orange
        { 1, 0.85, 1, 0.0 },         //6 Red
        { 0.5, 0.3, 0.82, 0.65 },    //7 Lime
        { 0.6, 0.35, 0.235, 0.10 },  //8 Magenta
        { 0.42, 0.26, 0.4, 0.24 },   //9 Purple
        { -10, -10, -10, -10 },      //10
        { -10, -10, -10, -10 },      //11
        { -10, -10, -10, -10 },      //12
        { -10, -10, -10, -10 },      //13
        { -10, -10, -10, -10 },      //14
        { -10, -10, -10, -10 },      //15
        { -10, -10, -10, -10 },      //16
        };
}

// ---------------------------------------------------------------
// Sensor configuration
// ---------------------------------------------------------------
namespace SensorDefaults {
    // ADC128S102 channel-select bytes — channel << 3 (ADD2..ADD0 in bits 5:3).
    // Channels 3-5 are the Enlight photodiode inputs (see EnlightDefaults).
    static constexpr uint8_t  CMD_BATT_VOLT             = 0x00;   // channel 0
    static constexpr uint8_t  CMD_LED_TEMP              = 0x08;   // channel 1
    static constexpr uint8_t  CMD_PD_TEMP               = 0x38;   // channel 7

    // ADC reference voltage.
    static constexpr float    ADC_VREF                  = 3.3f;

    // Divider supply rails.  The battery divider and the LED NTC divider are
    // powered from the battery, so their readings scale with it — the NTC needs
    // the measured battery voltage passed in as its supply, and the ADC
    // reference does not cancel out.  The PD NTC divider runs off a fixed 3.3 V
    // rail, equal to the ADC reference, so there the reference cancels and the
    // battery voltage is irrelevant.

    // NTC parameters — LED board temperature sensor.  Battery-referenced.
    // Reference part: NTCG104EF104FT1X
    static constexpr float    LED_NTC_R_FIXED           = 100000.0f;
    static constexpr float    LED_NTC_R0                = 100000.0f;
    static constexpr float    LED_NTC_BETA              = 4308.0f;

    // NTC parameters — photodiode temperature sensor.  Fixed 3.3 V reference.
    // R_FIXED is RT1, which carries a 1 uF cap in parallel: it holds the node
    // steady for the ADC's sampling instant, and would make the divider slow to
    // settle (tau ~ 18 ms) if that rail were ever switched rather than fixed.
    // Reference part: NCP15WF104F03RC
    static constexpr float    PD_NTC_R_FIXED            = 22000.0f;
    static constexpr float    PD_NTC_R0                 = 100000.0f;
    static constexpr float    PD_NTC_BETA               = 4250.0f;

    // Battery voltage divider resistors.
    static constexpr float    BATT_R_TOP                = 100000.0f;
    static constexpr float    BATT_R_BOTTOM             = 100000.0f;

    // Scheduling cadence.
    static constexpr uint32_t ACTIVE_WINDOW_MS          =  30000;   // 30 s
    static constexpr uint32_t SENSOR_ACTIVE_CADENCE_MS  =   2000;   //  2 s
    static constexpr uint32_t SENSOR_STANDBY_CADENCE_MS = 600000;   // 10 min
}

#endif // CONFIG_H
