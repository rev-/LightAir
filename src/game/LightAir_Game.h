#pragma once
#include "LightAir_GameVar.h"
#include "LightAir_StateRule.h"
#include "LightAir_StateBehavior.h"

// ----------------------------------------------------------------
// LightAir_Game — complete descriptor of a table-driven game.
//
// A concrete game is defined by filling in this struct and
// registering it with LightAir_GameManager.  No C++ class or
// inheritance is required.
//
// Fields:
//   typeId         — unique 32-bit game identifier.  Written as
//                    the first 4 bytes of config blobs so receivers
//                    can verify compatibility before applying.
//
//   name           — short display name (≤15 chars) shown in the
//                    game-selection menu.
//
//   vars / varCount — tracked game variables.  INT vars with
//                    isConfig=true appear in the config menu.
//                    Vars with stateMask != 0 are auto-bound to
//                    the display by GameRunner::begin().
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
// Minimal example — Free for All:
//
//   // --- rulesets/GameFreeForAll.cpp ---
//   #include <LightAir.h>
//
//   enum State : uint8_t { IN_GAME, OUT_GAME };
//   enum Msg   : uint8_t { MSG_HIT = 0x10 };
//
//   static int     lives = 3, score = 0;
//   static uint8_t gState;
//   extern Enlight enlight;   // defined in sketch
//
//   static GameVar vars[] = {
//       GameVar::Int("Lives", &lives, 1<<IN_GAME, ICON_LIFE,  0,0, true, 1,10,1),
//       GameVar::Int("Score", &score, 1<<IN_GAME, ICON_SCORE, 1,0),
//   };
//
//   static bool gotHit(const InputReport&, const RadioReport& r) {
//       for (uint8_t i = 0; i < r.count; i++)
//           if (r.events[i].type == RadioEventType::MessageReceived &&
//               r.events[i].packet.msgType == MSG_HIT) return true;
//       return false;
//   }
//   static const StateRule rules[] = {
//       { IN_GAME, gotHit,   OUT_GAME, nullptr },
//       { OUT_GAME, nullptr, IN_GAME,  nullptr },
//   };
//
//   static void doInGame(const InputReport& inp, const RadioReport&,
//                        LightAir_DisplayCtrl&, RadioOutput& out) {
//       for (uint8_t i = 0; i < inp.buttonCount; i++)
//           if (inp.buttons[i].id == InputDefaults::TRIG_1_ID &&
//               inp.buttons[i].state == ButtonState::RELEASED) {
//               enlight.run();
//               out.broadcast(MSG_HIT);
//           }
//   }
//   static const StateBehavior behaviors[] = {
//       { IN_GAME, doInGame }, { OUT_GAME, nullptr },
//   };
//
//   const LightAir_Game game_ffa = {
//       .typeId         = 0x00000001,
//       .name           = "Free for All",
//       .vars           = vars,      .varCount       = 2,
//       .rules          = rules,     .ruleCount      = 2,
//       .behaviors      = behaviors, .behaviorCount  = 2,
//       .currentState   = &gState,   .initialState   = IN_GAME,
//       .onBegin        = nullptr,
//   };
//
//   // --- rulesets/AllGames.cpp ---
//   extern const LightAir_Game game_ffa;
//   void registerAllGames(LightAir_GameManager& mgr) {
//       mgr.registerGame(game_ffa);
//   }
// ----------------------------------------------------------------
struct LightAir_Game {
    uint32_t             typeId;
    const char*          name;

    GameVar*             vars;
    uint8_t              varCount;

    const StateRule*     rules;
    uint8_t              ruleCount;

    const StateBehavior* behaviors;
    uint8_t              behaviorCount;

    uint8_t*             currentState;
    uint8_t              initialState;

    // Called by GameRunner::begin() after display binding sets are built.
    // nullptr = skip.
    void (*onBegin)(LightAir_DisplayCtrl&, LightAir_Radio&);
};
