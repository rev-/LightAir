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
//   Energy is simultaneously weapon ammo and life total.  The pool lives in
//   LightAir_ProjectorCtrl as projectorEnergy; this ruleset still owns what
//   reaching zero MEANS, which is why its projector is Recharge::NONE rather
//   than CONSUMED — the projector must survive zero so the player can respawn
//   holding it.
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
    { "Energy",     &startEnergy,   50,   200,  25  },
    { "HitDmg",     &hitDmg,        25,   200,  25  },
    { "DrainRate",  &drainRate,      2,    20,   2  },
    { "Respawn",    &respawnSecs,    5,   120,   5  },
    { "Time",       &gameTime,      60,   900,  60  },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::IntDyn("Energy",  &projectorEnergy, 1u<<IN_GAME, &projectorIcon, ICON_ENERGY, 0, 0),
    MonitorVar::Int("Points",     &points,       1u<<IN_GAME,                  ICON_SCORE,  1, 0),
    MonitorVar::Int("Time",       &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Shone",      &shoneTimes,   1u<<IN_GAME,                  ICON_LIFE,   1, 1),
    // GAME_END display (gameTime shared with configVars)
    MonitorVar::Int("Time",       &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points",     &points,       1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy",     &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Depletions", &depletions,   1u<<GAME_END, ICON_DOWN,   1, 1),
};

// ---- Incoming hit weight ----
// payload[0] is the attacker's projector strength in STANDARD HITS; one
// standard hit costs hitDmg energy here.  A packet with no payload comes from
// pre-projector firmware and counts as one standard hit.
static int litCost(const RadioPacket& pkt) {
    const int hits = pkt.payloadLen ? (int)pkt.payload[0] : 1;
    return hits * hitDmg;
}

// ---- DirectRadioRule conditions ----
static bool litAndTaken(const RadioPacket& pkt) { return projectorEnergy >  litCost(pkt); }
static bool litAndShone(const RadioPacket& pkt) { return projectorEnergy <= litCost(pkt); }

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket& reply, LightAir_DisplayCtrl& disp, GameOutput& out) {
    projectorEnergy -= litCost(reply);
    if (projectorEnergy < 0) projectorEnergy = 0;
    const char* name = (reply.senderId < PlayerDefs::MAX_PLAYER_ID)
                   ? PlayerDefs::playerShort[reply.senderId] : "???";
    char buf[20];
    snprintf(buf, sizeof(buf), "Lit by %s", name);
    disp.showMessage(buf, 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::GotLit);
}
static void onLitShone(const RadioPacket& reply, LightAir_DisplayCtrl& disp, GameOutput& out) {
    projectorEnergy = 0;
    pendingShone    = true;
    const char* name = (reply.senderId < PlayerDefs::MAX_PLAYER_ID)
           ? PlayerDefs::playerShort[reply.senderId] : "???";
    char buf[20];
    snprintf(buf, sizeof(buf), "Shone by %s", name);
    disp.showMessage(buf, 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
    
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
    out.ui.trigger(LightAir_UICtrl::UIEvent::Taken);
}

static void onReplyShone(const RadioPacket& reply, const RadioPacket&,
                         LightAir_DisplayCtrl& disp, GameOutput& out) {
    // Deliberately uncapped, as before: ProjectorCtrl clamps on refill only,
    // never on a direct write, so this mechanic survives the pool moving into
    // the framework.
    projectorEnergy += startEnergy;
    points          += 1;
    const char* name = (reply.senderId < PlayerDefs::MAX_PLAYER_ID)
               ? PlayerDefs::playerShort[reply.senderId] : "???";
    char buf[20];
    snprintf(buf, sizeof(buf), "%s LIT", name);
    disp.showMessage(buf, 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static void onReplyDown(const RadioPacket& reply, const RadioPacket&,
                         LightAir_DisplayCtrl& disp, GameOutput& out) {
    char buf[20];
    const char* name = (reply.senderId < PlayerDefs::MAX_PLAYER_ID)
                       ? PlayerDefs::playerShort[reply.senderId] : "???";
    snprintf(buf, sizeof(buf), "%s is OUT", name);
    disp.showMessage(buf, 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType       condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN, nullptr, onReplyTaken },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE, nullptr, onReplyShone },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_DOWN,  nullptr, onReplyDown  },
};

// ---- Winner election rules ----
static const WinnerVar winnerVars[] = {
    { &points,    WinnerDir::MAX },  // primary: most points wins
    { &shoneTimes, WinnerDir::MIN }, // tie-break: fewest times shone
};

// ---- onBegin: reset all runtime state from config ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl* ui,
                    const LightAir_GameRunner&) {
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

    // The projector carries cycles and cooldown now; only the DM-tunable pool
    // has to be stated here.  Recharge::NONE means the delay is never used.
    projector.setPool(startEnergy, 0);

    ui->trigger(LightAir_UICtrl::UIEvent::GameStart);
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
        if (projectorEnergy > 0) {
            projectorEnergy--;
            if (projectorEnergy <= 0)
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
static void onShone(LightAir_DisplayCtrl&, GameOutput&) {
    shoneTimes++;
    pendingShone = false;
    respawnAt    = millis() + (uint32_t)respawnSecs * 1000;
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
    projectorEnergy = startEnergy;
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

    // Poll Enlight; a confirmed hit sends MSG_LIT to the target, carrying this
    // projector's strength so the target can weigh it.  mayLight() is the
    // attacker-side anti-spam window (0 ms for this game, so always true).
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT && projector.mayLight(r.id)) {
        const Projector& p = projector.active();
        const uint8_t payload[3] = { p.strength, projector.activeId(), p.roleTag };
        out.radio.sendTo(r.id, MSG_LIT, payload, sizeof(payload));
        projector.noteLit(r.id);
    }

    for (uint8_t i = 0; i < inp.buttonCount; i++) {
        if (inp.buttons[i].id != InputDefaults::TRIG_1_ID) continue;
        ButtonState s = inp.buttons[i].state;
        if (s == ButtonState::PRESSED || s == ButtonState::HELD) {
            // trigger() folds the deploy-time check, the energy check,
            // Enlight::run() and the UI action into one call, and deducts
            // energy only when the run actually started.
            if (projector.trigger()) energySpent++;
        }
    }

    // Set depletion flag if energy reached zero (from either active or passive drain),
    // unless a fatal hit already set pendingShone (mutually exclusive).
    // <= 0 rather than == 0: a weighted hit can overshoot zero.
    if (projectorEnergy <= 0 && !pendingShone)
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

// ---- Projector ----
//
// Outflow has no powered projectors: it has one baseline whose values differ
// from the standard.  That is exactly what baseOverride is for — the profile
// keeps the BASE id and therefore all of BASE's structural behaviour
// (always held, never counted against maxOwned, never evicted, the
// availability fallback) while stating its own numbers.
//
// targetImmunityMs = 0 is load-bearing: Outflow is the one ruleset with no
// per-attacker immunity rule, so inheriting the standard BASE's 3000 ms would
// silently change the game.
static const Projector outflowBase = {
    ProjectorId::BASE, "BASE",
    /* cycles          */ 20,     // was enlightPtr->setRepetitions(20)
    /* cooldownMs      */ 20,     // was enlightPtr->setCooldown(20)
    /* rangeM          */ 0,      // device max — today's classifier behaviour
    /* recharge        */ Recharge::NONE,   // energy is the life total; never refills
    /* energyCost      */ 1,
    /* maxEnergy       */ 100,    // startEnergy default; setPool() carries the config var
    /* rechargeDelayMs */ 0,
    /* rechargeMs      */ 0,
    /* strength        */ 1,      // one standard hit = hitDmg energy
    /* roleTag         */ 0,
    /* targetImmunityMs*/ 0,      // no immunity in this game — see above
    /* readyMs         */ 0,      // nothing to switch to
    /* shotAction      */ nullptr,            // the standard Enlight action
    /* icon            */ ICON_ENERGY_BITMAP, // today's display
};
static_assert(outflowBase.id == ProjectorId::BASE,
              "a base override must keep the BASE id");
static_assert(outflowBase.recharge != Recharge::CONSUMED,
              "the baseline is undroppable; CONSUMED would delete it at zero");

static const ProjectorSet projectorSet = {
    nullptr, 0,      // no powered projectors
    0,               // empty catalogue
    &outflowBase,    // the baseline, retuned
    nullptr,         // maxOwned unused: nothing is grantable
    nullptr,         // isAvailable: never consulted for the baseline anyway
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
    /* replyRadioRules       */ Outflow::replyRadioRules,    /* replyRadioRuleCount    */ 3,
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
    /* gameTimeLeft          */ &Outflow::gameTimeLeft,
    /* projectors            */ &Outflow::projectorSet,
    /* onEnd                 */ nullptr,
};
