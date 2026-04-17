#include <LightAir.h>
#include <string.h>
#include "GameTypeIds.h"

// ================================================================
// Flag — capture-the-flag team game: O vs X.
//
// States
//   IN_GAME  (0) : active; can shoot; can pick up and score the flag.
//   OUT_GAME (1) : down; waiting for BASE beacon to respawn.
//   GAME_END (2) : over; shows final stats.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT           (0x10) : unicast hit to a target player.
//   MSG_FLAG_BEACON   (0x58) : broadcast by FLAG totems; payload[0]=state(0=in,1=out),
//                              payload[1]=flagTeam. Used for flag pickup.
//   MSG_BASE_BEACON   (0x56) : broadcast by BASE totems; payload[0]=team.
//                              Used for respawn (OUT_GAME) and flag scoring (IN_GAME).
//   MSG_FLAG_EVENT    (0x50) : broadcast by a player on flag state change.
//   MSG_SCORE_COLLECT (0x12) : broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x11 reply)
//   REPLY_TAKEN  (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE  (2) : target eliminated; lives reached 0.
//   REPLY_DOWN   (3) : target was already OUT_GAME; hit ignored.
//   REPLY_FRIEND (4) : hit rejected — friendly fire is disabled.
//
// Flag event sub-types (payload[0] of MSG_FLAG_EVENT broadcast)
//   FEVENT_TAKEN   (1) : player picked up flag; payload[1]=flag_team.
//   FEVENT_DROPPED (2) : carrier was shot, flag dropped; payload[1]=flag_team.
//   FEVENT_SCORED  (3) : flag captured at base; payload[1]=flag_team.
//
//   flag_team is the team that OWNS the flag (0=O, 1=X).
//   MSG_FLAG_EVENT broadcasts do not expect meaningful replies; the 6
//   DirectRadioRules for this message type suppress the auto-reply and
//   update local tracking state as a side-effect.
//
// Flag background alert
//   While hasEnemyFlag is true, uiCtrl->setBackground(kFlagCarryBg)
//   plays a continuous slow cyan pulse with gentle vibration.
//   clearBackground() is called on score, drop (shot), or game end.
//   uiCtrl is cached from the LightAir_UICtrl* passed to onBegin.
//
// Flag tracking
//   hasEnemyFlag       : this player is currently carrying opponent's flag.
//   enemyFlagCarrierId : 0xFF = available; other = non-zero means taken.
//   myFlagCarrierId    : 0xFF = own flag at home; other = enemy holding it.
//   myTeamPoints       : team-aggregate flag captures known locally.
//
// Scoring
//   flagsCaptured (= points) counts personal flag captures.
//   Winner = team with higher aggregate flagsCaptured; tie-break: fewer shoneTimes.
//   endPoints (config): if > 0, GAME_END fires once myTeamPoints >= endPoints.
//
// Respawn flow
//   Identical to Teams: respawn timer + own-team BASE beacon with RSSI >= -60 dBm.
//
// Config vars
//   startLives   : lives at start / after respawn (default 3).
//   respawnSecs  : minimum seconds dead before BASE beacon triggers respawn (default 30).
//   startEnergy  : energy at start / after respawn (default 50).
//   rechargeSecs : seconds after trigger release before energy restored (default 10).
//   gameTime     : total game duration in seconds (default 900).
//   friendlyFire : 0 = off (default), 1 = on.
//   endPoints    : team-capture limit to trigger early GAME_END; 0 = disabled.
// ================================================================

extern Enlight* enlightPtr;

namespace Flag {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types ----
using RadioMsg::MSG_LIT;            // 0x10
using RadioMsg::MSG_FLAG_EVENT;     // 0x50
using RadioMsg::MSG_SCORE_COLLECT;  // 0x12
using RadioMsg::MSG_BASE_BEACON;    // 0x56
using RadioMsg::MSG_FLAG_BEACON;    // 0x58

// ---- Reply sub-types ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN  = 1,
    REPLY_SHONE  = 2,
    REPLY_DOWN   = 3,
    REPLY_FRIEND = 4,
    REPLY_IMMUNE = 5,
};

// ---- Flag event sub-types ----
enum FlagEventType : uint8_t {
    FEVENT_TAKEN   = 1,
    FEVENT_DROPPED = 2,
    FEVENT_SCORED  = 3,
};

// ---- Proximity thresholds ----
static constexpr int8_t  NEAR_RSSI_THRESHOLD = -60;  // ~2 m: base proximity (respawn + scoring)
static constexpr int8_t  FLAG_RSSI_THRESHOLD  = -65;  // ~3-4 m: flag pickup zone
static constexpr uint32_t HIT_IMMUNITY_MS     = 3000;

// ---- Config variables ----
static int startLives   = 3;
static int respawnSecs  = 30;
static int startEnergy  = 50;
static int rechargeSecs = 10;
static int gameTime     = 900;
static int friendlyFire = 0;
static int endPoints    = 0;

// ---- Runtime variables ----
static int lives         = 3;
static int energy        = 50;
static int gameTimeLeft  = 900;
static int points        = 0;   // mirrors flagsCaptured; used for score slot
static int energySpent   = 0;
static int shoneTimes    = 0;
static int flagsCaptured = 0;   // times this player personally scored
static int myTeamPoints  = 0;   // aggregate team flag captures known locally

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;
static bool     canRespawn;
static bool     triggerWasActive = false;
static uint32_t releaseAt        = 0;
static uint32_t litAt[PlayerDefs::MAX_PLAYER_ID];

static bool     hasEnemyFlag;
static uint8_t  enemyFlagCarrierId;  // 0xFF = flag available at its totem
static uint8_t  myFlagCarrierId;     // 0xFF = our flag at home

static uint8_t  myTeam;      // 0=O, 1=X
static uint8_t  teamMap[PlayerDefs::MAX_PLAYER_ID];  // per-player team index; filled from config blob

// Cached UICtrl pointer for setBackground / clearBackground calls,
// which are not exposed through the UIOutput queue.
static LightAir_UICtrl* uiCtrl = nullptr;

// ---- Totem device-ID slots (populated in onBegin from runner) ----
static uint8_t baseO_ids[4] = {};
static uint8_t baseX_ids[4] = {};

// ---- Config vars ----
static const ConfigVar configVars[] = {
    //  name            value            min   max   step
    { "Lives",        &startLives,      1,    5,    1  },
    { "Respawn",      &respawnSecs,     5,    120,  5  },
    { "Energy",       &startEnergy,     10,   100,  10 },
    { "Recharge",     &rechargeSecs,    0,    20,   5  },
    { "Time",         &gameTime,        60,   900,  60 },
    { "FriendlyFire", &friendlyFire,    0,    1,    1  },
    { "EndPoints",    &endPoints,       0,    10,   1  },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME
    MonitorVar::Int("Lives",  &lives,        1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy", &energy,       1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",   &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Flags",  &flagsCaptured, 1u<<IN_GAME,                 ICON_FLAG,   1, 1),
    // GAME_END
    MonitorVar::Int("Time",   &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Flags",  &flagsCaptured, 1u<<GAME_END, ICON_FLAG,  1, 0),
    MonitorVar::Int("Energy", &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",  &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- Totem requirements ----
static const LightAir_TotemRequirement totemRequirements[] = {
    { TotemRoleId::FLAG_O, 1, 1, nullptr },   // exactly 1 required
    { TotemRoleId::FLAG_X, 1, 1, nullptr },   // exactly 1 required
    { TotemRoleId::BASE_O, 0, 4, nullptr },
    { TotemRoleId::BASE_X, 0, 4, nullptr },
    { TotemRoleId::BASE,   0, 4, nullptr },
    { TotemRoleId::BONUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr },
    { TotemRoleId::MALUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr },
};

// ---- Continuous flag-carry background alert ----
// Slow cyan pulse with interleaved tone: 300 ms at 4500 Hz (500 Hz above LIT's
// 4000 Hz) followed by 200 ms silence. Gentle vibration on the sound step only.
static const LightAir_UICtrl::UIAction kFlagCarryBg = {
    /* durations    */ { 300, 200, 0, 0 },
    /* stepCount    */ 2,
    /* soundFreqs   */ { 4500, 0, 0, 0 },
    /* vibIntensity */ { 25, 0, 0, 0 },
    /* rgbColors    */ { {0, 180, 255}, {0, 30, 80}, {0, 0, 0}, {0, 0, 0} },
    /* priority     */ 1,
};

// ---- Helpers ----
static uint8_t enemyTeam()            { return myTeam ^ 1; }

static bool isOpponent(uint8_t targetId) {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return false;
    return teamMap[targetId] != myTeam;
}

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio& radio, LightAir_UICtrl* ui,
                    const LightAir_GameRunner& runner) {
    lives              = startLives;
    energy             = startEnergy;
    gameTimeLeft       = gameTime;
    points             = 0;
    flagsCaptured      = 0;
    myTeamPoints       = 0;
    energySpent        = 0;
    shoneTimes         = 0;
    canRespawn         = false;
    respawnAt          = 0;
    hasEnemyFlag       = false;
    enemyFlagCarrierId = 0xFF;
    myFlagCarrierId    = 0xFF;
    lastTickAt         = millis();
    triggerWasActive   = false;
    releaseAt          = 0;
    memset(litAt, 0, sizeof(litAt));
    uiCtrl             = ui;

    myTeam = runner.teamOf(radio.playerId());

    for (uint8_t i = 0; i < 4; i++) {
        baseO_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_O, i);
        baseX_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_X, i);
    }
}

// ---- DirectRadioRule conditions ----
static bool notImmune(const RadioPacket& pkt) {
    return pkt.senderId >= PlayerDefs::MAX_PLAYER_ID
        || litAt[pkt.senderId] == 0
        || millis() - litAt[pkt.senderId] >= HIT_IMMUNITY_MS;
}

static bool litAndTakenAndValid(const RadioPacket& pkt) {
    return lives > 1 && (pkt.team != myTeam || friendlyFire) && notImmune(pkt);
}
static bool litAndShoneAndValid(const RadioPacket& pkt) {
    return lives <= 1 && (pkt.team != myTeam || friendlyFire) && notImmune(pkt);
}
static bool litButFriendly(const RadioPacket& pkt) {
    return pkt.team == myTeam && !friendlyFire;
}
static bool litButImmune(const RadioPacket& pkt) {
    return (pkt.team != myTeam || friendlyFire) && !notImmune(pkt);
}
static bool flagEventTaken(const RadioPacket& pkt) {
    return pkt.payloadLen >= 2 && pkt.payload[0] == FEVENT_TAKEN;
}
static bool flagEventDropped(const RadioPacket& pkt) {
    return pkt.payloadLen >= 2 && pkt.payload[0] == FEVENT_DROPPED;
}
static bool flagEventScored(const RadioPacket& pkt) {
    return pkt.payloadLen >= 2 && pkt.payload[0] == FEVENT_SCORED;
}

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    lives--;
    if (pkt.senderId < PlayerDefs::MAX_PLAYER_ID) litAt[pkt.senderId] = millis();
}
static void onLitShone(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    lives--;
    if (pkt.senderId < PlayerDefs::MAX_PLAYER_ID) litAt[pkt.senderId] = millis();
}

static void onFlagEventTaken(const RadioPacket& pkt,
                              LightAir_DisplayCtrl&, GameOutput& out) {
    uint8_t flagTeam = pkt.payload[1];
    if (flagTeam == enemyTeam()) {
        // A player (teammate or opponent player) picked up the enemy flag.
        // Mark as taken so we don't attempt a duplicate pickup.
        enemyFlagCarrierId = pkt.senderId;
    } else {
        // Enemy picked up our flag.
        myFlagCarrierId = pkt.senderId;
        out.ui.trigger(LightAir_UICtrl::UIEvent::FlagTaken);   // "FLAG LOST"
    }
}

static void onFlagEventDropped(const RadioPacket& pkt,
                                LightAir_DisplayCtrl&, GameOutput& out) {
    uint8_t flagTeam = pkt.payload[1];
    if (flagTeam == enemyTeam()) {
        enemyFlagCarrierId = 0xFF;
    } else {
        // Enemy dropped our flag — it resets to its totem.
        myFlagCarrierId = 0xFF;
        out.ui.trigger(LightAir_UICtrl::UIEvent::FlagReturn);  // "FLAG BACK"
    }
}

static void onFlagEventScored(const RadioPacket& pkt,
                               LightAir_DisplayCtrl&, GameOutput& out) {
    uint8_t flagTeam = pkt.payload[1];
    if (flagTeam == enemyTeam()) {
        // A teammate captured the enemy's flag: our team just scored.
        enemyFlagCarrierId = 0xFF;
        myTeamPoints++;
    } else {
        // Enemy scored using our flag; flag resets to our totem.
        myFlagCarrierId = 0xFF;
        out.ui.trigger(LightAir_UICtrl::UIEvent::FlagReturn);  // "FLAG BACK"
    }
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType         condition            replySubType  onReceive
    // — shot handling —
    { IN_GAME,  MSG_LIT,        litAndTakenAndValid, REPLY_TAKEN,  onLitTaken         },
    { IN_GAME,  MSG_LIT,        litAndShoneAndValid, REPLY_SHONE,  onLitShone         },
    { IN_GAME,  MSG_LIT,        litButFriendly,      REPLY_FRIEND, nullptr            },
    { IN_GAME,  MSG_LIT,        litButImmune,        REPLY_IMMUNE, nullptr            },
    { OUT_GAME, MSG_LIT,        nullptr,             REPLY_DOWN,   nullptr            },
    // — flag state synchronisation (IN_GAME) —
    { IN_GAME,  MSG_FLAG_EVENT, flagEventTaken,      0,            onFlagEventTaken   },
    { IN_GAME,  MSG_FLAG_EVENT, flagEventDropped,    0,            onFlagEventDropped },
    { IN_GAME,  MSG_FLAG_EVENT, flagEventScored,     0,            onFlagEventScored  },
    // — flag state synchronisation (OUT_GAME) —
    { OUT_GAME, MSG_FLAG_EVENT, flagEventTaken,      0,            onFlagEventTaken   },
    { OUT_GAME, MSG_FLAG_EVENT, flagEventDropped,    0,            onFlagEventDropped },
    { OUT_GAME, MSG_FLAG_EVENT, flagEventScored,     0,            onFlagEventScored  },
};

// ---- ReplyRadioRule handlers ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Taken);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    // Shooting eliminates a target but does NOT award points in Flag.
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyFriend(const RadioPacket&, const RadioPacket&,
                          LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Friend);
}
static void onReplyImmune(const RadioPacket&, const RadioPacket&,
                          LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Immune);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType        condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN,  nullptr, onReplyTaken  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE,  nullptr, onReplyShone  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_FRIEND, nullptr, onReplyFriend },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_IMMUNE, nullptr, onReplyImmune },
};

// ---- Winner election ----
static const WinnerVar winnerVars[] = {
    { &points,     WinnerDir::MAX },   // points = flagsCaptured
    { &shoneTimes, WinnerDir::MIN },
};

// ---- Per-second game-time ticker ----
static void tickGameTime() {
    uint32_t now = millis();
    if (now - lastTickAt >= 1000) {
        lastTickAt += 1000;
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
static bool canRespawnReady(const InputReport&, const RadioReport&) {
    return canRespawn;
}
static bool endPointsReached(const InputReport&, const RadioReport&) {
    return endPoints > 0 && myTeamPoints >= endPoints;
}

// ---- Transition actions ----
static void onShone(LightAir_DisplayCtrl& disp, GameOutput& out) {
    if (hasEnemyFlag) {
        uint8_t pl[2] = { FEVENT_DROPPED, enemyTeam() };
        out.radio.broadcast(MSG_FLAG_EVENT, pl, 2);
        hasEnemyFlag       = false;
        enemyFlagCarrierId = 0xFF;
        out.ui.trigger(LightAir_UICtrl::UIEvent::FlagTaken);  // "FLAG LOST"
        if (uiCtrl) uiCtrl->clearBackground();
    }
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
    memset(litAt, 0, sizeof(litAt));
    disp.showMessage("Back in game!", 1000);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Up);
}

static void onGameEnd(LightAir_DisplayCtrl& disp, GameOutput& out) {
    if (hasEnemyFlag) {
        hasEnemyFlag = false;
        if (uiCtrl) uiCtrl->clearBackground();
    }
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

    // ---- Shooting / energy ----
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) {
        if (isOpponent(r.id) || friendlyFire)
            out.radio.sendTo(r.id, MSG_LIT);
    }

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

    if (triggerWasActive && !triggerActive)
        releaseAt = millis();
    triggerWasActive = triggerActive;

    if (!triggerActive && energy < startEnergy) {
        if (rechargeSecs == 0)
            energy = startEnergy;
        else if ((millis() - releaseAt) / 1000 >= (uint32_t)rechargeSecs)
            energy = startEnergy;
    }

    // ---- Flag pickup ----
    // Attempt only when we are not already carrying and the enemy flag is free.
    // Flag totem broadcasts MSG_FLAG_BEACON with payload[0]=0 (in) and payload[1]=flagTeam.
    if (!hasEnemyFlag) {
        for (uint8_t e = 0; e < radio.count; e++) {
            const RadioEvent& ev = radio.events[e];
            if (ev.type           != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != MSG_FLAG_BEACON)                 continue;
            if (ev.packet.payloadLen < 2)                             continue;
            if (ev.packet.payload[0] != 0)                           continue;  // 0 = FLAG_IN
            if (ev.packet.payload[1] != enemyTeam())                 continue;
            if (ev.rssi           <  FLAG_RSSI_THRESHOLD)             continue;
            if (enemyFlagCarrierId != 0xFF)                           continue;

            hasEnemyFlag       = true;
            enemyFlagCarrierId = 0x01;   // mark taken locally; other players update via broadcast
            uint8_t pl[2] = { FEVENT_TAKEN, enemyTeam() };
            out.radio.broadcast(MSG_FLAG_EVENT, pl, 2);
            out.ui.trigger(LightAir_UICtrl::UIEvent::FlagGain);   // "FLAG +"
            if (uiCtrl) uiCtrl->setBackground(kFlagCarryBg);
            break;
        }
    }

    // ---- Flag score ----
    // Score when carrying and we reach a friendly BASE.
    if (hasEnemyFlag) {
        for (uint8_t e = 0; e < radio.count; e++) {
            const RadioEvent& ev = radio.events[e];
            if (ev.type           != RadioEventType::MessageReceived) continue;
            if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
            if (ev.packet.payloadLen < 1)                             continue;
            if (ev.packet.payload[0] != myTeam)                      continue;
            if (ev.rssi           <  NEAR_RSSI_THRESHOLD)             continue;

            flagsCaptured++;
            points             = flagsCaptured;
            myTeamPoints++;
            hasEnemyFlag       = false;
            enemyFlagCarrierId = 0xFF;
            uint8_t pl[2] = { FEVENT_SCORED, enemyTeam() };
            out.radio.broadcast(MSG_FLAG_EVENT, pl, 2);
            out.ui.trigger(LightAir_UICtrl::UIEvent::FlagGain);   // "FLAG +"
            if (uiCtrl) uiCtrl->clearBackground();
            disp.showMessage("FLAG SCORED!", 2000);
            break;
        }
    }
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

    // Minimum respawn timer: ignore beacons until the wait has elapsed.
    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon: own team or teamless (payload[0]==0xFF),
    // RSSI >= proximity threshold.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
        if (ev.packet.payloadLen < 1)                             continue;
        if (ev.packet.payload[0] != myTeam &&
            ev.packet.payload[0] != 0xFF)                         continue;
        if (ev.rssi           <  NEAR_RSSI_THRESHOLD)             continue;
        canRespawn = true;
        // Notify the BASE totem so it can show a Respawn animation.
        out.radio.reply(ev.packet, (uint8_t)(myTeam + 1));
        break;
    }
}

static const StateBehavior behaviors[] = {
    { IN_GAME,  doInGame  },
    { OUT_GAME, doOutGame },
    { GAME_END, nullptr   },
};

// ---- Team-aggregate winner announcement ----
static void onScoreAnnounce(const ScoreTable& t, LightAir_DisplayCtrl& disp) {
    int32_t teamPts[2]   = {0, 0};
    int32_t teamShone[2] = {0, 0};

    for (uint8_t r = 0; r < t.rosterCount; r++) {
        if (!(t.accumMask & (1u << r))) continue;
        uint8_t pid  = t.roster[r];
        uint8_t team = (pid < PlayerDefs::MAX_PLAYER_ID) ? t.teamMap[pid] : 0;
        if (team > 1) team = 0;

        int32_t pts = 0, shn = 0;
        memcpy(&pts, t.slots[r],     4);  // winnerVars[0] = flagsCaptured (MAX)
        memcpy(&shn, t.slots[r] + 4, 4);  // winnerVars[1] = shoneTimes   (MIN)
        teamPts[team]   += pts;
        teamShone[team] += shn;
    }

    uint8_t winner = 0xFF;
    if      (teamPts[1]   > teamPts[0])   winner = 1;
    else if (teamPts[0]   > teamPts[1])   winner = 0;
    else if (teamShone[1] < teamShone[0]) winner = 1;
    else if (teamShone[0] < teamShone[1]) winner = 0;

    if (winner == 0xFF)
        disp.showMessage("Your team tied!", 0);
    else if (winner == myTeam)
        disp.showMessage("Your team won!", 0);
    else
        disp.showMessage("Your team lost!", 0);

    if (winner == 0xFF)
        disp.showMessage("TIE!", 0);
    else if (winner == 0)
        disp.showMessage("TEAM O WINS!", 0);
    else
        disp.showMessage("TEAM X WINS!", 0);
}

} // namespace Flag

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
extern const LightAir_Game game_flag = {
    /* typeId                */ GameTypeId::FLAG,
    /* name                  */ "Flag",
    /* configVars            */ Flag::configVars,          /* configCount            */ 7,
    /* monitorVars           */ Flag::monitorVars,         /* monitorCount           */ 8,
    /* directRadioRules      */ Flag::directRadioRules,    /* directRadioRuleCount   */ 11,
    /* replyRadioRules       */ Flag::replyRadioRules,     /* replyRadioRuleCount    */ 4,
    /* rules                 */ Flag::rules,               /* ruleCount              */ 6,
    /* behaviors             */ Flag::behaviors,           /* behaviorCount          */ 3,
    /* currentState          */ &Flag::gState,             /* initialState           */ Flag::IN_GAME,
    /* onBegin               */ Flag::onBegin,
    /* winnerVars            */ Flag::winnerVars,          /* winnerVarCount         */ 2,
    /* scoringState          */ Flag::GAME_END,
    /* scoreMsgType          */ Flag::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ Flag::onScoreAnnounce,
    /* totemRequirements     */ Flag::totemRequirements,   /* totemRequirementCount  */ 7,
    /* teamCount             */ 2,
    /* teamMap               */ Flag::teamMap,
    /* onEnd                 */ nullptr,
};
