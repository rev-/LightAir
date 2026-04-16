#include <LightAir.h>
#include <string.h>
#include "GameTypeIds.h"

// ================================================================
// Teams — two-team game: O vs X.
//
// States
//   IN_GAME  (0) : player is active; can shine; display: lives/energy/time/points.
//   OUT_GAME (1) : player is down; display: time.
//   GAME_END (2) : game over; display: time/points/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT           (0x10) : unicast hit to a target player.
//   MSG_TOTEM_BEACON  (0xF0) : broadcast by all totems periodically (received only).
//   MSG_SCORE_COLLECT (0x12) : broadcast per-player scores during GAME_END.
//   MSG_POINT_REPORT  (0x14) : broadcast by a player on each point scored, so teammates
//                              can increment their local myTeamPoints counter.
//
// Reply sub-types (payload[0] of the 0x11 reply)
//   REPLY_TAKEN  (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE  (2) : target eliminated; lives reached 0.
//   REPLY_DOWN   (3) : target was already OUT_GAME; hit ignored.
//   REPLY_FRIEND (4) : hit rejected — sender and receiver are on the same team
//                      and friendly fire is disabled.
//
// Respawn flow
//   On being shone: respawnAt = millis() + respawnSecs*1000; canRespawn = false.
//   doOutGame polls incoming MSG_TOTEM_BEACON events.  If the beacon comes from
//   one of this player's team BASE totems AND its RSSI >= NEAR_RSSI_THRESHOLD
//   AND millis() >= respawnAt, canRespawn is set true.  The state machine then
//   fires onRespawn on the next cycle.
//   This prevents both instant respawn (timer gate) and respawn from a wrong or
//   distant base (team + RSSI gates).
//
// Scoring
//   Sender awards itself 1 point per REPLY_SHONE (opponent eliminated).
//   Winner is the team with the higher aggregate point total.
//   Tie-breaker: team with fewer aggregate shoneTimes.
//   Implemented via onScoreAnnounce (custom team-aggregate logic).
//
// Config vars
//   startLives   : lives at start / after respawn (default 3).
//   respawnSecs  : minimum seconds dead before a BASE beacon can trigger respawn (default 30).
//   startEnergy  : energy at start / after respawn (default 50).
//   rechargeSecs : secs after trigger release before energy restored (default 10).
//   gameTime     : total game duration in seconds (default 900).
//   friendlyFire : 0 = off (default), 1 = on.
//   endPoints    : team-kill limit to trigger early GAME_END; 0 = disabled (default).
// ================================================================

extern Enlight* enlightPtr;

namespace Teams {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types ----
using RadioMsg::MSG_LIT;            // 0x10
using RadioMsg::MSG_SCORE_COLLECT;  // 0x12
using RadioMsg::MSG_POINT_REPORT;   // 0x14
using RadioMsg::MSG_BASE_BEACON;    // 0x56

// ---- Reply sub-types ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN  = 1,
    REPLY_SHONE  = 2,
    REPLY_DOWN   = 3,
    REPLY_FRIEND = 4,
};

// ---- RSSI proximity threshold ----
// Packets from a BASE with RSSI below this value are ignored for respawn.
// -60 dBm corresponds to approximately 2 m indoors at 2.4 GHz.
static constexpr int8_t NEAR_RSSI_THRESHOLD = -60;

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
static int points        = 0;
static int energySpent   = 0;
static int shoneTimes    = 0;
static int myTeamPoints  = 0;  // team-aggregate kills known locally

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;    // millis() before which BASE beacons are ignored
static bool     canRespawn;   // set true by doOutGame when timer + RSSI condition met
static bool     triggerWasActive = false;
static uint32_t releaseAt        = 0;
static uint8_t  myTeam;       // 0=O, 1=X; loaded from runner in onBegin
static uint8_t  teamMap[PlayerDefs::MAX_PLAYER_ID];  // per-player team index; filled from config blob

// ---- Totem device-ID slots (populated in onBegin from runner) ----
static uint8_t baseO_ids[4] = {};   // team-O base device IDs
static uint8_t baseX_ids[4] = {};   // team-X base device IDs

// ---- Config vars (startup menu) ----
static const ConfigVar configVars[] = {
    //name             value            min   max   step
    { "Lives",        &startLives,     1,    5,    1  },
    { "Respawn",      &respawnSecs,    5,    120,  5  },
    { "Energy",       &startEnergy,    10,   100,  10 },
    { "Recharge",     &rechargeSecs,   0,    20,   5  },
    { "Time",         &gameTime,       60,   900,  60 },
    { "FriendlyFire", &friendlyFire,   0,    1,    1  },
    { "EndPoints",    &endPoints,      0,    50,   5  },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME
    MonitorVar::Int("Lives",  &lives,        1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy", &energy,       1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",   &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Points", &points,       1u<<IN_GAME,                  ICON_SCORE,  1, 1),
    // GAME_END
    MonitorVar::Int("Time",   &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points", &points,       1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy", &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",  &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- Totem requirements ----
static const LightAir_TotemRequirement totemRequirements[] = {
    { TotemRoleId::BASE_O, 0, 4, nullptr },
    { TotemRoleId::BASE_X, 0, 4, nullptr },
    { TotemRoleId::BASE,   0, 4, nullptr },
    { TotemRoleId::BONUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr },
    { TotemRoleId::MALUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr },
};

// ---- Helper: is targetId on the opposing team? ----
static bool isOpponent(uint8_t targetId) {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return false;
    return teamMap[targetId] != myTeam;
}

// ---- Helper: is senderId on the same team as this player? ----
static bool isMyTeammate(uint8_t senderId) {
    if (senderId >= PlayerDefs::MAX_PLAYER_ID) return false;
    return teamMap[senderId] == myTeam;
}

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio& radio, LightAir_UICtrl* ui,
                    const LightAir_GameRunner& runner) {
    lives         = startLives;
    energy        = startEnergy;
    gameTimeLeft  = gameTime;
    points        = 0;
    energySpent   = 0;
    shoneTimes    = 0;
    myTeamPoints  = 0;
    canRespawn       = false;
    respawnAt        = 0;
    lastTickAt       = millis();
    triggerWasActive = false;
    releaseAt        = 0;

    myTeam = runner.teamOf(radio.playerId());

    for (uint8_t i = 0; i < 4; i++) {
        baseO_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_O, i);
        baseX_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_X, i);
    }
}

// ---- DirectRadioRule conditions ----
static bool litAndTakenAndValid(const RadioPacket& pkt) {
    return lives > 1 && (pkt.team != myTeam || friendlyFire);
}
static bool litAndShoneAndValid(const RadioPacket& pkt) {
    return lives <= 1 && (pkt.team != myTeam || friendlyFire);
}
static bool litButFriendly(const RadioPacket& pkt) {
    return pkt.team == myTeam && !friendlyFire;
}

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }
static void onLitShone(const RadioPacket&, LightAir_DisplayCtrl&, GameOutput&) { lives--; }

static void onPointReport(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    if (isMyTeammate(pkt.senderId))
        myTeamPoints++;
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType            condition           replySubType  onReceive
    { IN_GAME,  MSG_LIT,           litAndTakenAndValid, REPLY_TAKEN,  onLitTaken   },
    { IN_GAME,  MSG_LIT,           litAndShoneAndValid, REPLY_SHONE,  onLitShone   },
    { IN_GAME,  MSG_LIT,           litButFriendly,      REPLY_FRIEND, nullptr      },
    { OUT_GAME, MSG_LIT,           nullptr,             REPLY_DOWN,   nullptr      },
    { IN_GAME,  MSG_POINT_REPORT,  nullptr,             0,            onPointReport },
    { OUT_GAME, MSG_POINT_REPORT,  nullptr,             0,            onPointReport },
};

// ---- ReplyRadioRule handlers ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Taken);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    points++;
    myTeamPoints++;
    out.radio.broadcast(MSG_POINT_REPORT, nullptr, 0);
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyFriend(const RadioPacket&, const RadioPacket&,
                          LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Friend);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType        condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN,  nullptr, onReplyTaken  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE,  nullptr, onReplyShone  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_FRIEND, nullptr, onReplyFriend },
};

// ---- Winner election (used for individual score slot format; aggregation in onScoreAnnounce) ----
static const WinnerVar winnerVars[] = {
    { &points,     WinnerDir::MAX },
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

    if (triggerWasActive && !triggerActive)
        releaseAt = millis();
    triggerWasActive = triggerActive;

    if (!triggerActive && energy < startEnergy) {
        if (rechargeSecs == 0)
            energy = startEnergy;
        else if ((millis() - releaseAt) / 1000 >= (uint32_t)rechargeSecs)
            energy = startEnergy;
    }

    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) {
        // Only fire if target is an opponent (or friendly fire is enabled).
        if (isOpponent(r.id) || friendlyFire)
            out.radio.sendTo(r.id, MSG_LIT);
    }
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

    // Minimum respawn timer: ignore beacons until the wait has elapsed.
    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon: must belong to this player's team
    // (or be teamless, payload[0]==0xFF) and have RSSI above the proximity
    // threshold (~2 m indoors).
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
        if (ev.packet.payloadLen < 1)                             continue;
        if (ev.packet.payload[0] != myTeam &&
            ev.packet.payload[0] != 0xFF)                         continue;
        if (ev.rssi < NEAR_RSSI_THRESHOLD)                        continue;
        canRespawn = true;
        // Reply so the BASE totem can show a Respawn animation.
        // subType 1 = team-O, 2 = team-X.
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
        memcpy(&pts, t.slots[r],     4);  // winnerVars[0] = points  (MAX)
        memcpy(&shn, t.slots[r] + 4, 4);  // winnerVars[1] = shoneTimes (MIN)
        teamPts[team]   += pts;
        teamShone[team] += shn;
    }

    // Primary: higher team points wins; tie-break: fewer team shoneTimes.
    uint8_t winner = 0xFF;  // 0xFF = genuine tie
    if      (teamPts[1]   > teamPts[0])   winner = 1;
    else if (teamPts[0]   > teamPts[1])   winner = 0;
    else if (teamShone[1] < teamShone[0]) winner = 1;
    else if (teamShone[0] < teamShone[1]) winner = 0;

    // Personalised line (pushed first → ends up on bottom row).
    if (winner == 0xFF)
        disp.showMessage("Your team tied!", 0);
    else if (winner == myTeam)
        disp.showMessage("Your team won!", 0);
    else
        disp.showMessage("Your team lost!", 0);

    // Top-row winner announcement.
    if (winner == 0xFF)
        disp.showMessage("TIE!", 0);
    else if (winner == 0)
        disp.showMessage("TEAM O WINS!", 0);
    else
        disp.showMessage("TEAM X WINS!", 0);
}

} // namespace Teams

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
extern const LightAir_Game game_teams = {
    /* typeId                */ GameTypeId::TEAMS,
    /* name                  */ "Teams",
    /* configVars            */ Teams::configVars,         /* configCount            */ 7,
    /* monitorVars           */ Teams::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ Teams::directRadioRules,   /* directRadioRuleCount   */ 6,
    /* replyRadioRules       */ Teams::replyRadioRules,    /* replyRadioRuleCount    */ 3,
    /* rules                 */ Teams::rules,              /* ruleCount              */ 6,
    /* behaviors             */ Teams::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &Teams::gState,            /* initialState           */ Teams::IN_GAME,
    /* onBegin               */ Teams::onBegin,
    /* winnerVars            */ Teams::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ Teams::GAME_END,
    /* scoreMsgType          */ Teams::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ Teams::onScoreAnnounce,
    /* totemRequirements     */ Teams::totemRequirements,  /* totemRequirementCount  */ 5,
    /* teamCount             */ 2,
    /* teamMap               */ Teams::teamMap,
    /* onEnd                 */ nullptr,
};
