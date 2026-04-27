#include <LightAir.h>
#include <string.h>
#include <stdio.h>
#include "GameTypeIds.h"
#include "../totem-rulesets/CPTotemPolicy.h"

// ================================================================
// Upkeep — two-team game with Control Points (CPs).
//
// States
//   IN_GAME  (0) : player is active; can shine; display: lives/energy/time/score.
//   OUT_GAME (1) : player is down; waits for BASE-beacon respawn; display: time.
//   GAME_END (2) : game over; display: time/score/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT           (0x10) : unicast hit to a target player.
//   MSG_TOTEM_BEACON  (0xF0) : broadcast by all totems periodically (received only).
//   MSG_CP_BEACON     (0x52) : broadcast by CP totems every 2 s.
//                              payload[0]=CPState; [1]=assocPlayer; [2]=assocTeam;
//                              [3]=assocRole; [4..5]=context LE; [6]=countdownRemSecs.
//                              Players reply 0x53 with payload[0]=CPAction::PRESENCE.
//                              team/role are carried in the radio packet header.
//   MSG_CP_SCORE      (0x54) : broadcast by CP totem when it awards 1 point.
//                              payload[0]=assocPlayer, [1]=assocTeam, [2]=assocRole.
//   MSG_SCORE_COLLECT (0x12) : broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x11 reply to MSG_LIT)
//   REPLY_TAKEN  (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE  (2) : target eliminated; lives reached 0.
//   REPLY_DOWN   (3) : target was already OUT_GAME; hit ignored.
//   REPLY_FRIEND (4) : hit rejected — friendly fire is disabled.
//
// CP attachment and point-scoring (runs on CP totem firmware):
//   - CP starts teamless (payload 0xFF); gives no points until first attachment.
//   - Every 2 s, the CP collects 0x55 presence replies and decides:
//       · only subType=1 (team-O) replies  → attach (or stay) to O
//       · only subType=2 (team-X) replies  → attach (or stay) to X
//       · both subtypes or no replies      → maintain current attachment
//   - On team switch: no point awarded; 10 s countdown resets.
//   - After 10 s of unchanged attachment: broadcast MSG_CP_SCORE, restart countdown.
//
// Respawn flow (identical to Teams):
//   On shone: respawnAt = millis() + respawnSecs*1000.
//   doOutGame waits until then and scans for this team's BASE beacon
//   with RSSI ≥ NEAR_BASE_RSSI; when found, sets canRespawn = true.
//
// Scoring and victory:
//   Points come from MSG_CP_SCORE broadcasts tracked locally.
//   The display shows "myPts/enemyPts" for situational awareness.
//   The team with the higher total at game-end wins.
//   Tie-break: team with fewer total shoneTimes.
//   endPoints: if totalCPPoints (O + X combined) reaches this threshold
//   the game ends early; 0 = disabled.
//
// Config vars
//   startLives       : lives at start / after respawn (default 3).
//   respawnSecs      : minimum seconds dead before BASE respawn (default 30).
//   startEnergy      : energy at start / after respawn (default 50).
//   rechargeSecs     : secs after trigger release before energy restored (default 10).
//   gameTime         : total game duration in seconds (default 900).
//   friendlyFire     : 0 = off (default), 1 = on.
//   endPoints        : combined CP-point limit for early GAME_END; 0 = disabled (default 150).
//   cpCountdownSecs  : CP countdown duration in seconds (default 10).
// ================================================================

extern Enlight* enlightPtr;

namespace Upkeep {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Radio message types ----
using RadioMsg::MSG_LIT;            // 0x10
using RadioMsg::MSG_CP_BEACON;      // 0x52
using RadioMsg::MSG_CP_SCORE;       // 0x54
using RadioMsg::MSG_SCORE_COLLECT;  // 0x12
using RadioMsg::MSG_BASE_BEACON;    // 0x56
using RadioMsg::MSG_BONUS_BEACON;   // 0x5E
using RadioMsg::MSG_MALUS_BEACON;   // 0x60

// ---- Reply sub-types for MSG_LIT ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN  = 1,
    REPLY_SHONE  = 2,
    REPLY_DOWN   = 3,
    REPLY_FRIEND = 4,
    REPLY_IMMUNE = 5,
};

// ---- CP display-key sentinels (stored in cpState[] for change detection) ----
static constexpr uint8_t CP_KEY_NEUTRAL   = 0xFF;  // IDLE / COOLDOWN / INACTIVE / etc.
static constexpr uint8_t CP_KEY_CONTESTED = 0xFE;  // CONTESTED

// ---- RSSI proximity thresholds ----
static constexpr int8_t  NEAR_CP_RSSI    = -65;  // ~3 m indoors for CP presence
static constexpr int8_t  NEAR_BASE_RSSI  = -60;  // ~2 m indoors for BASE respawn
static constexpr uint32_t HIT_IMMUNITY_MS = 3000;

// ---- Config variables ----
static int startLives       = 3;
static int respawnSecs      = 30;
static int startEnergy      = 50;
static int rechargeSecs     = 10;
static int gameTime         = 900;
static int friendlyFire     = 0;
static int endPoints        = 150;
static int cpCountdownSecs  = 10;

// ---- Runtime variables ----
static int lives          = 3;
static int energy         = 50;
static int gameTimeLeft   = 900;
static int shoneTimes     = 0;
static int energySpent    = 0;
static int teamOPoints    = 0;   // CP points scored by team O (all players track both)
static int teamXPoints    = 0;   // CP points scored by team X
static int myTeamCPPoints = 0;   // = teamOPoints or teamXPoints; used by WinnerVar

// Score string shown on display: "myPts/enemyPts"
static char teamScoreStr[9] = "0/0";

// Compact display key per CP; updated from MSG_CP_BEACON.
// 0 = team-O owned, 1 = team-X owned, CP_KEY_CONTESTED, CP_KEY_NEUTRAL.
static uint8_t cpState[6];

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;
static bool     canRespawn;
static uint8_t  myTeam;         // 0=O, 1=X; loaded from runner in onBegin
static uint8_t  myPlayerId;     // hardware player ID (1–15)
static uint8_t  teamMap[PlayerDefs::MAX_PLAYER_ID];  // per-player team index; filled from config blob

// Moved from doInGame static locals so they reset correctly on game restart.
static bool     triggerWasActive = false;
static uint32_t releaseAt        = 0;
static uint32_t litAt[PlayerDefs::MAX_PLAYER_ID];

// ---- Totem device-ID slots (populated in onBegin from runner) ----
static uint8_t cpIds[6]     = {};
static uint8_t baseO_ids[3] = {};
static uint8_t baseX_ids[3] = {};
static uint8_t base_ids[3]  = {};
static uint8_t bonusIds[GameDefaults::MAX_PARTICIPANTS] = {};
static uint8_t malusIds[GameDefaults::MAX_PARTICIPANTS] = {};
static uint8_t numActiveCPs      = 0;
static uint8_t numActiveBonuses  = 0;
static uint8_t numActiveMaluses  = 0;

// ---- Config vars ----
static const ConfigVar configVars[] = {
    //name              value               min   max   step
    { "Lives",         &startLives,         1,    5,    1  },
    { "Respawn",       &respawnSecs,        5,   120,   5  },
    { "Energy",        &startEnergy,       10,   100,  10  },
    { "Recharge",      &rechargeSecs,       0,    20,   5  },
    { "Time",          &gameTime,          60,   900,  60  },
    { "FriendlyFire",  &friendlyFire,       0,     1,   1  },
    { "EndPoints",     &endPoints,          0,   500,  50  },
    { "CP Secs",       &cpCountdownSecs,    5,    60,   5  },
};

// ---- Monitor vars ----
// teamScoreStr shows "myPts/enemyPts" in both IN_GAME and GAME_END.
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::Int("Lives",  &lives,        1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy", &energy,       1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",   &gameTimeLeft, (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Str("Points", teamScoreStr,  1u<<IN_GAME,                  ICON_SCORE,  1, 1),
    // GAME_END display
    MonitorVar::Int("Time",   &gameTime,     1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Str("Points", teamScoreStr,  1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy", &energySpent,  1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",  &shoneTimes,   1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- Activation payload builders ----
// Each fills buf[0..14] (15 bytes); roleId is prepended by the game runner.

static uint8_t buildCpPayload(uint8_t* buf, uint8_t /*maxLen*/) {
    constexpr uint32_t mode =
        CPPolicy::ASSOCIATION_TEAM           |
        CPPolicy::CONTEST_PAUSE              |
        CPPolicy::POSTSCORE_REPEAT           |
        CPPolicy::RESOLUTION_PREV_WINS       |
        CPPolicy::TIMER_SIMPLE               |
        CPPolicy::FLAG_TIMER_PAUSE_IN_CONTEST;
    buf[0]  = (uint8_t)( mode        & 0xFFu);
    buf[1]  = (uint8_t)((mode >>  8) & 0xFFu);
    buf[2]  = (uint8_t)((mode >> 16) & 0xFFu);
    buf[3]  = (uint8_t)((mode >> 24) & 0xFFu);
    buf[4]  = (uint8_t)( cpCountdownSecs       & 0xFFu);
    buf[5]  = (uint8_t)((cpCountdownSecs >> 8) & 0xFFu);
    buf[6]  = 0; buf[7]  = 0;   // initial context
    buf[8]  = 0; buf[9]  = 0; buf[10] = 0;  // cooldown/suspend/prime (defaults)
    buf[11] = 0;               // beaconMsgType (0 → MSG_CP_BEACON default)
    buf[12] = 0;               // maxPoints (unlimited)
    buf[13] = 0;               // beaconInterval (0 → 2 s default)
    buf[14] = 0xFF;            // totemTeam (teamless CP)
    return 15;
}

// TIMER_IMMEDIATE + POSTSCORE_LOOP: any valid player triggers instant score then totem resets.
// FLAG_ENEMY_TEAM_ONLY filters by totemTeam so only the opposite team's players associate.
static uint8_t buildBaseOPayload(uint8_t* buf, uint8_t /*maxLen*/) {
    constexpr uint32_t mode =
        CPPolicy::ASSOCIATION_ANY      |
        CPPolicy::CONTEST_HOLD         |
        CPPolicy::POSTSCORE_LOOP       |
        CPPolicy::RESOLUTION_PREV_WINS |
        CPPolicy::TIMER_IMMEDIATE      |
        CPPolicy::FLAG_ENEMY_TEAM_ONLY;
    buf[0]  = (uint8_t)( mode        & 0xFFu);
    buf[1]  = (uint8_t)((mode >>  8) & 0xFFu);
    buf[2]  = (uint8_t)((mode >> 16) & 0xFFu);
    buf[3]  = (uint8_t)((mode >> 24) & 0xFFu);
    buf[4]  = 0; buf[5]  = 0;
    buf[6]  = 0; buf[7]  = 0;
    buf[8]  = 0; buf[9]  = 0; buf[10] = 0;
    buf[11] = MSG_BASE_BEACON;
    buf[12] = 0;
    buf[13] = 0;
    buf[14] = 1;    // totemTeam = X → only team-O players associate
    return 15;
}

static uint8_t buildBaseXPayload(uint8_t* buf, uint8_t /*maxLen*/) {
    constexpr uint32_t mode =
        CPPolicy::ASSOCIATION_ANY      |
        CPPolicy::CONTEST_HOLD         |
        CPPolicy::POSTSCORE_LOOP       |
        CPPolicy::RESOLUTION_PREV_WINS |
        CPPolicy::TIMER_IMMEDIATE      |
        CPPolicy::FLAG_ENEMY_TEAM_ONLY;
    buf[0]  = (uint8_t)( mode        & 0xFFu);
    buf[1]  = (uint8_t)((mode >>  8) & 0xFFu);
    buf[2]  = (uint8_t)((mode >> 16) & 0xFFu);
    buf[3]  = (uint8_t)((mode >> 24) & 0xFFu);
    buf[4]  = 0; buf[5]  = 0;
    buf[6]  = 0; buf[7]  = 0;
    buf[8]  = 0; buf[9]  = 0; buf[10] = 0;
    buf[11] = MSG_BASE_BEACON;
    buf[12] = 0;
    buf[13] = 0;
    buf[14] = 0;    // totemTeam = O → only team-X players associate
    return 15;
}

static uint8_t buildBaseAnyPayload(uint8_t* buf, uint8_t /*maxLen*/) {
    constexpr uint32_t mode =
        CPPolicy::ASSOCIATION_ANY      |
        CPPolicy::CONTEST_HOLD         |
        CPPolicy::POSTSCORE_LOOP       |
        CPPolicy::RESOLUTION_PREV_WINS |
        CPPolicy::TIMER_IMMEDIATE;     // no FLAG_ENEMY_TEAM_ONLY; accepts any player
    buf[0]  = (uint8_t)( mode        & 0xFFu);
    buf[1]  = (uint8_t)((mode >>  8) & 0xFFu);
    buf[2]  = (uint8_t)((mode >> 16) & 0xFFu);
    buf[3]  = (uint8_t)((mode >> 24) & 0xFFu);
    buf[4]  = 0; buf[5]  = 0;
    buf[6]  = 0; buf[7]  = 0;
    buf[8]  = 0; buf[9]  = 0; buf[10] = 0;
    buf[11] = MSG_BASE_BEACON;
    buf[12] = 0;
    buf[13] = 0;
    buf[14] = 0xFF;  // totemTeam = teamless
    return 15;
}

static uint8_t buildBonusPayload(uint8_t* buf, uint8_t /*maxLen*/) {
    constexpr uint32_t mode =
        CPPolicy::ASSOCIATION_ANY      |
        CPPolicy::CONTEST_HOLD         |
        CPPolicy::POSTSCORE_COOLDOWN   |
        CPPolicy::RESOLUTION_PREV_WINS |
        CPPolicy::TIMER_IMMEDIATE;
    buf[0]  = (uint8_t)( mode        & 0xFFu);
    buf[1]  = (uint8_t)((mode >>  8) & 0xFFu);
    buf[2]  = (uint8_t)((mode >> 16) & 0xFFu);
    buf[3]  = (uint8_t)((mode >> 24) & 0xFFu);
    buf[4]  = 0; buf[5]  = 0;
    buf[6]  = 0; buf[7]  = 0;
    buf[8]  = 30;               // cooldown_secs = 30 s
    buf[9]  = 0; buf[10] = 0;
    buf[11] = MSG_BONUS_BEACON;
    buf[12] = 0;
    buf[13] = 0;
    buf[14] = 0xFF;
    return 15;
}

static uint8_t buildMalusPayload(uint8_t* buf, uint8_t maxLen) {
    uint8_t n = buildBonusPayload(buf, maxLen);
    buf[11] = MSG_MALUS_BEACON;
    return n;
}

// ---- Totem requirements ----
static const LightAir_TotemRequirement totemRequirements[] = {
    { TotemRoleId::CP,     3, 6,  nullptr, buildCpPayload      },
    { TotemRoleId::BASE_O, 1, 3,  nullptr, buildBaseOPayload   },
    { TotemRoleId::BASE_X, 1, 3,  nullptr, buildBaseXPayload   },
    { TotemRoleId::BASE,   0, 3,  nullptr, buildBaseAnyPayload },
    { TotemRoleId::BONUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr, buildBonusPayload },
    { TotemRoleId::MALUS,  0, GameDefaults::MAX_PARTICIPANTS, nullptr, buildMalusPayload },
};

// ---- Helpers ----

// Returns true if senderId is a known base for the local player's team (or teamless).
static bool isMyTeamBase(uint8_t senderId) {
    const uint8_t* myBases = (myTeam == 0) ? baseO_ids : baseX_ids;
    for (int i = 0; i < 3; i++) {
        if (myBases[i] != 0 && myBases[i] == senderId) return true;
        if (base_ids[i] != 0 && base_ids[i] == senderId) return true;
    }
    return false;
}

// Returns true if senderId is any registered base totem (any team).
static bool isBaseTotem(uint8_t senderId) {
    for (int i = 0; i < 3; i++) {
        if (baseO_ids[i] != 0 && baseO_ids[i] == senderId) return true;
        if (baseX_ids[i] != 0 && baseX_ids[i] == senderId) return true;
        if (base_ids[i]  != 0 && base_ids[i]  == senderId) return true;
    }
    return false;
}

static bool isOpponent(uint8_t targetId) {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return false;
    return teamMap[targetId] != myTeam;
}

// Returns the index (0–5) of a known CP with the given sender ID, or -1.
static int8_t cpIndex(uint8_t senderId) {
    for (int i = 0; i < numActiveCPs; i++) {
        if (cpIds[i] != 0 && cpIds[i] == senderId)
            return (int8_t)i;
    }
    return -1;
}

static int8_t bonusIndex(uint8_t senderId) {
    for (int i = 0; i < numActiveBonuses; i++)
        if (bonusIds[i] != 0 && bonusIds[i] == senderId) return (int8_t)i;
    return -1;
}

static int8_t malusIndex(uint8_t senderId) {
    for (int i = 0; i < numActiveMaluses; i++)
        if (malusIds[i] != 0 && malusIds[i] == senderId) return (int8_t)i;
    return -1;
}

// Refresh the score display string from current totals.
static void refreshScoreStr() {
    int myPts    = (myTeam == 0) ? teamOPoints : teamXPoints;
    int enemyPts = (myTeam == 0) ? teamXPoints : teamOPoints;
    snprintf(teamScoreStr, sizeof(teamScoreStr), "%d/%d", myPts, enemyPts);
}

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio& radio, LightAir_UICtrl* ui,
                    const LightAir_GameRunner& runner) {
    lives            = startLives;
    energy           = startEnergy;
    gameTimeLeft     = gameTime;
    shoneTimes       = 0;
    energySpent      = 0;
    teamOPoints      = 0;
    teamXPoints      = 0;
    myTeamCPPoints   = 0;
    canRespawn       = false;
    respawnAt        = 0;
    lastTickAt       = millis();
    triggerWasActive = false;
    releaseAt        = 0;
    memset(litAt, 0, sizeof(litAt));

    for (uint8_t i = 0; i < 6; i++) cpState[i] = CP_KEY_NEUTRAL;
    snprintf(teamScoreStr, sizeof(teamScoreStr), "0/0");

    myTeam     = runner.teamOf(radio.playerId());
    myPlayerId = radio.playerId();

    ui->trigger(LightAir_UICtrl::UIEvent::GameStart);

    numActiveCPs = 0;
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t id = runner.totemIdForRole(TotemRoleId::CP, i);
        if (id == 0) break;
        cpIds[numActiveCPs++] = id;
    }
    for (uint8_t i = 0; i < 3; i++) {
        baseO_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_O, i);
        baseX_ids[i] = runner.totemIdForRole(TotemRoleId::BASE_X, i);
        base_ids[i]  = runner.totemIdForRole(TotemRoleId::BASE,   i);
    }
    numActiveBonuses = 0;
    for (uint8_t i = 0; i < GameDefaults::MAX_PARTICIPANTS; i++) {
        uint8_t id = runner.totemIdForRole(TotemRoleId::BONUS, i);
        if (id == 0) break;
        bonusIds[numActiveBonuses++] = id;
    }
    numActiveMaluses = 0;
    for (uint8_t i = 0; i < GameDefaults::MAX_PARTICIPANTS; i++) {
        uint8_t id = runner.totemIdForRole(TotemRoleId::MALUS, i);
        if (id == 0) break;
        malusIds[numActiveMaluses++] = id;
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

// ---- DirectRadioRule actions ----
static void onLitTaken(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput& out) {
    lives--;
    if (pkt.senderId < PlayerDefs::MAX_PLAYER_ID) litAt[pkt.senderId] = millis();
    out.ui.trigger(LightAir_UICtrl::UIEvent::GotLit);
}
static void onLitShone(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    lives--;
    if (pkt.senderId < PlayerDefs::MAX_PLAYER_ID) litAt[pkt.senderId] = millis();
}

static void onCpScore(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    if (cpIndex(pkt.senderId) < 0) return;
    if (pkt.payloadLen < 2) return;
    uint8_t scoringTeam = pkt.payload[1];    // [0]=assocPlayer, [1]=assocTeam
    if      (scoringTeam == 0) teamOPoints++;
    else if (scoringTeam == 1) teamXPoints++;
    else return;
    myTeamCPPoints = (myTeam == 0) ? teamOPoints : teamXPoints;
    refreshScoreStr();
}

static void onBaseScore(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    if (!isBaseTotem(pkt.senderId))    return;
    if (pkt.payloadLen < 1)            return;
    if (pkt.payload[0] != myPlayerId)  return;
    canRespawn = true;
}

static void onBonusScore(const RadioPacket& pkt, LightAir_DisplayCtrl& disp, GameOutput&) {
    if (bonusIndex(pkt.senderId) < 0) return;
    if (pkt.payloadLen < 1)           return;
    if (pkt.payload[0] != myPlayerId)  return;
    energy = startEnergy;
    disp.showMessage("Bonus!", 1500);
}

static void onMalusScore(const RadioPacket& pkt, LightAir_DisplayCtrl& disp, GameOutput&) {
    if (malusIndex(pkt.senderId) < 0) return;
    if (pkt.payloadLen < 1)           return;
    if (pkt.payload[0] != myPlayerId)  return;
    if (lives > 1) lives--;
    disp.showMessage("Malus!", 1500);
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType          condition            replySubType  onReceive
    { IN_GAME,  MSG_LIT,        litAndTakenAndValid, REPLY_TAKEN,  onLitTaken   },
    { IN_GAME,  MSG_LIT,        litAndShoneAndValid, REPLY_SHONE,  onLitShone   },
    { IN_GAME,  MSG_LIT,        litButFriendly,      REPLY_FRIEND, nullptr      },
    { IN_GAME,  MSG_LIT,        litButImmune,        REPLY_IMMUNE, nullptr      },
    { OUT_GAME, MSG_LIT,        nullptr,             REPLY_DOWN,   nullptr      },
    { IN_GAME,  MSG_CP_SCORE,   nullptr,             0,            onCpScore    },
    { OUT_GAME, MSG_CP_SCORE,   nullptr,             0,            onCpScore    },
    { OUT_GAME, MSG_CP_SCORE,   nullptr,             0,            onBaseScore  },
    { IN_GAME,  MSG_CP_SCORE,   nullptr,             0,            onBonusScore },
    { IN_GAME,  MSG_CP_SCORE,   nullptr,             0,            onMalusScore },
};

// ---- ReplyRadioRules ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Taken);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
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

// ---- Winner election rules ----
static const WinnerVar winnerVars[] = {
    { &myTeamCPPoints, WinnerDir::MAX },  // primary: team with more CP points wins
    { &shoneTimes,     WinnerDir::MIN },  // tie-break: fewer times shone
};

// ---- Per-second ticker ----
static void tickGameTime() {
    uint32_t now = millis();
    if (now - lastTickAt >= 1000) {
        lastTickAt += 1000;
        if (gameTimeLeft > 0) gameTimeLeft--;
    }
}

// Derive a compact display key from a CP beacon packet.
// Returns 0/1 = owning team, CP_KEY_CONTESTED, or CP_KEY_NEUTRAL.
static uint8_t cpDisplayKey(const RadioPacket& pkt) {
    if (pkt.payloadLen < 3) return CP_KEY_NEUTRAL;
    if ((CPState)pkt.payload[0] == CPState::CONTESTED) return CP_KEY_CONTESTED;
    uint8_t assocTeam = pkt.payload[2];
    if (assocTeam == 0xFF) return CP_KEY_NEUTRAL;
    return assocTeam;  // 0 = team-O, 1 = team-X
}

// ---- Scan CP beacons: update ownership cache and (if sendPresence) reply with presence ----
// Called from both doInGame (sendPresence=true) and doOutGame (sendPresence=false).
// Ownership changes trigger a tray message and a UI event so the player is informed.
static void scanCpBeacons(const RadioReport& radio, LightAir_DisplayCtrl& disp,
                           GameOutput& out, bool sendPresence) {
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_CP_BEACON)                   continue;

        int8_t idx = cpIndex(ev.packet.senderId);
        if (idx < 0) continue;

        uint8_t newKey = cpDisplayKey(ev.packet);
        if (newKey != cpState[idx]) {
            cpState[idx] = newKey;
            char msg[20];
            if (newKey == CP_KEY_NEUTRAL) {
                snprintf(msg, sizeof(msg), "CP %d neutral", idx + 1);
                disp.showMessage(msg, 3000);
            } else if (newKey == CP_KEY_CONTESTED) {
                snprintf(msg, sizeof(msg), "CP %d contested!", idx + 1);
                disp.showMessage(msg, 3000);
            } else {
                snprintf(msg, sizeof(msg), "CP %d->Team %c!", idx + 1,
                         newKey == 0 ? 'O' : 'X');
                disp.showMessage(msg, 3000);
                if (newKey == myTeam)
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagReturn);
                else
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagTaken);
            }
        }

        // Reply with CPAction::PRESENCE; the radio layer carries team in the packet header.
        if (sendPresence && ev.rssi >= NEAR_CP_RSSI) {
            uint8_t pres = (uint8_t)CPAction::PRESENCE;
            out.radio.replyWithPayload(ev.packet, &pres, 1);
        }
    }
}

// ---- Transition conditions ----
static bool gameTimeExpired(const InputReport&, const RadioReport&) {
    return gameTimeLeft <= 0;
}
static bool endPointsReached(const InputReport&, const RadioReport&) {
    return endPoints > 0 && (teamOPoints + teamXPoints) >= endPoints;
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
    memset(litAt, 0, sizeof(litAt));
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
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) {
        if (isOpponent(r.id) || friendlyFire)
            out.radio.sendTo(r.id, MSG_LIT);
    }

    bool triggerActive = false;

    for (uint8_t i = 0; i < inp.buttonCount; i++) {
        if (inp.buttons[i].id != InputDefaults::TRIG_1_ID) continue;
        ButtonState s = inp.buttons[i].state;
        if (s == ButtonState::PRESSED || s == ButtonState::HELD) {
            triggerActive = true;
            if (energy > 0) {
                energy--;
                energySpent++;
                enlightPtr->run();
                out.ui.triggerEnlight(enlightPtr->cycleTime());
            }
        }
    }

    if (triggerWasActive && !triggerActive) releaseAt = millis();
    triggerWasActive = triggerActive;

    if (!triggerActive && energy < startEnergy) {
        if ((millis() - releaseAt) >= (uint32_t)rechargeSecs * 1000)
            energy = startEnergy;
    }
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl& disp, GameOutput& out) {
    tickGameTime();
    scanCpBeacons(radio, disp, out, false);

    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon; reply with PRESENCE so the CPTotem
    // awards a score → onBaseScore() sets canRespawn = true.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
        if (!isMyTeamBase(ev.packet.senderId))                    continue;
        if (ev.rssi            < NEAR_BASE_RSSI)                  continue;
        uint8_t pres = (uint8_t)CPAction::PRESENCE;
        out.radio.replyWithPayload(ev.packet, &pres, 1);
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
    // Each player reports myTeamCPPoints and shoneTimes.
    // Aggregate: max CP points per team (tolerates missed MSG_CP_SCORE packets),
    //            sum shoneTimes per team.
    int32_t cpPts[2] = {0, 0};
    int32_t shone[2] = {0, 0};

    for (uint8_t pid = 1; pid < PlayerDefs::MAX_PLAYER_ID; pid++) {
        if (!(t.accumMask & (1u << pid))) continue;
        uint8_t team = t.teamMap[pid];
        if (team > 1) team = 0;

        int32_t cp = 0, shn = 0;
        memcpy(&cp,  t.slots[pid],     4);  // winnerVars[0] = myTeamCPPoints (MAX)
        memcpy(&shn, t.slots[pid] + 4, 4);  // winnerVars[1] = shoneTimes     (MIN)

        if (cp > cpPts[team]) cpPts[team] = cp;
        shone[team] += shn;
    }

    uint8_t winner = 0xFF;
    if      (cpPts[1] > cpPts[0])  winner = 1;
    else if (cpPts[0] > cpPts[1])  winner = 0;
    else if (shone[1] < shone[0])  winner = 1;
    else if (shone[0] < shone[1])  winner = 0;

    if (winner == 0xFF)
        disp.showMessage("Your team tied!", 0);
    else if (winner == myTeam)
        disp.showMessage("Your team won!", 0);
    else
        disp.showMessage("Your team lost!", 0);

    if (winner == 0xFF)   disp.showMessage("TIE!", 0);
    else if (winner == 0) disp.showMessage("TEAM O WINS!", 0);
    else                  disp.showMessage("TEAM X WINS!", 0);
}

} // namespace Upkeep

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
extern const LightAir_Game game_upkeep = {
    /* typeId                */ GameTypeId::UPKEEP,
    /* name                  */ "Upkeep",
    /* configVars            */ Upkeep::configVars,
    /* configCount           */ 8,
    /* monitorVars           */ Upkeep::monitorVars,
    /* monitorCount          */ sizeof(Upkeep::monitorVars) / sizeof(*Upkeep::monitorVars),
    /* directRadioRules      */ Upkeep::directRadioRules,
    /* directRadioRuleCount  */ sizeof(Upkeep::directRadioRules) / sizeof(*Upkeep::directRadioRules),
    /* replyRadioRules       */ Upkeep::replyRadioRules,
    /* replyRadioRuleCount   */ sizeof(Upkeep::replyRadioRules) / sizeof(*Upkeep::replyRadioRules),
    /* rules                 */ Upkeep::rules,
    /* ruleCount             */ sizeof(Upkeep::rules) / sizeof(*Upkeep::rules),
    /* behaviors             */ Upkeep::behaviors,
    /* behaviorCount         */ sizeof(Upkeep::behaviors) / sizeof(*Upkeep::behaviors),
    /* currentState          */ &Upkeep::gState,
    /* initialState          */ Upkeep::IN_GAME,
    /* onBegin               */ Upkeep::onBegin,
    /* winnerVars            */ Upkeep::winnerVars,
    /* winnerVarCount        */ sizeof(Upkeep::winnerVars) / sizeof(*Upkeep::winnerVars),
    /* scoringState          */ Upkeep::GAME_END,
    /* scoreMsgType          */ Upkeep::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ Upkeep::onScoreAnnounce,
    /* totemRequirements     */ Upkeep::totemRequirements,
    /* totemRequirementCount */ 6,
    /* teamCount             */ 2,
    /* teamMap               */ Upkeep::teamMap,
    /* onEnd                 */ nullptr,
};
