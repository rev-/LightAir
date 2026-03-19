#include <LightAir.h>
#include "GameTypeIds.h"

// ================================================================
// Free For All — every player shines every other player.
//
// States
//   IN_GAME  (0) : player is active; can shine; display shows lives/energy/time/points.
//   OUT_GAME (1) : player is out; waits for auto-respawn; display shows time.
//   GAME_END (2) : game over; display shows final time/points/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT  (0x10) : unicast to the player who was optically detected.
//   MSG_LIT reply (0x11) : sent back by the target; payload[0] = reply sub-type.
//
// Reply sub-types (payload[0] of the 0x11 reply)
//   REPLY_TAKEN (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE (2) : target was eliminated; lives reached 0.
//   REPLY_DOWN  (3) : target was already OUT_GAME; hit ignored.
//
// Config vars
//   startLives   : lives at game start (default 3).
//   respawnSecs  : seconds until auto-respawn (default 30).
//   startEnergy  : energy at game start / after respawn (default 50).
//   rechargeSecs : seconds after trigger release before full energy is restored (default 10).
//   gameTime     : total game duration in seconds (default 900).
//
// Shine flow (sender side)
//   1. TRIG_1 PRESSED/HELD → enlight.run(REPS)  (non-blocking, ~REPS*8 ms)
//   2. enlight.poll() → PLAYER_HIT → radio.sendTo(target, MSG_LIT)
//      NO_HIT / LOW_POW: missed shot, no radio message.
//   3. Receiving REPLY_SHONE → points++ + UI "lit".
//      Receiving REPLY_TAKEN → UI "lit" only.
//
// Shone flow (receiver side)
//   Receiving MSG_LIT:
//     lives > 1 → lives--, reply REPLY_TAKEN.
//     lives <= 1 → lives--, reply REPLY_SHONE → state machine → OUT_GAME.
//   OUT_GAME → reply REPLY_DOWN, no effect.
//   Respawn: automatic after respawnSecs seconds → transition back to IN_GAME.
//
// Game-end flow
//   gameTimeLeft reaches 0 from either IN_GAME or OUT_GAME → GAME_END.
// ================================================================

// Enlight is constructed in setup() after NVS calib is loaded.
// The sketch exposes it as a global pointer; game files use the pointer.
extern Enlight* enlightPtr;

namespace FFA {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types and reply sub-types ----
enum Msg         : uint8_t { MSG_LIT = 0x10, MSG_SCORE_COLLECT = 0x12 };
enum ReplySubType: uint8_t { REPLY_TAKEN = 1, REPLY_SHONE = 2, REPLY_DOWN = 3 };

// ---- Config variables ----
// default value, if not changed in startup menu
static int startLives   = 3;     // lives at start / after respawn
static int respawnSecs  = 30;    // seconds until auto-respawn
static int startEnergy  = 50;    // energy at start / after respawn
static int rechargeSecs = 10;    // secs after trigger release before energy restored
static int gameTime     = 900;   // total game duration in seconds

// ---- Runtime variables ----
static int lives        = 3;
static int energy       = 50;
static int gameTimeLeft = 900;  // live countdown (seconds)
static int points       = 0;
static int energySpent  = 0;
static int shoneTimes   = 0;

static uint8_t   gState;
static uint32_t  respawnAt;   // millis() when respawn fires
static uint32_t  lastTickAt;  // millis() of last per-second decrement
static bool      triggerWasActive = false;
static uint32_t  releaseAt        = 0;

// ---- Config vars (startup menu) ----
// all vars must be int, 
static const ConfigVar configVars[] = {
    //name         value          min    max    step
    { "Lives",    &startLives,   1,    5,      1 },
    { "Respawn",  &respawnSecs,  5,   120,      5 },
    { "Energy",   &startEnergy, 10,   100,     10 },
    { "Recharge", &rechargeSecs, 0,    20,      5 },
    { "Time",     &gameTime,    60,   900,     60 },
};

// ---- Monitor vars (LCD display during play) ----
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::Int("Lives",    &lives,        1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy",   &energy,       1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",     &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Points",   &points,       1u<<IN_GAME,                  ICON_SCORE,  1, 1),
    // GAME_END display (gameTime shared with configVars)
    MonitorVar::Int("Time",     &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points",   &points,       1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy",   &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",    &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- DirectRadioRules — incoming message handlers ----

static bool litAndTaken (const RadioPacket&) { return lives > 1;  }
static bool litAndShone (const RadioPacket&) { return lives <= 1; }

static void onLitTaken(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }
static void onLitShone(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType   condition      replySubType  onReceive
    { IN_GAME,  MSG_LIT, litAndTaken,  REPLY_TAKEN, onLitTaken },
    { IN_GAME,  MSG_LIT, litAndShone,  REPLY_SHONE, onLitShone },
    { OUT_GAME, MSG_LIT, nullptr,      REPLY_DOWN,  nullptr    },
};

// ---- ReplyRadioRules — reply and timeout handlers ----

static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    points++;
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType       condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN, nullptr, onReplyTaken },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE, nullptr, onReplyShone },
};

// ---- Winner election rules ----
static const WinnerVar winnerVars[] = {
    { &points,     WinnerDir::MAX },  // primary: most points wins
    { &shoneTimes, WinnerDir::MIN },  // tie-break: fewest times shone
};

// ---- onBegin: reset all runtime state from config ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*) {
    lives        = startLives;
    energy       = startEnergy;
    gameTimeLeft = gameTime;
    points       = 0;
    energySpent  = 0;
    shoneTimes       = 0;
    lastTickAt       = millis();
    triggerWasActive = false;
    releaseAt        = 0;
}

// ---- Shared per-second ticker (call from every active-state behavior) ----
static void tickGameTime() {
    uint32_t now = millis();
    if (now - lastTickAt >= 1000) {
        lastTickAt += 1000;          // step forward to avoid drift
        if (gameTimeLeft > 0) gameTimeLeft--;
    }
}

// ---- Transition conditions ----

static bool gameTimeExpired(const InputReport&, const RadioReport&) {
    return gameTimeLeft <= 0;
}

static bool shone(const InputReport&, const RadioReport&) {
    return lives <= 0;
}

static bool readyToRespawn(const InputReport&, const RadioReport&) {
    return millis() >= respawnAt;
}

// ---- Transition actions ----

static void onShone(LightAir_DisplayCtrl& disp, GameOutput& out) {
    shoneTimes++;
    respawnAt = millis() + (uint32_t)respawnSecs * 1000;
    disp.showMessage("Shone!", 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
}

static void onRespawn(LightAir_DisplayCtrl& disp, GameOutput& out) {
    lives  = startLives;
    energy = startEnergy;
    disp.showMessage("Back in game!", 1000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Up);
}

static void onGameEnd(LightAir_DisplayCtrl& disp, GameOutput& out) {
    disp.showMessage("Game over!", 3000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::EndGame);
}

// ---- State machine (first matching rule wins) ----

static const StateRule rules[] = {
    { IN_GAME,  gameTimeExpired, GAME_END, onGameEnd  },  // time check before shone
    { IN_GAME,  shone,           OUT_GAME, onShone    },
    { OUT_GAME, gameTimeExpired, GAME_END, onGameEnd  },
    { OUT_GAME, readyToRespawn,  IN_GAME,  onRespawn  },
};

// ---- Per-state behaviors ----

static void doInGame(const InputReport& inp, const RadioReport&,
                     LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

    constexpr uint8_t REPS = 4;
    bool triggerActive = false;

    for (uint8_t i = 0; i < inp.buttonCount; i++) {
        if (inp.buttons[i].id != InputDefaults::TRIG_1_ID) continue;
        ButtonState s = inp.buttons[i].state;
        if (s == ButtonState::PRESSED || s == ButtonState::HELD) {
            triggerActive = true;
            if (energy > 0) {
                energy--;
                energySpent++;
                enlightPtr->run(REPS);
                out.ui.triggerEnlight(REPS * EnlightDefaults::MS_PER_REP);
            }
        }
    }

    // Detect release edge → start rechargeSecs cooldown.
    if (triggerWasActive && !triggerActive)
        releaseAt = millis();
    triggerWasActive = triggerActive;

    // Restore full energy once cooldown has elapsed.
    if (!triggerActive && energy < startEnergy) {
        if (rechargeSecs == 0)
            energy = startEnergy;
        else if ((millis() - releaseAt)/1000 >= (uint32_t)rechargeSecs)
            energy = startEnergy;
    }

    // Poll Enlight; a confirmed hit sends MSG_LIT to the target.
    // points++ is deferred to onReplyShone when the target confirms elimination.
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT)
        out.radio.sendTo(r.id, MSG_LIT);
    // NO_HIT / LOW_POW: missed shot — no radio message.
}

static void doOutGame(const InputReport&, const RadioReport&,
                      LightAir_DisplayCtrl&, GameOutput&) {
    tickGameTime();   // keep countdown running while waiting to respawn
}

static const StateBehavior behaviors[] = {
    { IN_GAME,  doInGame  },
    { OUT_GAME, doOutGame },
    { GAME_END, nullptr   },   // static display — no per-cycle logic needed
};

} // namespace FFA

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
const LightAir_Game game_ffa = {
    /* typeId                */ GameTypeId::FREE_FOR_ALL,
    /* name                  */ "Free for All",
    /* configVars            */ FFA::configVars,         /* configCount            */ 5,
    /* monitorVars           */ FFA::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ FFA::directRadioRules,   /* directRadioRuleCount   */ 3,
    /* replyRadioRules       */ FFA::replyRadioRules,    /* replyRadioRuleCount    */ 2,
    /* rules                 */ FFA::rules,              /* ruleCount              */ 4,
    /* behaviors             */ FFA::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &FFA::gState,            /* initialState           */ FFA::IN_GAME,
    /* onBegin               */ FFA::onBegin,
    /* winnerVars            */ FFA::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ FFA::GAME_END,
    /* scoreMsgType          */ FFA::MSG_SCORE_COLLECT,
    /* totemVars             */ nullptr, // FFA uses no named totem roles
    /* totemVarCount         */ 0,
    /* hasTeams              */ false,
    /* teamBitmask           */ nullptr,
    /* onScoreAnnounce       */ nullptr, // use default individual ranking
    /* totemRunner           */ nullptr, // FFA has no totem-side behaviour
};
