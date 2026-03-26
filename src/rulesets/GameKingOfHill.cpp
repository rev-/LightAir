#include <LightAir.h>
#include <string.h>
#include <stdio.h>
#include "GameTypeIds.h"

// ================================================================
// King of Hill — FFA combat, one (or more) CP totem(s), teamless BASE.
//
// States
//   IN_GAME  (0) : active; can shoot; declares presence to CP.
//   OUT_GAME (1) : down; waits for teamless BASE beacon to respawn.
//   GAME_END (2) : game over; final score display.
//
// Teams
//   No host-assigned teams.  Each player's 0-indexed "team slot" is
//   derived from their hardware player ID: myTeam = cfg.id - 1 (0–15).
//   This maps one-to-one to the CP's team/player index (cpTeam 0–15).
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT           (0x10) : unicast hit to a target player.
//   MSG_CP_BEACON     (0x52) : broadcast by CP totem every 2 s.
//                              payload[0] = cpTeam (0–15 = owner, 0xFF = neutral).
//                              Players reply with subType = myTeam+1 (1–16) if near.
//   MSG_CP_SCORE      (0x54) : broadcast by CP totem when it awards a point.
//                              payload[0] = cpTeam (0–15). Only the matching
//                              player increments their local points counter.
//   MSG_BASE_BEACON   (0x56) : broadcast by teamless BASE (payload[0]=0xFF).
//                              OUT_GAME players reply to request respawn.
//   MSG_SCORE_COLLECT (0x12) : per-player score broadcast at GAME_END.
//
// Reply sub-types (payload[0] of 0x11 reply to MSG_LIT)
//   REPLY_TAKEN (1) : target absorbed hit; lives > 0 after decrement.
//   REPLY_SHONE (2) : target eliminated; lives reached 0.
//   REPLY_DOWN  (3) : target already OUT_GAME; hit ignored.
//
// CP attachment and scoring (on CP totem firmware):
//   Same logic as Upkeep, extended to up to 16 player slots.
//   If exactly one player replies to a 2-s beacon window, the CP
//   attaches (or stays) to that player.  Multiple repliers → contested,
//   hold current owner.  After 10 s of unchanged ownership the CP
//   broadcasts MSG_CP_SCORE.  Only the owning player counts the point.
//
// Respawn flow:
//   On shone: respawnAt = millis() + respawnSecs*1000; canRespawn = false.
//   doOutGame waits until then, then scans for a teamless BASE beacon
//   (payload[0]==0xFF) with RSSI ≥ NEAR_BASE_RSSI.  When found,
//   sets canRespawn = true and replies so the BASE shows a Respawn animation.
//
// Scoring and victory:
//   Winner = most CP points.  Tie-break: fewest shoneTimes.
//   When endPoints > 0 and a player's local points reach that value, the
//   GameRunner transitions them to GAME_END and broadcasts MSG_END_GAME;
//   all other devices are forced to GAME_END by the infrastructure.
//   0 = unlimited (time-only).
//
// Config vars
//   startLives   : lives at start / after respawn (default 3).
//   respawnSecs  : minimum seconds dead before BASE respawn (default 30).
//   startEnergy  : energy at start / after respawn (default 50).
//   rechargeSecs : secs after trigger release before energy restored (default 10).
//   gameTime     : total game duration in seconds (default 900).
//   endPoints    : CP-point limit that ends the game early; 0 = disabled (default).
// ================================================================

extern Enlight* enlightPtr;

namespace KoH {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types ----
using RadioMsg::MSG_LIT;            // 0x10
using RadioMsg::MSG_SCORE_COLLECT;  // 0x12
using RadioMsg::MSG_CP_BEACON;      // 0x52
using RadioMsg::MSG_CP_SCORE;       // 0x54
using RadioMsg::MSG_BASE_BEACON;    // 0x56

// ---- Reply sub-types for MSG_LIT ----
enum ReplySubType : uint8_t { REPLY_TAKEN = 1, REPLY_SHONE = 2, REPLY_DOWN = 3 };

// ---- CP state sentinel ----
static constexpr uint8_t CP_TEAM_NONE   = 0xFF;

// ---- RSSI proximity thresholds ----
static constexpr int8_t NEAR_CP_RSSI   = -65;  // ~3 m indoors; CP presence
static constexpr int8_t NEAR_BASE_RSSI = -60;  // ~2 m indoors; BASE respawn

// ---- Config variables ----
static int startLives   = 3;
static int respawnSecs  = 30;
static int startEnergy  = 50;
static int rechargeSecs = 10;
static int gameTime     = 900;
static int endPoints    = 0;    // 0 = unlimited

// ---- Runtime variables ----
static int lives        = 3;
static int energy       = 50;
static int gameTimeLeft = 900;
static int points       = 0;   // CP points only
static int energySpent  = 0;
static int shoneTimes   = 0;

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;
static bool     canRespawn;
static uint8_t  myTeam;    // cfg.id - 1 (0-indexed player slot)

static bool     triggerWasActive = false;
static uint32_t releaseAt        = 0;

// ---- Totem device-ID slots ----
static uint8_t cpIds[6]      = {};
static uint8_t numActiveCPs  = 0;
static uint8_t cpState[6];   // last known cpTeam broadcast per CP

// ---- Config vars (startup menu) ----
static const ConfigVar configVars[] = {
    //name           value            min    max   step
    { "Lives",      &startLives,      1,    5,     1 },
    { "Respawn",    &respawnSecs,     5,  120,     5 },
    { "Energy",     &startEnergy,    10,  100,    10 },
    { "Recharge",   &rechargeSecs,    0,   20,     5 },
    { "Time",       &gameTime,       60,  900,    60 },
    { "EndPoints",  &endPoints,       0,  100,    20 },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::Int("Lives",   &lives,        1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy",  &energy,       1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",    &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Points",  &points,       1u<<IN_GAME,                  ICON_SCORE,  1, 1),
    // GAME_END display
    MonitorVar::Int("Time",    &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points",  &points,       1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy",  &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",   &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- Totem requirements ----
static const LightAir_TotemRequirement totemRequirements[] = {
    { TotemRoleId::CP,    1, 6, nullptr },  // 1 required, up to 6
    { TotemRoleId::BASE,  1, 4, nullptr },  // 1 required teamless base
    { TotemRoleId::BONUS, 0, GameDefaults::MAX_PARTICIPANTS, nullptr },
    { TotemRoleId::MALUS, 0, GameDefaults::MAX_PARTICIPANTS, nullptr },
};

// ---- Helper: find CP by sender ID ----
static int8_t cpIndex(uint8_t senderId) {
    for (uint8_t i = 0; i < numActiveCPs; i++)
        if (cpIds[i] != 0 && cpIds[i] == senderId) return (int8_t)i;
    return -1;
}

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*,
                    const LightAir_GameRunner& runner) {
    lives            = startLives;
    energy           = startEnergy;
    gameTimeLeft     = gameTime;
    points           = 0;
    energySpent      = 0;
    shoneTimes       = 0;
    canRespawn       = false;
    respawnAt        = 0;
    lastTickAt       = millis();
    triggerWasActive = false;
    releaseAt        = 0;

    for (uint8_t i = 0; i < 6; i++) cpState[i] = CP_TEAM_NONE;

    PlayerConfig cfg;
    player_config_load(cfg);
    // Each player is their own "team"; use 0-indexed slot so the CP reply
    // formula (subType = myTeam+1) maps player 1→subType 1, player 2→subType 2, etc.
    myTeam = (cfg.id > 0 && cfg.id < PlayerDefs::MAX_PLAYER_ID)
             ? (uint8_t)(cfg.id - 1)
             : 0;

    numActiveCPs = 0;
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t id = runner.totemIdForRole(TotemRoleId::CP, i);
        if (id == 0) break;
        cpIds[numActiveCPs++] = id;
    }
}

// ---- DirectRadioRule conditions ----
static bool litAndTaken(const RadioPacket&) { return lives > 1; }
static bool litAndShone(const RadioPacket&) { return lives <= 1; }

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }
static void onLitShone(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }

static void onCpScore(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    if (cpIndex(pkt.senderId) < 0) return;   // ignore unknown CP
    if (pkt.payloadLen < 1)        return;
    if (pkt.payload[0] != myTeam)  return;   // only count this player's points
    points++;
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType         condition    replySubType  onReceive
    { IN_GAME,  MSG_LIT,        litAndTaken, REPLY_TAKEN,  onLitTaken },
    { IN_GAME,  MSG_LIT,        litAndShone, REPLY_SHONE,  onLitShone },
    { OUT_GAME, MSG_LIT,        nullptr,     REPLY_DOWN,   nullptr    },
    { IN_GAME,  MSG_CP_SCORE,   nullptr,     0,            onCpScore  },
    { OUT_GAME, MSG_CP_SCORE,   nullptr,     0,            onCpScore  },
};

// ---- ReplyRadioRule handlers ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType       condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN, nullptr, onReplyTaken },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE, nullptr, onReplyShone },
};

// ---- Winner election rules ----
static const WinnerVar winnerVars[] = {
    { &points,     WinnerDir::MAX },  // primary: most CP points wins
    { &shoneTimes, WinnerDir::MIN },  // tie-break: fewest times shone
};

// ---- Per-second ticker ----
static void tickGameTime() {
    uint32_t now = millis();
    if (now - lastTickAt >= 1000) {
        lastTickAt += 1000;
        if (gameTimeLeft > 0) gameTimeLeft--;
    }
}

// ---- Scan CP beacons: update ownership display and (if active) reply with presence ----
// sendPresence = true in IN_GAME, false in OUT_GAME (down players cannot capture CPs).
static void scanCpBeacons(const RadioReport& radio, LightAir_DisplayCtrl& disp,
                           GameOutput& out, bool sendPresence) {
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_CP_BEACON)                   continue;

        int8_t idx = cpIndex(ev.packet.senderId);
        if (idx < 0) continue;

        // Notify on ownership change (CP_TEAM_NONE = first beacon ever heard).
        uint8_t newState = ev.packet.payload[0];
        if (newState != cpState[idx]) {
            cpState[idx] = newState;
            char msg[20];
            if (newState == CP_TEAM_NONE) {
                snprintf(msg, sizeof(msg), "CP %d neutral", idx + 1);
                disp.showMessage(msg, 3000);
            } else {
                // +1 converts 0-indexed cpTeam back to player ID for display.
                snprintf(msg, sizeof(msg), "CP %d -> P%d!", idx + 1, (int)newState + 1);
                disp.showMessage(msg, 3000);
                if (newState == myTeam)
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagReturn);  // gained CP
                else
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagTaken);   // lost CP
            }
        }

        // Reply with our presence so the CP totem can update ownership.
        // subType = myTeam + 1 (same formula as Upkeep: 1 = slot-0, 2 = slot-1, …).
        if (sendPresence && ev.rssi >= NEAR_CP_RSSI)
            out.radio.reply(ev.packet, (uint8_t)(myTeam + 1));
    }
}

// ---- Transition conditions ----
static bool gameTimeExpired(const InputReport&, const RadioReport&) {
    return gameTimeLeft <= 0;
}
static bool endPointsReached(const InputReport&, const RadioReport&) {
    // Only the player who actually accumulated enough points triggers this;
    // the GameRunner then broadcasts MSG_END_GAME so all other devices follow.
    return endPoints > 0 && points >= endPoints;
}
static bool shone(const InputReport&, const RadioReport&) {
    return lives <= 0;
}
static bool canRespawnReady(const InputReport&, const RadioReport&) {
    return canRespawn;
}

// ---- Transition actions ----
static void onShone(LightAir_DisplayCtrl& disp, GameOutput& out) {
    shoneTimes++;
    respawnAt  = millis() + (uint32_t)respawnSecs * 1000;
    canRespawn = false;
    disp.showMessage("Shone!", 2000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Down);
}
static void onRespawn(LightAir_DisplayCtrl& disp, GameOutput& out) {
    lives      = startLives;
    energy     = startEnergy;
    canRespawn = false;
    disp.showMessage("Back in game!", 1000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Up);
}
static void onGameEnd(LightAir_DisplayCtrl& disp, GameOutput& out) {
    disp.showMessage("Game over!", 3000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::EndGame);
}

// ---- State machine ----
static const StateRule rules[] = {
    { IN_GAME,  gameTimeExpired,  GAME_END, onGameEnd  },
    { IN_GAME,  endPointsReached, GAME_END, onGameEnd  },
    { IN_GAME,  shone,            OUT_GAME, onShone    },
    { OUT_GAME, gameTimeExpired,  GAME_END, onGameEnd  },
    { OUT_GAME, endPointsReached, GAME_END, onGameEnd  },
    { OUT_GAME, canRespawnReady,  IN_GAME,  onRespawn  },
};

// ---- Per-state behaviors ----
static void doInGame(const InputReport& inp, const RadioReport& radio,
                     LightAir_DisplayCtrl& disp, GameOutput& out) {
    tickGameTime();
    scanCpBeacons(radio, disp, out, true);

    // ---- Combat ----
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

    if (triggerWasActive && !triggerActive) releaseAt = millis();
    triggerWasActive = triggerActive;

    if (!triggerActive && energy < startEnergy) {
        if (rechargeSecs == 0)
            energy = startEnergy;
        else if ((millis() - releaseAt) / 1000 >= (uint32_t)rechargeSecs)
            energy = startEnergy;
    }

    // Poll Enlight; a confirmed hit sends MSG_LIT. No friendly-fire check needed
    // since every player is their own team.
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT)
        out.radio.sendTo(r.id, MSG_LIT);
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl& disp, GameOutput& out) {
    tickGameTime();
    // Track CP ownership while down (sendPresence=false: cannot capture CPs).
    scanCpBeacons(radio, disp, out, false);

    if (millis() < respawnAt) return;

    // Scan for a teamless BASE beacon (payload[0]==0xFF) within range.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
        if (ev.packet.payloadLen < 1)                             continue;
        if (ev.packet.payload[0] != 0xFF)                         continue;  // teamless only
        if (ev.rssi            < NEAR_BASE_RSSI)                  continue;
        canRespawn = true;
        // Reply so the BASE totem shows a Respawn animation in this player's colour.
        out.radio.reply(ev.packet, (uint8_t)(myTeam + 1));
        break;
    }
}

static const StateBehavior behaviors[] = {
    { IN_GAME,  doInGame  },
    { OUT_GAME, doOutGame },
    { GAME_END, nullptr   },
};

} // namespace KoH

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
const LightAir_Game game_koh = {
    /* typeId                */ GameTypeId::KING_OF_HILL,
    /* name                  */ "King of Hill",
    /* configVars            */ KoH::configVars,         /* configCount            */ 6,
    /* monitorVars           */ KoH::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ KoH::directRadioRules,   /* directRadioRuleCount   */ 5,
    /* replyRadioRules       */ KoH::replyRadioRules,    /* replyRadioRuleCount    */ 2,
    /* rules                 */ KoH::rules,              /* ruleCount              */ 6,
    /* behaviors             */ KoH::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &KoH::gState,            /* initialState           */ KoH::IN_GAME,
    /* onBegin               */ KoH::onBegin,
    /* winnerVars            */ KoH::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ KoH::GAME_END,
    /* scoreMsgType          */ KoH::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ nullptr,                 // default individual ranking
    /* totemRequirements     */ KoH::totemRequirements,  /* totemRequirementCount  */ 4,
    /* teamCount             */ 0,
    /* teamMap               */ nullptr,
};
