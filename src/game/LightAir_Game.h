#pragma once
#include "LightAir_GameVar.h"
#include "LightAir_StateRule.h"
#include "LightAir_StateBehavior.h"
#include "LightAir_DirectRadioRule.h"
#include "LightAir_ReplyRadioRule.h"
#include "LightAir_WinnerVar.h"
#include "LightAir_TotemRequirement.h"
#include "LightAir_TotemProgram.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../config.h"

// Forward declaration: LightAir_GameRunner is defined in LightAir_GameRunner.h,
// which includes this header.  The forward decl breaks the cycle; the full
// type is only needed in function pointer signatures and .cpp implementations.
class LightAir_GameRunner;

// ----------------------------------------------------------------
// MenuResult — returned by blocking pre-game menu classes.
// ----------------------------------------------------------------
enum class MenuResult : uint8_t { Confirmed, Cancelled };

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

// ----------------------------------------------------------------
// LightAir_Game — complete descriptor of a table-driven game.
//
// A concrete game is defined by filling in this struct and
// registering it with LightAir_GameManager.  No C++ class or
// inheritance is required.
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
//   currentState   — pointer to a file-scope uint8_t that holds
//                    the running state index.  GameRunner writes
//                    to it on every transition.
//
//   initialState   — value written to *currentState on begin().
//
//   onBegin        — optional callback invoked by GameRunner::begin()
//                    after display binding sets are created.
//                    Use it for radio setup, initial messages, etc.
//                    nullptr = skip.
//
// ----------------------------------------------------------------
// Games are not written in C++: every ruleset is a .lua file (see
// games/*.lua and docs/lua-games-design.md).  LightAir_LuaGame loads
// a file and synthesizes this descriptor from it — the ConfigVar /
// MonitorVar / WinnerVar tables point into its variable slots and the
// callbacks are trampolines into the game's Lua handlers.  GameRunner
// consumes the descriptor without knowing Lua exists.
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
    // teamMap points to a file-scope uint8_t[MAX_PLAYER_ID] in the ruleset;
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
