#include <LightAir.h>
#include "GameTypeIds.h"

// ================================================================
// Outflow — energy-only FFA; no lives.
//
// States
//   IN_GAME  (0) : player is active; can project light; display: energy/points/time/shoneTimes.
//   OUT_GAME (1) : player is down; waits for timed respawn; display: time.
//   GAME_END (2) : game over; display: time/points/energySpent/depletions.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT          (0x10) : unicast to the player optically detected.
//   MSG_LIT reply    (0x11) : sent back by the target; payload[0] = reply sub-type.
//   MSG_SCORE_COLLECT (0x12) : broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x21 reply)
//   REPLY_TAKEN (1) : target absorbed the hit; energy > 0 after decrement.
//   REPLY_SHONE (2) : target was eliminated; energy reached 0.
//   REPLY_DOWN  (3) : target was already OUT_GAME; hit ignored.
//
// Energy mechanics
//   Energy is simultaneously weapon ammo and life total.
//   Projecting light costs 1 energy per trigger event (no recharge).
//   A hit reduces energy by hitDmg (clamped to 0).
//   Passive drain reduces energy by 1 every (10000/drainRate) ms.
//   If a hit or drain reduces energy to 0 the player goes OUT_GAME.
//   On eliminating another player, the shooter gains startEnergy (uncapped).
//
// Elimination causes
//   pendingShone      : set by DirectRadioRule when a fatal hit arrives.
//   pendingDepletion  : set by tickDrain() when passive drain zeros energy.
//   The two are mutually exclusive: DirectRadioRules run before behaviors,
//   so a fatal hit zeroes energy first; tickDrain's guard (energy > 0) then
//   prevents a simultaneous pendingDepletion flag.
//
// Scoring
//   Points start at 100 for all players.
//   Each self-depletion (passive drain OUT_GAME) costs -1 point.
//   Eliminating another player grants no points directly; the shooter gains
//   energy instead, which is the primary reward.
//   Winner: most points; tie-break: fewest shoneTimes.
//
// Config vars
//   startEnergy  : energy at game start / after respawn (default 100).
//   hitDmg       : energy lost per hit received (default 50, clamp to 0).
//   drainRate    : energy points drained per 10 seconds (default 10 → 1/s).
//   respawnSecs  : seconds until auto-respawn (default 30).
//   gameTime     : total game duration in seconds (default 900).
// ================================================================

extern Enlight* enlightPtr;

namespace Outflow {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types ----
using RadioMsg::MSG_LIT;           // 0x10
using RadioMsg::MSG_SCORE_COLLECT; // 0x12

// ---- Reply sub-types ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN = 1,
    REPLY_SHONE = 2,
    REPLY_DOWN  = 3,
};

// ---- Config variables ----
static int startEnergy = 100;
static int hitDmg      = 50;
static int drainRate   = 10;   // energy points per 10 s; drainIntervalMs = 10000/drainRate
static int respawnSecs = 30;
static int gameTime    = 900;

// ---- Runtime variables ----
static int energy       = 100;
static int gameTimeLeft = 900;
static int points       = 100;  // starts at 100; drops on self-depletion
static int shoneTimes   = 0;
static int depletions   = 0;    // times this player drained to 0 naturally
static int energySpent  = 0;    // energy spent projecting light (not hits received)

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t lastDrainAt;
static uint32_t drainIntervalMs;
static uint32_t respawnAt;

static bool pendingShone;       // a fatal hit was received this cycle
static bool pendingDepletion;   // passive drain zeroed energy this cycle

// ---- Config vars (startup menu) ----
static const ConfigVar configVars[] = {
    //name           value           min   max   step
    { "Energy",     &energy,        50,   200,  25  },
    { "HitDmg",     &hitDmg,        25,   200,  25  },
    { "DrainRate",  &drainRate,      2,    20,   2  },
    { "Respawn",    &respawnSecs,    5,   120,   5  },
    { "Time",       &gameTime,      60,   900,  60  },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::Int("Energy",     &energy,       1u<<IN_GAME,                  ICON_ENERGY, 0, 0),
    MonitorVar::Int("Points",     &points,       1u<<IN_GAME,                  ICON_SCORE,  1, 0),
    MonitorVar::Int("Time",       &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Shone",      &shoneTimes,   1u<<IN_GAME,                  ICON_LIFE,   1, 1),
    // GAME_END display (gameTime shared with configVars)
    MonitorVar::Int("Time",       &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points",     &points,       1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy",     &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Depletions", &depletions,   1u<<GAME_END, ICON_DOWN,   1, 1),
};

// ---- DirectRadioRule conditions ----
static bool litAndTaken(const RadioPacket&) { return energy > hitDmg; }
static bool litAndShone(const RadioPacket&) { return energy <= hitDmg; }

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) {
    energy -= hitDmg;
    if (energy < 0) energy = 0;
}
static void onLitShone(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) {
    energy       = 0;
    pendingShone = true;
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType   condition    replySubType  onReceive
    { IN_GAME,  MSG_LIT, litAndTaken, REPLY_TAKEN, onLitTaken },
    { IN_GAME,  MSG_LIT, litAndShone, REPLY_SHONE, onLitShone },
    { OUT_GAME, MSG_LIT, nullptr,     REPLY_DOWN,  nullptr    },
};

// ---- ReplyRadioRule handlers ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    energy += startEnergy;   // uncapped reward for eliminating another player
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType       condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN, nullptr, onReplyTaken },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE, nullptr, onReplyShone },
};

// ---- Winner election rules ----
static const WinnerVar winnerVars[] = {
    { &points,    WinnerDir::MAX },  // primary: most points wins
    { &shoneTimes, WinnerDir::MIN }, // tie-break: fewest times shone
};

// ---- onBegin: reset all runtime state from config ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*,
                    const LightAir_GameRunner&) {
    energy          = startEnergy;
    gameTimeLeft    = gameTime;
    points          = 100;
    shoneTimes      = 0;
    depletions      = 0;
    energySpent     = 0;
    pendingShone     = false;
    pendingDepletion = false;
    lastTickAt      = millis();
    lastDrainAt     = millis();
    drainIntervalMs = (drainRate > 0) ? (10000u / (uint32_t)drainRate) : 1000u;
}

// ---- Shared per-second game-time ticker ----
static void tickGameTime() {
    uint32_t now = millis();
    if (now - lastTickAt >= 1000) {
        lastTickAt += 1000;
        if (gameTimeLeft > 0) gameTimeLeft--;
    }
}

// ---- Passive drain ticker (IN_GAME only) ----
// Drains 1 energy every drainIntervalMs ms.  Uses step-forward to avoid drift.
// Guard (energy > 0) keeps the two elimination paths mutually exclusive with
// pendingShone: a fatal hit zeroes energy before behaviors run.
static void tickDrain() {
    uint32_t now = millis();
    if (now - lastDrainAt >= drainIntervalMs) {
        lastDrainAt += drainIntervalMs;
        if (energy > 0) {
            energy--;
            if (energy == 0)
                pendingDepletion = true;
        }
    }
}

// ---- Transition conditions ----
static bool gameTimeExpired(const InputReport&, const RadioReport&) {
    return gameTimeLeft <= 0;
}
static bool wasShone(const InputReport&, const RadioReport&) {
    return pendingShone;
}
static bool wasDepleted(const InputReport&, const RadioReport&) {
    return pendingDepletion;
}
static bool readyToRespawn(const InputReport&, const RadioReport&) {
    return millis() >= respawnAt;
}

// ---- Transition actions ----
static void onShone(LightAir_DisplayCtrl& disp, GameOutput& out) {
    shoneTimes++;
    pendingShone = false;
    respawnAt    = millis() + (uint32_t)respawnSecs * 1000;
    disp.showMessage("Shone!", 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
}
static void onDepletion(LightAir_DisplayCtrl& disp, GameOutput& out) {
    depletions++;
    points--;
    pendingDepletion = false;
    respawnAt        = millis() + (uint32_t)respawnSecs * 1000;
    disp.showMessage("Drained out!", 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
}
static void onRespawn(LightAir_DisplayCtrl& disp, GameOutput& out) {
    energy = startEnergy;
    lastDrainAt = millis();
    disp.showMessage("Back in game!", 1000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Up);
}
static void onGameEnd(LightAir_DisplayCtrl& disp, GameOutput& out) {
    disp.showMessage("Game over!", 3000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::EndGame);
}

// ---- State machine (first matching rule wins) ----
static const StateRule rules[] = {
    { IN_GAME,  gameTimeExpired, GAME_END, onGameEnd   },
    { IN_GAME,  wasShone,        OUT_GAME, onShone     },
    { IN_GAME,  wasDepleted,     OUT_GAME, onDepletion },
    { OUT_GAME, gameTimeExpired, GAME_END, onGameEnd   },
    { OUT_GAME, readyToRespawn,  IN_GAME,  onRespawn   },
};

// ---- Per-state behaviors ----
static void doInGame(const InputReport& inp, const RadioReport&,
                     LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();
    tickDrain();

    constexpr uint8_t REPS = 10;

    for (uint8_t i = 0; i < inp.buttonCount; i++) {
        if (inp.buttons[i].id != InputDefaults::TRIG_1_ID) continue;
        ButtonState s = inp.buttons[i].state;
        if (s == ButtonState::PRESSED || s == ButtonState::HELD) {
            if (energy > 0) {
                energy--;
                energySpent++;
                enlightPtr->run(REPS);
                out.ui.triggerEnlight(REPS * EnlightDefaults::MS_PER_REP);
            }
        }
    }

    // Poll Enlight; a confirmed hit sends MSG_LIT to the target.
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT)
        out.radio.sendTo(r.id, MSG_LIT);

    // Set depletion flag if energy reached zero (from either active or passive drain),
    // unless a fatal hit already set pendingShone (mutually exclusive).
    if (energy == 0 && !pendingShone)
        pendingDepletion = true;
}

static void doOutGame(const InputReport&, const RadioReport&,
                      LightAir_DisplayCtrl&, GameOutput&) {
    tickGameTime();
}

static const StateBehavior behaviors[] = {
    { IN_GAME,  doInGame  },
    { OUT_GAME, doOutGame },
    { GAME_END, nullptr   },
};

// ---- Totem requirements (BONUS and MALUS are optional) ----
static const LightAir_TotemRequirement totemRequirements[] = {
    { TotemRoleId::BONUS, 0, GameDefaults::MAX_PARTICIPANTS, nullptr },
    { TotemRoleId::MALUS, 0, GameDefaults::MAX_PARTICIPANTS, nullptr },
};

} // namespace Outflow

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
extern const LightAir_Game game_outflow = {
    /* typeId                */ GameTypeId::OUTFLOW,
    /* name                  */ "Outflow",
    /* configVars            */ Outflow::configVars,         /* configCount            */ 5,
    /* monitorVars           */ Outflow::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ Outflow::directRadioRules,   /* directRadioRuleCount   */ 3,
    /* replyRadioRules       */ Outflow::replyRadioRules,    /* replyRadioRuleCount    */ 2,
    /* rules                 */ Outflow::rules,              /* ruleCount              */ 5,
    /* behaviors             */ Outflow::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &Outflow::gState,            /* initialState           */ Outflow::IN_GAME,
    /* onBegin               */ Outflow::onBegin,
    /* winnerVars            */ Outflow::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ Outflow::GAME_END,
    /* scoreMsgType          */ Outflow::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ nullptr,
    /* totemRequirements     */ Outflow::totemRequirements,  /* totemRequirementCount  */ 2,
    /* teamCount             */ 0,
    /* teamMap               */ nullptr,
    /* onEnd                 */ nullptr,
};
