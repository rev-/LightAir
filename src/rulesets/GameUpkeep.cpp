#include <LightAir.h>
#include <string.h>
#include <stdio.h>
#include "TotemProtocol.h"

// ================================================================
// Upkeep — two-team game with Control Points (CPs).
//
// States
//   IN_GAME  (0) : player is active; can shine; display: lives/energy/time/score.
//   OUT_GAME (1) : player is down; waits for BASE-beacon respawn; display: time.
//   GAME_END (2) : game over; display: time/score/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT           (0x50) : unicast hit to a target player.
//   MSG_TOTEM_BEACON  (0xF0) : broadcast by all totems periodically (received only).
//   MSG_CP_BEACON     (0x54) : broadcast by CP totems every 2 s.
//                              payload[0] = cpTeam: 0=O, 1=X, 0xFF=teamless.
//                              Players reply 0x55 to notify presence:
//                              subType = 1 (team-O) or 2 (team-X).
//                              subType = 0 (empty auto-reply) is ignored by the CP.
//   MSG_CP_SCORE      (0x56) : broadcast by CP totem when it awards 1 point.
//                              payload[0] = team (0=O, 1=X) that receives the point.
//   MSG_SCORE_COLLECT (0x58) : broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x51 reply to MSG_LIT)
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
//   numControlPoints : number of active CP totems (default 3, max 6).
//   numBases         : total BASE totems across both teams (default 2, step 2).
//   friendlyFire     : 0 = off (default), 1 = on.
//   endPoints        : combined CP-point limit for early GAME_END; 0 = disabled (default 150).
// ================================================================

extern Enlight* enlightPtr;

namespace Upkeep {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Message types ----
enum Msg : uint8_t {
    MSG_LIT           = 0x50,
    MSG_CP_BEACON     = 0x54,
    // 0x55 = presence reply to MSG_CP_BEACON; subType 1=team-O, 2=team-X
    MSG_CP_SCORE      = 0x56,
    MSG_SCORE_COLLECT = 0x58,
};

// ---- Reply sub-types for MSG_LIT ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN  = 1,
    REPLY_SHONE  = 2,
    REPLY_DOWN   = 3,
    REPLY_FRIEND = 4,
};

// ---- CP team encoding (used in MSG_CP_BEACON payload[0] and cpState[]) ----
static constexpr uint8_t CP_TEAM_NONE = 0xFF;  // teamless

// ---- RSSI proximity thresholds ----
static constexpr int8_t NEAR_CP_RSSI   = -65;  // ~3 m indoors for CP presence
static constexpr int8_t NEAR_BASE_RSSI = -60;  // ~2 m indoors for BASE respawn

// ---- Config variables ----
static int startLives       = 3;
static int respawnSecs      = 30;
static int startEnergy      = 50;
static int rechargeSecs     = 10;
static int gameTime         = 900;
static int numControlPoints = 3;
static int numBases         = 2;
static int friendlyFire     = 0;
static int endPoints        = 150;

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

// Cached CP ownership states; updated from MSG_CP_BEACON.
// CP_TEAM_NONE = not yet heard from this CP.
static uint8_t cpState[6];

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;
static bool     canRespawn;
static uint8_t  myTeam;         // 0=O, 1=X; loaded in onBegin
static int      teamBitmask;    // bit i=1 → player i on team X

// Moved from doInGame static locals so they reset correctly on game restart.
static bool     triggerWasActive = false;
static uint32_t releaseAt        = 0;

// ---- Totem device-ID slots ----
static int cpIds[6]     = {0, 0, 0, 0, 0, 0};
static int baseO_ids[3] = {0, 0, 0};
static int baseX_ids[3] = {0, 0, 0};

// ---- Config vars ----
static const ConfigVar configVars[] = {
    //name              value               min   max   step
    { "Lives",         &startLives,         1,    5,    1  },
    { "Respawn",       &respawnSecs,        5,   120,   5  },
    { "Energy",        &startEnergy,       10,   100,  10  },
    { "Recharge",      &rechargeSecs,       0,    20,   5  },
    { "Time",          &gameTime,          60,   900,  60  },
    { "CtrlPts",       &numControlPoints,   3,     6,   1  },
    { "Bases",         &numBases,           2,     6,   2  },
    { "FriendlyFire",  &friendlyFire,       0,     1,   1  },
    { "EndPoints",     &endPoints,          0,   500,  50  },
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

// ---- TotemVars — 3 required CPs, 3 optional CPs, 3+3 optional BASEs ----
static const TotemVar totemVars[] = {
    //  name       id ptr          team  required
    { "CP 1",    &cpIds[0],       0,    true  },
    { "CP 2",    &cpIds[1],       0,    true  },
    { "CP 3",    &cpIds[2],       0,    true  },
    { "CP 4",    &cpIds[3],       0,    false },
    { "CP 5",    &cpIds[4],       0,    false },
    { "CP 6",    &cpIds[5],       0,    false },
    { "Base O1", &baseO_ids[0],   0,    true  },
    { "Base O2", &baseO_ids[1],   0,    false },
    { "Base O3", &baseO_ids[2],   0,    false },
    { "Base X1", &baseX_ids[0],   1,    true  },
    { "Base X2", &baseX_ids[1],   1,    false },
    { "Base X3", &baseX_ids[2],   1,    false },
};

// ---- Helpers ----

static bool isMyTeamBase(uint8_t senderId) {
    const int* ids   = (myTeam == 0) ? baseO_ids : baseX_ids;
    int        count = numBases / 2;
    if (count > 3) count = 3;
    for (int i = 0; i < count; i++) {
        if (ids[i] != 0 && (uint8_t)ids[i] == senderId)
            return true;
    }
    return false;
}

static bool isOpponent(uint8_t targetId) {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return false;
    bool onX = (teamBitmask >> targetId) & 1;
    return (myTeam == 0) ? onX : !onX;
}

// Returns the index (0–5) of a known CP with the given sender ID, or -1.
static int8_t cpIndex(uint8_t senderId) {
    int active = (numControlPoints < 6) ? numControlPoints : 6;
    for (int i = 0; i < active; i++) {
        if (cpIds[i] != 0 && (uint8_t)cpIds[i] == senderId)
            return (int8_t)i;
    }
    return -1;
}

// Refresh the score display string from current totals.
static void refreshScoreStr() {
    int myPts    = (myTeam == 0) ? teamOPoints : teamXPoints;
    int enemyPts = (myTeam == 0) ? teamXPoints : teamOPoints;
    snprintf(teamScoreStr, sizeof(teamScoreStr), "%d/%d", myPts, enemyPts);
}

// ---- UIAction for friendly-fire feedback (UIEvent::Custom1) ----
static const LightAir_UICtrl::UIAction kFriendlyFireAction = {
    /* durations    */ { 200, 0, 0, 0 },
    /* stepCount    */ 1,
    /* soundFreqs   */ { 200, 0, 0, 0 },
    /* vibIntensity */ { 60,  0, 0, 0 },
    /* rgbColors    */ { {255, 100, 0}, {0,0,0}, {0,0,0}, {0,0,0} },
    /* lcdText      */ "Teammate!",
    /* lcdTotalMs   */ 800,
    /* priority     */ 3,
};

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl* ui) {
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

    for (uint8_t i = 0; i < 6; i++) cpState[i] = CP_TEAM_NONE;
    snprintf(teamScoreStr, sizeof(teamScoreStr), "0/0");

    PlayerConfig cfg;
    player_config_load(cfg);
    myTeam = (cfg.team == 1) ? 1 : 0;

    if (ui) ui->defineCustomAction(LightAir_UICtrl::UIEvent::Custom1, kFriendlyFireAction);
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

static void onCpScore(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    if (cpIndex(pkt.senderId) < 0) return;   // ignore unknown senders
    uint8_t scoringTeam = pkt.payload[0];
    if      (scoringTeam == 0) teamOPoints++;
    else if (scoringTeam == 1) teamXPoints++;
    else return;
    myTeamCPPoints = (myTeam == 0) ? teamOPoints : teamXPoints;
    refreshScoreStr();
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType          condition            replySubType  onReceive
    { IN_GAME,  MSG_LIT,        litAndTakenAndValid, REPLY_TAKEN,  onLitTaken },
    { IN_GAME,  MSG_LIT,        litAndShoneAndValid, REPLY_SHONE,  onLitShone },
    { IN_GAME,  MSG_LIT,        litButFriendly,      REPLY_FRIEND, nullptr    },
    { OUT_GAME, MSG_LIT,        nullptr,             REPLY_DOWN,   nullptr    },
    { IN_GAME,  MSG_CP_SCORE,   nullptr,             0,            onCpScore  },
    { OUT_GAME, MSG_CP_SCORE,   nullptr,             0,            onCpScore  },
};

// ---- ReplyRadioRules ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyFriend(const RadioPacket&, const RadioPacket&,
                          LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Custom1);
}

static const ReplyRadioRule replyRadioRules[] = {
    //  activeInStateMask               eventType                       subType        condition  onReply
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_TAKEN,  nullptr, onReplyTaken  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_SHONE,  nullptr, onReplyShone  },
    { (1u<<IN_GAME)|(1u<<OUT_GAME), RadioEventType::ReplyReceived, REPLY_FRIEND, nullptr, onReplyFriend },
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

// ---- Scan CP beacons: update ownership cache and (if sendPresence) reply with team ----
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

        // Notify on ownership change (CP_TEAM_NONE = unknown = first beacon ever heard).
        uint8_t newState = ev.packet.payload[0];
        if (newState != cpState[idx]) {
            cpState[idx] = newState;
            char msg[16];
            if (newState == CP_TEAM_NONE) {
                snprintf(msg, sizeof(msg), "CP %d neutral", idx + 1);
                disp.showMessage(msg, 3000);
            } else {
                snprintf(msg, sizeof(msg), "CP %d->Team %c!", idx + 1,
                         newState == 0 ? 'O' : 'X');
                disp.showMessage(msg, 3000);
                // Good news if our team captured it, bad news if the enemy did.
                if (newState == myTeam)
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagReturn);
                else
                    out.ui.trigger(LightAir_UICtrl::UIEvent::FlagTaken);
            }
        }

        // Reply with our team so the CP totem can track who is nearby.
        // subType: 1 = team-O player, 2 = team-X player (0 = empty auto-reply, ignored).
        if (sendPresence && ev.rssi >= NEAR_CP_RSSI)
            out.radio.reply(ev.packet, (uint8_t)(myTeam + 1));
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

    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) {
        if (isOpponent(r.id) || friendlyFire)
            out.radio.sendTo(r.id, MSG_LIT);
    }
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl& disp, GameOutput& out) {
    tickGameTime();
    scanCpBeacons(radio, disp, out, false);

    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon to enable respawn.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_TOTEM_BEACON)                continue;
        if (!isMyTeamBase(ev.packet.senderId))                    continue;
        if (ev.rssi            < NEAR_BASE_RSSI)                  continue;
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
    // Each player reports myTeamCPPoints and shoneTimes.
    // Aggregate: max CP points per team (tolerates missed MSG_CP_SCORE packets),
    //            sum shoneTimes per team.
    int32_t cpPts[2] = {0, 0};
    int32_t shone[2] = {0, 0};

    for (uint8_t r = 0; r < t.rosterCount; r++) {
        if (!(t.accumMask & (1u << r))) continue;
        uint8_t pid  = t.roster[r];
        uint8_t team = (pid < PlayerDefs::MAX_PLAYER_ID) ? t.teamMap[pid] : 0;
        if (team > 1) team = 0;

        int32_t cp = 0, shn = 0;
        memcpy(&cp,  t.slots[r],     4);  // winnerVars[0] = myTeamCPPoints (MAX)
        memcpy(&shn, t.slots[r] + 4, 4);  // winnerVars[1] = shoneTimes     (MIN)

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

// ---- Totem-side runner: handles both BASE and CP (control-point) totem roles ----
//
// Role is determined from the first activation message:
//   0xF1 reply (player near base)        → ROLE_BASE
//   0x55 reply to MSG_CP_BEACON (0x54)   → ROLE_CP
//
// BASE role: shows Respawn animation when a player acknowledges the beacon.
//
// CP role:
//   - Broadcasts MSG_CP_BEACON (0x54) every 2 s with current cpTeam in payload[0].
//   - Collects 0x55 replies (subType 1=O, 2=X) in a 2 s window.
//   - After 2 s: if only one team replied, attach to that team; both or none = hold.
//   - After 10 s unchanged attachment: broadcast MSG_CP_SCORE (payload[0]=team),
//     restart countdown.
//   - Shows ControlO / ControlX / ControlContest / Idle background accordingly.
//
class UpkeepCompositeRunner : public LightAir_TotemRunner {
    enum Role : uint8_t { ROLE_UNKNOWN, ROLE_BASE, ROLE_CP };

    static constexpr uint32_t CP_BEACON_INTERVAL_MS = 2000;
    static constexpr uint32_t CP_SCORE_INTERVAL_MS  = 10000;

    Role    _role;
    uint8_t _cpTeam;       // CP_TEAM_NONE (0xFF), 0=O, 1=X
    bool    _presenceO;    // team-O player replied in current window
    bool    _presenceX;    // team-X player replied in current window
    uint32_t _windowStart; // millis() when current 2 s window opened
    uint32_t _attachStart; // millis() when current attachment started
    uint32_t _lastBeacon;  // millis() of last 0x54 beacon sent

public:
    UpkeepCompositeRunner()
        : _role(ROLE_UNKNOWN), _cpTeam(CP_TEAM_NONE),
          _presenceO(false), _presenceX(false),
          _windowStart(0), _attachStart(0), _lastBeacon(0) {}

    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override {
        // Lazy role detection on first message.
        if (_role == ROLE_UNKNOWN) {
            if (msg.msgType == MSG_TOTEM_BEACON + 1)   _role = ROLE_BASE;
            else if (msg.msgType == MSG_CP_BEACON + 1) _role = ROLE_CP;
        }

        if (_role == ROLE_BASE) {
            if (msg.msgType != MSG_TOTEM_BEACON + 1) return;
            uint8_t r = (msg.team == 0) ? 255 :   0;
            uint8_t g = (msg.team == 0) ?  80 :  80;
            uint8_t b = (msg.team == 0) ?   0 : 255;
            out.ui.trigger(TotemUIEvent::Respawn, r, g, b);
            return;
        }

        if (_role == ROLE_CP && msg.msgType == MSG_CP_BEACON + 1) {
            // 0x55 reply: subType 1=O, 2=X; subType 0 = auto-empty, ignore.
            if (msg.payloadLen == 0) return;
            uint8_t sub = msg.payload[0];
            if (sub == 1) _presenceO = true;
            if (sub == 2) _presenceX = true;
        }
    }

    void update(LightAir_TotemOutput& out) override {
        if (_role != ROLE_CP) return;

        uint32_t now = millis();

        // ---- Broadcast CP beacon every 2 s ----
        bool windowExpired = (now - _windowStart) >= CP_BEACON_INTERVAL_MS;
        if (windowExpired) {
            // Evaluate the window that just closed.
            if (_presenceO || _presenceX) {
                uint8_t newTeam = _cpTeam;
                if (_presenceO && !_presenceX) newTeam = 0;
                if (_presenceX && !_presenceO) newTeam = 1;
                // Both teams present: contested — hold current attachment.

                if (newTeam != _cpTeam) {
                    // Team switch: reset scoring countdown.
                    _cpTeam    = newTeam;
                    _attachStart = now;
                    updateCPBackground(out);
                } else if (_cpTeam != CP_TEAM_NONE) {
                    // Same team: check if 10 s elapsed for a point.
                    if ((now - _attachStart) >= CP_SCORE_INTERVAL_MS) {
                        uint8_t payload[1] = { _cpTeam };
                        out.radio.broadcast(MSG_CP_SCORE, payload, 1);
                        out.ui.trigger(TotemUIEvent::Bonus);
                        _attachStart = now;  // restart countdown
                    }
                }

                if (_presenceO && _presenceX) {
                    // Contested — show alternate colours.
                    out.ui.trigger(TotemUIEvent::ControlContest);
                }
            } else if (_cpTeam == CP_TEAM_NONE) {
                // No presence and no attachment.
                out.ui.trigger(TotemUIEvent::Idle);
            }

            // Reset window and broadcast new beacon.
            _presenceO   = false;
            _presenceX   = false;
            _windowStart = now;
            uint8_t pl[1] = { _cpTeam };
            out.radio.broadcast(MSG_CP_BEACON, pl, 1);
        }
    }

    void reset() override {
        _role        = ROLE_UNKNOWN;
        _cpTeam      = CP_TEAM_NONE;
        _presenceO   = false;
        _presenceX   = false;
        _windowStart = 0;
        _attachStart = 0;
        _lastBeacon  = 0;
    }

private:
    void updateCPBackground(LightAir_TotemOutput& out) const {
        if      (_cpTeam == 0)         out.ui.trigger(TotemUIEvent::ControlO);
        else if (_cpTeam == 1)         out.ui.trigger(TotemUIEvent::ControlX);
        else                           out.ui.trigger(TotemUIEvent::Idle);
    }
};
static UpkeepCompositeRunner upkeepCompositeRunner;

} // namespace Upkeep

// ================================================================
// Public game descriptor — registered in AllGames.cpp
// ================================================================
const LightAir_Game game_upkeep = {
    /* typeId                */ 0x00000005,
    /* name                  */ "Upkeep",
    /* configVars            */ Upkeep::configVars,
    /* configCount           */ sizeof(Upkeep::configVars) / sizeof(*Upkeep::configVars),
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
    /* totemVars             */ Upkeep::totemVars,
    /* totemVarCount         */ sizeof(Upkeep::totemVars) / sizeof(*Upkeep::totemVars),
    /* hasTeams              */ true,
    /* teamBitmask           */ &Upkeep::teamBitmask,
    /* totemRunner           */ &Upkeep::upkeepCompositeRunner,
};
