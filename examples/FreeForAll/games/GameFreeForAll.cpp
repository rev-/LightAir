#include <LightAir.h>

// ================================================================
// Free For All — every player shines every other player.
//
// States
//   IN_GAME  (0) : player is active; can shine; display shows lives/energy/time/points.
//   OUT_GAME (1) : player is out; waits for auto-respawn; display shows time.
//   GAME_END (2) : game over; display shows final time/points/energySpent/shoneTimes.
//
// Radio messages (even = request)
//   MSG_LIT  (0x10) : unicast to the player who was optically detected.
//
// Radio replies (odd = reply, msgType = request + 1)
//   Reply to MSG_LIT (0x11), payload[0] indicates outcome:
//     MSG_REPLY_TAKEN (0x00) : hit registered; player still alive (lives > 0).
//     MSG_REPLY_SHONE (0x01) : hit registered; player eliminated (lives == 0).
//     MSG_REPLY_OUT   (0x02) : player was already out; hit had no effect.
//
// Config vars
//   startLives   : lives at game start (default 3).
//   respawnSecs  : seconds until auto-respawn (default 30).
//   startEnergy  : energy at game start / after respawn (default 50).
//   recharge     : ms after trigger release before full energy is restored (default 1000).
//   gameTime     : total game duration in seconds (default 900).
//
// Shine flow
//   1. TRIG_1 PRESSED/HELD → enlight.run(REPS)  (non-blocking, ~REPS*8 ms)
//   2. enlight.poll() → PLAYER_HIT → radio.sendTo(target, MSG_LIT)
//      NO_HIT / LOW_POW: missed shot, no radio message.
//
// Shone flow
//   Receiving MSG_LIT → lives-- → transition to OUT_GAME.
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

// ---- Radio message types ----
enum Msg      : uint8_t { MSG_LIT = 0x10 };
enum MsgReply : uint8_t { MSG_REPLY_TAKEN = 0x00, MSG_REPLY_SHONE = 0x01, MSG_REPLY_OUT = 0x02 };

// ---- Config variables ----
static int startLives  = 3;     // lives at start / after respawn
static int respawnSecs = 30;    // seconds until auto-respawn
static int startEnergy = 50;    // energy at start / after respawn
static int recharge    = 1000;  // ms after trigger release before energy restored
static int gameTime    = 900;   // total game duration in seconds

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

// ---- Game var declarations ----
static GameVar vars[] = {
    //                                                               col row  cfg    min    max   step
    // Shown in IN_GAME
    GameVar::Int("Lives",    &lives,        1u<<IN_GAME,            ICON_LIFE,   0, 0, false),
    GameVar::Int("Energy",   &energy,       1u<<IN_GAME,            ICON_ENERGY, 1, 0, false),
    GameVar::Int("Time",     &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME, 0, 1, false),
    GameVar::Int("Points",   &points,       1u<<IN_GAME,            ICON_SCORE,  1, 1, false),
    // Config (stateMask=0 → not monitored, only in config menu)
    GameVar::Int("Start",    &startLives,   0, ICON_LIFE,   0, 0, true,   1,    10,     1),
    GameVar::Int("Respawn",  &respawnSecs,  0, ICON_LIFE,   0, 0, true,   5,   120,     5),
    GameVar::Int("Energy",   &startEnergy,  0, ICON_ENERGY, 0, 0, true,  10,   100,    10),
    GameVar::Int("Recharge", &recharge,     0, ICON_ENERGY, 0, 0, true,   0, 10000,  1000),
    GameVar::Int("Time",     &gameTime,     0, ICON_TIME,   0, 0, true,  60,   900,    60),
    // Shown in GAME_END (same pointers, different positions / stateMask)
    GameVar::Int("Time",     &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0, false),
    GameVar::Int("Points",   &points,       1u<<GAME_END, ICON_SCORE,  1, 0, false),
    GameVar::Int("Energy",   &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1, false),
    GameVar::Int("Shone",    &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1, false),
};

// ---- onBegin: reset all runtime state from config ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&) {
    lives        = startLives;
    energy       = startEnergy;
    gameTimeLeft = gameTime;
    points       = 0;
    energySpent  = 0;
    shoneTimes   = 0;
    lastTickAt   = millis();
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

static void doInGame(const InputReport& inp, const RadioReport& radio,
                     LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

    // Each incoming MSG_LIT costs one life; reply signals the outcome.
    for (uint8_t i = 0; i < radio.count; i++) {
        if (radio.events[i].type == RadioEventType::MessageReceived &&
            radio.events[i].packet.msgType == MSG_LIT) {
            if (lives > 0) lives--;
            uint8_t replyPayload = (lives > 0) ? MSG_REPLY_TAKEN : MSG_REPLY_SHONE;
            out.radio.reply(radio.events[i].packet, &replyPayload, 1);
        }
    }


    static bool     triggerWasActive = false;
    static uint32_t releaseAt        = 0;

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

    // Detect release edge → start recharge cooldown.
    if (triggerWasActive && !triggerActive)
        releaseAt = millis();
    triggerWasActive = triggerActive;

    // Restore full energy once cooldown has elapsed.
    if (!triggerActive && energy < startEnergy) {
        if (recharge == 0)
            energy = startEnergy;
        else if (millis() - releaseAt >= (uint32_t)recharge)
            energy = startEnergy;
    }

    // Poll Enlight; a hit means we shone someone — send them MSG_LIT.
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) {
        points++;
        out.radio.sendTo(r.id, MSG_LIT);
    }
    // NO_HIT / LOW_POW: missed shot — no radio message.
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();   // keep countdown running while waiting to respawn

    // Reject any incoming MSG_LIT — player is out; inform the sender.
    for (uint8_t i = 0; i < radio.count; i++) {
        if (radio.events[i].type == RadioEventType::MessageReceived &&
            radio.events[i].packet.msgType == MSG_LIT) {
            uint8_t replyPayload = MSG_REPLY_OUT;
            out.radio.reply(radio.events[i].packet, &replyPayload, 1);
        }
    }
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
    /* typeId         */ 0x00000001,
    /* name           */ "Free for All",
    /* vars           */ FFA::vars,      /* varCount      */ 13,
    /* rules          */ FFA::rules,     /* ruleCount     */  4,
    /* behaviors      */ FFA::behaviors, /* behaviorCount */  3,
    /* currentState   */ &FFA::gState,   /* initialState  */ FFA::IN_GAME,
    /* onBegin        */ FFA::onBegin,
};
