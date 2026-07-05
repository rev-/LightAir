#pragma once
#include <stdint.h>
#include "../config.h"
#include "../input/LightAir_InputTypes.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_Display_Icons.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "LightAir_GameOutput.h"

// ================================================================
// LightAir_Game — the complete descriptor of a table-driven game,
// together with every row type its tables are made of.
//
// This one header is the whole contract between a game and the
// runtime: GameRunner executes the descriptor, GameSetupMenu edits
// its ConfigVars and serializes them into the config blob, and the
// LCD binds to its MonitorVars.
//
// Games are not written in C++: every ruleset is a .lua file (see
// games/*.lua and docs/lua-games-design.md).  LightAir_LuaGame loads
// a file and synthesizes this descriptor from it — the ConfigVar /
// MonitorVar / WinnerVar tables point into its variable slots and the
// callbacks are trampolines into the game's Lua handlers.  GameRunner
// consumes the descriptor without knowing Lua exists.
//
// Layout of this file (row types in descriptor order):
//   §1  ConfigVar / MonitorVar        — menu-edited + LCD-bound ints
//   §2  DirectRadioRule               — incoming request handlers
//   §3  ReplyRadioRule                — reply / timeout handlers
//   §4  StateRule                     — state transitions
//   §5  StateBehavior                 — per-state tick bodies
//   §6  WinnerVar / ScoreTable        — end-game winner election
//   §7  TotemRequirement / -Program   — totem roles + TotemVM bytes
//   §8  MenuResult                    — pre-game menu result
//   §9  struct LightAir_Game          — the descriptor itself
//
// All callbacks are plain function pointers (no captures); stateless
// lambdas decay to them automatically.
// ================================================================

// Forward declaration: LightAir_GameRunner is defined in LightAir_GameRunner.h,
// which includes this header.  The forward decl breaks the cycle; the full
// type is only needed in function pointer signatures and .cpp implementations.
class LightAir_GameRunner;

/* ================================================================
 * §1  Variables: ConfigVar / MonitorVar
 * ================================================================ */

// ----------------------------------------------------------------
// VarType — discriminates the two kinds of monitored variable.
//
//   INT   : int* (32-bit signed on ESP32-S3).
//           Compatible with DisplayCtrl::bindIntVariable.
//
//   CHARS : char* (mutable null-terminated buffer).
//           Compatible with DisplayCtrl::bindStringVariable.
// ----------------------------------------------------------------
enum class VarType : uint8_t { INT, CHARS };

// ----------------------------------------------------------------
// ConfigVar — one variable shown and edited in the pre-game config menu.
//
// All ConfigVars are integer values.  The menu lets the player
// adjust the value in increments of step within [min, max].
// step = 0 is treated as 1 by the config menu.
// ----------------------------------------------------------------
struct ConfigVar {
    const char* name;   // ≤12 chars; shown in config menu
    int*        value;
    int         min;
    int         max;
    int         step;   // 0 treated as 1
};

// ----------------------------------------------------------------
// MonitorVar — one variable displayed on the LCD during the game.
//
// stateMask encodes which states show this variable: bit N means
// "display in state N".  GameRunner::begin() reads this mask to
// create the necessary DisplayCtrl binding sets automatically.
//
// Construction: use the static factory helpers to avoid ambiguity
// between the int and chars forms:
//
//     MonitorVar::Int("Lives", &lives, 1u<<IN_GAME, ICON_LIFE, 0, 0)
//     MonitorVar::Str("Role",  role,   1u<<IN_GAME, ICON_ROLE, 1, 0)
// ----------------------------------------------------------------
struct MonitorVar {
    const char* name;
    VarType     type;
    int*        asInt;      // non-null when type == INT
    char*       asChars;    // non-null when type == CHARS
    uint32_t    stateMask;  // bit N → display in state N
    IconType    icon;
    uint8_t     col, row;

    // ---- factory helpers ----

    static MonitorVar Int(const char* name, int* value,
                          uint32_t stateMask, IconType icon,
                          uint8_t col, uint8_t row) {
        MonitorVar v = {};
        v.name      = name;
        v.type      = VarType::INT;
        v.asInt     = value;
        v.asChars   = nullptr;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;  v.row = row;
        return v;
    }

    static MonitorVar Str(const char* name, char* buf,
                          uint32_t stateMask, IconType icon,
                          uint8_t col, uint8_t row) {
        MonitorVar v = {};
        v.name      = name;
        v.type      = VarType::CHARS;
        v.asInt     = nullptr;
        v.asChars   = buf;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;  v.row = row;
        return v;
    }
};

/* ================================================================
 * §2  DirectRadioRule — incoming request handlers
 * ================================================================ */

// ----------------------------------------------------------------
// DirectRadioRule — table-driven handler for incoming radio messages.
//
// GameRunner evaluates these rules before StateRules each cycle,
// once per MessageReceived event.  First matching rule fires;
// remaining rules are skipped for that event.
//
// If no rule matches an incoming message the runner sends the
// standard reply (msgType + 1, empty payload) so the sender does
// not time out needlessly.
//
// Fields:
//   fromState    — state in which this rule is active.
//
//   msgType      — even msgType of the incoming request to match.
//
//   condition    — optional extra guard on the packet content or
//                  game state.  nullptr = always matches.
//
//   replySubType — value placed in payload[0] of the reply.
//                  0 = no payload (standard empty reply).
//                  Non-zero values distinguish reply semantics on
//                  the sender side via ReplyRadioRule.
//
//   onReceive    — optional side-effect (modify game state, queue
//                  UI events, etc.).  Called before the reply is
//                  queued.  nullptr = no effect.
// ----------------------------------------------------------------
struct DirectRadioRule {
    // Sentinel for replySubType: the rule's onReceive callback queues its
    // own reply (with a runtime-decided sub-type); GameRunner must NOT send
    // the automatic reply.  Used by the Lua binding, whose handlers return
    // the sub-type dynamically.
    static constexpr uint8_t DYNAMIC_REPLY = 0xFF;

    uint8_t fromState;    // state in which this rule is active
    uint8_t msgType;      // incoming even msgType to match

    bool (*condition)(const RadioPacket& pkt); // nullptr = always true

    uint8_t replySubType; // payload[0] of reply; 0 = no payload

    // Called when the rule fires, before the reply is queued.
    void (*onReceive)(const RadioPacket& pkt,
                      LightAir_DisplayCtrl&, GameOutput&); // nullptr = no effect
};

/* ================================================================
 * §3  ReplyRadioRule — reply / timeout handlers
 * ================================================================ */

// ----------------------------------------------------------------
// ReplyRadioRule — table-driven handler for radio replies and timeouts.
//
// GameRunner evaluates these rules after DirectRadioRules and before
// StateRules each cycle, once per ReplyReceived or Timeout event.
// First matching rule fires; remaining rules are skipped for that event.
//
// Fields:
//   activeInStateMask — bitmask of states in which this rule is active.
//                       Bit N = process in state N.
//                       e.g. (1u<<IN_GAME)|(1u<<OUT_GAME) to skip GAME_END.
//
//   eventType         — RadioEventType::ReplyReceived or ::Timeout.
//                       For Timeout events, the `reply` packet passed
//                       to callbacks is zeroed; only `original` is valid.
//
//   replySubType      — payload[0] value to match in a ReplyReceived
//                       packet.  0 = match any sub-type.
//                       Ignored for Timeout events.
//
//   condition         — optional extra guard on the packets or game state.
//                       nullptr = always matches.
//
//   onReply           — side-effect callback.  For ReplyReceived, both
//                       packets are valid.  For Timeout, `reply` is zeroed.
//                       nullptr = no effect.
// ----------------------------------------------------------------
struct ReplyRadioRule {
    uint32_t       activeInStateMask; // bit N = active in state N

    RadioEventType eventType;         // ReplyReceived or Timeout

    uint8_t        replySubType;      // payload[0] to match; 0 = any (ReplyReceived only)

    bool (*condition)(const RadioPacket& reply,
                      const RadioPacket& original); // nullptr = always true

    // For Timeout events, `reply` is zeroed; only `original` is valid.
    void (*onReply)(const RadioPacket& reply,
                    const RadioPacket& original,
                    LightAir_DisplayCtrl&, GameOutput&); // nullptr = no effect
};

/* ================================================================
 * §4  StateRule — state transitions
 * ================================================================ */

// ----------------------------------------------------------------
// StateRule — one row in a game's state-transition table.
//
// GameRunner evaluates rules in order for the current state.
// The first rule whose condition returns true fires:
//   1. currentState is set to toState.
//   2. GameRunner activates the new state's display binding set.
//   3. onTransition is called (if non-null).
// Only one rule fires per loop cycle.
//
// condition    : nullptr means the rule fires unconditionally.
//
// onTransition : called after the display binding set is switched.
//                Queue radio messages via out.radio and UI events
//                via out.ui.  nullptr = no extra action.
// ----------------------------------------------------------------
struct StateRule {
    uint8_t  fromState;

    bool   (*condition)(const InputReport&, const RadioReport&);

    uint8_t  toState;

    // Called after display is switched; nullptr = skip.
    void   (*onTransition)(LightAir_DisplayCtrl&, GameOutput&);
};

/* ================================================================
 * §5  StateBehavior — per-state tick bodies
 * ================================================================ */

// ----------------------------------------------------------------
// StateBehavior — per-state loop body, called every cycle while
// the game is in `state`.
//
// onUpdate runs AFTER transition rules have been evaluated and
// the display binding set has been switched.  It always sees the
// state the game settled into during this cycle.
//
// Use onUpdate to:
//   - React to trigger buttons (e.g. call Enlight::run())
//   - Handle radio messages (e.g. incoming lit notifications)
//   - Queue radio messages    via out.radio
//   - Queue UI events         via out.ui
//   - Update display tray     via disp.showMessage(...)
//   - Modify game variables   (lives, energy, score...)
//
// nullptr = no per-cycle action for this state.
// ----------------------------------------------------------------
struct StateBehavior {
    uint8_t  state;

    void   (*onUpdate)(const InputReport&, const RadioReport&,
                       LightAir_DisplayCtrl&, GameOutput&);
};

/* ================================================================
 * §6  WinnerVar / ScoreTable — end-game winner election
 * ================================================================ */

// ----------------------------------------------------------------
// WinnerVar — one variable that participates in winner election.
//
// A game defines a winnerVars[] table in its descriptor to specify
// which runtime variables determine the winner and how they compare.
// GameRunner reads this table at scoringState entry to:
//   1. Collect each player's values into the broadcast payload.
//   2. Compare collected slots to elect the winner.
//
// Priority is determined by position in the array:
//   index 0 = primary criterion
//   index 1 = first tie-breaker (used only when index 0 is equal)
//   index 2 = second tie-breaker, etc.
// ----------------------------------------------------------------

enum class WinnerDir : uint8_t {
    MAX,  // higher value wins (e.g. points scored)
    MIN,  // lower value wins  (e.g. times shone — fewer is better)
};

struct WinnerVar {
    int*      value;  // pointer to the game-namespace runtime variable to sample
    WinnerDir dir;    // comparison direction
};

// ----------------------------------------------------------------
// ScoreTable — snapshot of collected end-game scores passed to
// an optional onScoreAnnounce callback.  Allows rulesets to
// perform custom winner computation (e.g. team aggregation)
// instead of the default individual-player ranking.
//
// accumMask     — bit id = slots[id] is valid (player-ID-indexed, ids 1–16)
// slots[id]     — winnerVarCount × int32_t LE for player id
// teamMap[id]   — team index (0–7) or 0xFF (teamless) for player id (size MAX_PLAYER_ID)
// myPlayerId    — this device's logical player ID
// ----------------------------------------------------------------
struct ScoreTable {
    uint32_t         accumMask;          // bit id set = slots[id] valid
    const uint8_t  (*slots)[GameDefaults::MAX_WINNER_VARS * 4];  // indexed by player ID
    uint8_t          winnerVarCount;
    const WinnerVar* winnerVars;
    const uint8_t*   teamMap;    // size PlayerDefs::MAX_PLAYER_ID
    uint8_t          myPlayerId;
};

/* ================================================================
 * §7  Totem roles: TotemRequirement / TotemProgramEntry
 * ================================================================ */

// ----------------------------------------------------------------
// LightAir_TotemRequirement — describes one totem role that a game
// uses, including how many totems of that role are needed.
//
// roleId      — TotemRoleId constant (BASE_O, FLAG_X, CP, BONUS, …)
// minCount    — minimum totems with this role required to start; 0 = optional
// maxCount    — maximum totems that can be assigned this role
// configSecs  — optional pointer to a game config int (seconds).
//               When non-null, its current value is patched into the
//               role's TotemVM program ({"cfg"} sites) as the program
//               is serialized into the 0xF1 activation reply, so a
//               game-configured cooldown reaches the totem.
//               nullptr = the program's own cfg_default applies.
// ----------------------------------------------------------------
struct LightAir_TotemRequirement {
    uint8_t     roleId;
    uint8_t     minCount;
    uint8_t     maxCount;
    const int*  configSecs;   // optional; points to a game config var
};

// ----------------------------------------------------------------
// TotemProgramEntry — one serialized TotemVM program for one totem
// role of a game (see docs/totem-behavior-handshake.md).
//
// The bytes travel inside the 0xF1 activation reply:
//   [0] roleId  [1] sessionToken  [2:3] gameTimeLeft
//   [4] vmVersion  [5:6] progLen (u16 LE)  [7..] program
//
// A game exposes its programs through LightAir_Game::totemProgram —
// a provider function rather than a static table because the Lua
// binding patches config-derived immediates ({"cfg"} seconds) into
// the bytes with the *current* config value at reply time.
// ----------------------------------------------------------------
struct TotemProgramEntry {
    uint8_t        roleId;   // TotemRoleId constant
    uint8_t        len;      // program bytes (≤ TotemVMDefs::MAX_PROG)
    const uint8_t* bytes;
};

/* ================================================================
 * §8  MenuResult
 * ================================================================ */

// Returned by blocking pre-game menu classes.
enum class MenuResult : uint8_t { Confirmed, Cancelled };

/* ================================================================
 * §9  LightAir_Game — the descriptor
 * ================================================================ */

// ----------------------------------------------------------------
// LightAir_Game — complete descriptor of a table-driven game.
//
// A concrete game is defined by filling in this struct and
// registering it with LightAir_GameManager (in practice:
// LightAir_LuaGame fills it from a .lua file and LightAir_GameStore
// registers it).
//
// Fields:
//   typeId         — unique 16-bit game identifier.  Written as
//                    the first 2 bytes of config blobs so receivers
//                    can verify compatibility before applying.
//
//   name           — short display name (≤15 chars) shown in the
//                    game-selection menu.
//
//   configVars / configCount — variables shown and edited in the
//                    pre-game config menu (integers with min/max/step).
//
//   monitorVars / monitorCount — variables auto-bound to the LCD by
//                    GameRunner::begin() based on each var's stateMask.
//
//   directRadioRules / directRadioRuleCount — incoming message handlers.
//                    Evaluated before StateRules.  First match per event
//                    sends a reply and runs the action.  Unmatched messages
//                    receive a standard empty reply automatically.
//
//   replyRadioRules / replyRadioRuleCount — reply and timeout handlers.
//                    Evaluated after DirectRadioRules, before StateRules.
//                    First match per event runs the action.
//
//   rules / ruleCount — state-transition table, evaluated in order.
//                    First matching rule fires per cycle.
//
//   behaviors / behaviorCount — per-state loop bodies.  Only the
//                    entry whose `state` matches currentState is
//                    called each cycle.
//
//   currentState   — pointer to the uint8_t that holds the running
//                    state index.  GameRunner writes to it on every
//                    transition.
//
//   initialState   — value written to *currentState on begin().
//
//   onBegin        — optional callback invoked by GameRunner::begin()
//                    after display binding sets are created.
//                    Use it for radio setup, initial messages, etc.
//                    nullptr = skip.
// ----------------------------------------------------------------
struct LightAir_Game {
    uint16_t             typeId;
    const char*          name;

    const ConfigVar*     configVars;
    uint8_t              configCount;

    const MonitorVar*    monitorVars;
    uint8_t              monitorCount;

    const DirectRadioRule* directRadioRules;
    uint8_t                directRadioRuleCount;

    const ReplyRadioRule*  replyRadioRules;
    uint8_t                replyRadioRuleCount;

    const StateRule*     rules;
    uint8_t              ruleCount;

    const StateBehavior* behaviors;
    uint8_t              behaviorCount;

    uint8_t*             currentState;
    uint8_t              initialState;

    // Called by GameRunner::begin() after display binding sets are built.
    // ui is the optional UICtrl pointer passed to GameRunner::begin(); may be nullptr.
    // runner is the GameRunner instance; use runner.totemIdForRole() to populate
    // local totem-ID caches.
    // nullptr = skip.
    void (*onBegin)(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*,
                    const LightAir_GameRunner&);

    // ---- End-game score collection and winner election (optional) ----
    //
    // When the game enters scoringState, every player broadcasts its own
    // scores; each device accumulates received data and computes the winner
    // locally once all expected scores arrive.  GameRunner manages the roster,
    // accumulation, fusion re-broadcast, and winner display entirely.
    //
    // Set winnerVars = nullptr or scoringState = 255 to disable.
    //
    // Slot size is winnerVarCount × 4 bytes (one int32_t per variable).
    // Constraint: 4 + rosterCount × slotSize ≤ RADIO_MAX_PAYLOAD (239).
    //
    // winnerVars[] priority order: index 0 = primary, index 1 = tie-breaker…
    // Use PlayerDefs::playerShort[roster[i]] for player names in messages.

    const WinnerVar* winnerVars;      // ordered scoring rules; nullptr = disabled
    uint8_t          winnerVarCount;
    uint8_t          scoringState;    // state that activates collection; 255 = disabled
    uint8_t          scoreMsgType;    // even msgType for the per-player score broadcast

    // Optional custom winner announcement.  If non-null, called instead of the
    // default individual-player ranking after all scores are collected.
    // Use for team-aggregate or other non-individual winner logic.
    // nullptr = use default ranking (scoreSlotBeats / scoreAnnounce).
    void (*onScoreAnnounce)(const ScoreTable&, LightAir_DisplayCtrl&);

    // ---- Totem roles and team configuration ----
    //
    // totemRequirements[] lists the totem roles this game supports
    // (e.g. BASE_O, FLAG_X, BONUS) together with min/max counts.
    // The host assigns specific totem device IDs to each role in
    // LightAir_GameSetupMenu (S4c).
    //
    // teamCount > 0 enables the Teams submenu (S4b) where the host assigns each
    // player to one of teamCount teams (indices 0..teamCount-1).
    // teamMap points to a uint8_t[MAX_PLAYER_ID] owned by the game;
    // entry i holds the team index (0–7) or 0xFF if unassigned.
    // teamCount == 0 means the game is teamless; teamMap may be nullptr.
    // Maximum supported value: TeamColors::kCount (8).
    //
    const LightAir_TotemRequirement* totemRequirements;   // nullptr = none
    uint8_t                          totemRequirementCount;
    uint8_t                          teamCount;   // 0 = teamless; 2–8 = number of teams
    uint8_t*                         teamMap;     // size MAX_PLAYER_ID; nullptr if teamCount==0

    // Optional pointer to the ruleset's live in-game countdown (seconds remaining),
    // e.g. &gameTimeLeft.  Sent to totems in the 0xF1 activation reply so they can
    // arm a self-revert watchdog.  nullptr = ruleset has no such counter.
    const int* gameTimeLeft;

    // Called by GameRunner immediately before esp_restart() after the player
    // presses A+B on the end-game screen.  Use for last-moment display updates
    // or NVS writes.  nullptr = skip.
    void (*onEnd)(LightAir_DisplayCtrl&);

    // ---- TotemVM programs (Lua-defined games) ----
    //
    // Returns the serialized TotemVM program for a role, or nullptr if the
    // game defines none for it.  When non-null for a beaconing totem's
    // assigned role, GameRunner appends [vmVersion][progLen][program] to the
    // 0xF1 activation reply so the totem needs no game files at all.
    // A role without a program gets no 0xF1 reply and the totem stays
    // IDLE — the VM form is the only activation form (the pre-VM short
    // reply and the native totem role runners are retired).
    const TotemProgramEntry* (*totemProgram)(uint8_t roleId);
};
