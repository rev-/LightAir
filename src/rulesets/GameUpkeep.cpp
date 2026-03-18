#include <LightAir.h>
#include <string.h>

// ================================================================
// Upkeep — two-team game with Control Points (CPs).
//
// States
//   IN_GAME  (0) : player is active; can shine; display: lives/energy/time/cpPoints.
//   OUT_GAME (1) : player is down; waits for BASE-beacon respawn; display: time.
//   GAME_END (2) : game over; display: time/cpPoints/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT            (0x50) : unicast hit to a target player.
//   MSG_BASE_BEACON    (0x52) : broadcast by BASE totems periodically (received only).
//   MSG_CP_BEACON      (0x54) : broadcast by CP totems every 2 s.
//                               payload[0] = cpTeam: 0=teamless, 1=O, 2=X.
//   MSG_CP_PRESENCE    (0x56) : unicast player → CP when within range.
//                               payload[0] = myTeam (0=O, 1=X).
//                               The CP firmware tracks presences over its 2 s window
//                               to decide whether to switch attachment team.
//   MSG_CP_SCORE       (0x58) : broadcast by CP totem when it awards 1 point.
//                               payload[0] = team (0=O, 1=X) that receives the point.
//   MSG_SCORE_COLLECT  (0x5A) : broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x51 reply to MSG_LIT)
//   REPLY_TAKEN  (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE  (2) : target eliminated; lives reached 0.
//   REPLY_DOWN   (3) : target was already OUT_GAME; hit ignored.
//   REPLY_FRIEND (4) : hit rejected — friendly fire is disabled.
//
// CP attachment and point-scoring (runs on CP totem firmware):
//   - CP starts teamless; gives no points until first attachment.
//   - Every 2 s, the CP collects MSG_CP_PRESENCE messages and decides:
//       · only team-O players near  → attach (or stay) to O
//       · only team-X players near  → attach (or stay) to X
//       · both teams or nobody near → maintain current attachment
//   - On team switch: no point awarded; 10 s countdown resets.
//   - After 10 s of unchanged attachment: broadcast MSG_CP_SCORE for the
//     attached team, then restart the 10 s countdown.
//
// Respawn flow (identical to Teams):
//   On shone: respawnAt = millis() + respawnSecs*1000.
//   doOutGame waits until then and scans for this team's BASE beacon
//   with RSSI ≥ NEAR_BASE_RSSI; when found, sets canRespawn = true.
//
// Scoring and victory:
//   Points come from CP_SCORE broadcasts.  The team with the higher
//   total at game-end wins; tie-break: team with fewer total shoneTimes.
//   endPoints: if totalCPPoints (O + X combined) reaches this threshold
//   the game ends early, regardless of remaining time.
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
//   endPoints        : combined CP-point limit for early GAME_END (default 150).
// ================================================================

extern Enlight* enlightPtr;

namespace Upkeep {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Message types ----
enum Msg : uint8_t {
    MSG_LIT            = 0x50,
    MSG_BASE_BEACON    = 0x52,
    MSG_CP_BEACON      = 0x54,
    MSG_CP_PRESENCE    = 0x56,
    MSG_CP_SCORE       = 0x58,
    MSG_SCORE_COLLECT  = 0x5A,
};

// ---- Reply sub-types ----
enum ReplySubType : uint8_t {
    REPLY_TAKEN  = 1,
    REPLY_SHONE  = 2,
    REPLY_DOWN   = 3,
    REPLY_FRIEND = 4,
};

// ---- RSSI proximity thresholds ----
// -65 dBm ≈ 3 m indoors for CP detection; -60 dBm ≈ 2 m for BASE respawn.
static constexpr int8_t NEAR_CP_RSSI   = -65;
static constexpr int8_t NEAR_BASE_RSSI = -60;

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
static int teamOPoints    = 0;   // CP points scored by team O (both teams track this)
static int teamXPoints    = 0;   // CP points scored by team X
static int myTeamCPPoints = 0;   // = teamOPoints or teamXPoints depending on myTeam

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;
static bool     canRespawn;
static uint8_t  myTeam;         // 0=O, 1=X; loaded in onBegin
static int      teamBitmask;    // bit i=1 → player i on team X

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
    { "EndPoints",     &endPoints,        100,   500,  50  },
};

// ---- Monitor vars ----
static const MonitorVar monitorVars[] = {
    // IN_GAME display
    MonitorVar::Int("Lives",  &lives,          1u<<IN_GAME,                  ICON_LIFE,   0, 0),
    MonitorVar::Int("Energy", &energy,         1u<<IN_GAME,                  ICON_ENERGY, 1, 0),
    MonitorVar::Int("Time",   &gameTimeLeft,   (1u<<IN_GAME)|(1u<<OUT_GAME), ICON_TIME,   0, 1),
    MonitorVar::Int("Points", &myTeamCPPoints, 1u<<IN_GAME,                  ICON_SCORE,  1, 1),
    // GAME_END display
    MonitorVar::Int("Time",   &gameTime,       1u<<GAME_END, ICON_TIME,   0, 0),
    MonitorVar::Int("Points", &myTeamCPPoints, 1u<<GAME_END, ICON_SCORE,  1, 0),
    MonitorVar::Int("Energy", &energySpent,    1u<<GAME_END, ICON_ENERGY, 0, 1),
    MonitorVar::Int("Shone",  &shoneTimes,     1u<<GAME_END, ICON_LIFE,   1, 1),
};

// ---- TotemVars — 3 required CPs, 3 optional CPs, 3+3 optional BASEs ----
// CP totems are team-neutral (team field = 0 is used as a placeholder; the setup
// menu assigns them independently of the player team assignment step).
static const TotemVar totemVars[] = {
    //  name       id ptr          team  required
    { "CP 1",    &cpIds[0],       0,    true  },
    { "CP 2",    &cpIds[1],       0,    true  },
    { "CP 3",    &cpIds[2],       0,    true  },
    { "CP 4",    &cpIds[3],       0,    false },
    { "CP 5",    &cpIds[4],       0,    false },
    { "CP 6",    &cpIds[5],       0,    false },
    { "Base O1", &baseO_ids[0],   0,    false },
    { "Base O2", &baseO_ids[1],   0,    false },
    { "Base O3", &baseO_ids[2],   0,    false },
    { "Base X1", &baseX_ids[0],   1,    false },
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

static bool isCp(uint8_t senderId) {
    int active = numControlPoints;
    if (active > 6) active = 6;
    for (int i = 0; i < active; i++) {
        if (cpIds[i] != 0 && (uint8_t)cpIds[i] == senderId)
            return true;
    }
    return false;
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
    lives          = startLives;
    energy         = startEnergy;
    gameTimeLeft   = gameTime;
    shoneTimes     = 0;
    energySpent    = 0;
    teamOPoints    = 0;
    teamXPoints    = 0;
    myTeamCPPoints = 0;
    canRespawn     = false;
    respawnAt      = 0;
    lastTickAt     = millis();

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

// Called when a CP broadcasts a point award; update both team totals and the
// derived myTeamCPPoints used by WinnerVar and the display.
static void onCpScore(const RadioPacket& pkt, LightAir_DisplayCtrl&, GameOutput&) {
    uint8_t scoringTeam = pkt.payload[0];
    if      (scoringTeam == 0) teamOPoints++;
    else if (scoringTeam == 1) teamXPoints++;
    myTeamCPPoints = (myTeam == 0) ? teamOPoints : teamXPoints;
}

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType           condition            replySubType  onReceive
    { IN_GAME,  MSG_LIT,         litAndTakenAndValid, REPLY_TAKEN,  onLitTaken },
    { IN_GAME,  MSG_LIT,         litAndShoneAndValid, REPLY_SHONE,  onLitShone },
    { IN_GAME,  MSG_LIT,         litButFriendly,      REPLY_FRIEND, nullptr    },
    { OUT_GAME, MSG_LIT,         nullptr,             REPLY_DOWN,   nullptr    },
    { IN_GAME,  MSG_CP_SCORE,    nullptr,             0,            onCpScore  },
    { OUT_GAME, MSG_CP_SCORE,    nullptr,             0,            onCpScore  },
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
// Primary: team with the higher aggregate CP points wins.
// Tie-break: team with fewer aggregate shoneTimes wins.
static const WinnerVar winnerVars[] = {
    { &myTeamCPPoints, WinnerDir::MAX },
    { &shoneTimes,     WinnerDir::MIN },
};

// ---- Per-second ticker ----
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
static bool endPointsReached(const InputReport&, const RadioReport&) {
    return (teamOPoints + teamXPoints) >= endPoints;
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
                     LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

    // ---- Combat ----
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

    // ---- Control Point presence notification ----
    // For each CP beacon received above the proximity threshold, send a
    // presence message so the CP totem can track which team is nearby and
    // decide whether to switch attachment or award a point.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_CP_BEACON)                   continue;
        if (!isCp(ev.packet.senderId))                            continue;
        if (ev.rssi            < NEAR_CP_RSSI)                    continue;
        uint8_t pld[1] = { myTeam };
        out.radio.sendTo(ev.packet.senderId, MSG_CP_PRESENCE, pld, 1);
    }
}

static void doOutGame(const InputReport&, const RadioReport& radio,
                      LightAir_DisplayCtrl&, GameOutput&) {
    tickGameTime();

    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon: must be from this team's base
    // and have RSSI above the proximity threshold.
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type           != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)                 continue;
        if (!isMyTeamBase(ev.packet.senderId))                    continue;
        if (ev.rssi            < NEAR_BASE_RSSI)                  continue;
        canRespawn = true;
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
    // Each player broadcasts their own team's CP points (myTeamCPPoints) and
    // their individual shoneTimes.  Aggregate per team:
    //   cpPts[team]  = max across team members (handles missed MSG_CP_SCORE)
    //   shone[team]  = sum across team members
    int32_t cpPts[2]  = {0, 0};
    int32_t shone[2]  = {0, 0};

    for (uint8_t r = 0; r < t.rosterCount; r++) {
        if (!(t.accumMask & (1u << r))) continue;
        uint8_t pid  = t.roster[r];
        uint8_t team = (pid < PlayerDefs::MAX_PLAYER_ID) ? t.teamMap[pid] : 0;
        if (team > 1) team = 0;

        int32_t cp = 0, shn = 0;
        memcpy(&cp,  t.slots[r],     4);  // winnerVars[0] = myTeamCPPoints (MAX)
        memcpy(&shn, t.slots[r] + 4, 4);  // winnerVars[1] = shoneTimes (MIN)

        if (cp > cpPts[team]) cpPts[team] = cp;  // take max to handle missed broadcasts
        shone[team] += shn;
    }

    uint8_t winner = 0xFF;  // 0xFF = genuine tie
    if      (cpPts[1]  > cpPts[0])  winner = 1;
    else if (cpPts[0]  > cpPts[1])  winner = 0;
    else if (shone[1]  < shone[0])  winner = 1;
    else if (shone[0]  < shone[1])  winner = 0;

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
const LightAir_Game game_upkeep = {
    /* typeId                */ 0x00000005,
    /* name                  */ "Upkeep",
    /* configVars            */ Upkeep::configVars,         /* configCount            */ 9,
    /* monitorVars           */ Upkeep::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ Upkeep::directRadioRules,   /* directRadioRuleCount   */ 6,
    /* replyRadioRules       */ Upkeep::replyRadioRules,    /* replyRadioRuleCount    */ 3,
    /* rules                 */ Upkeep::rules,              /* ruleCount              */ 6,
    /* behaviors             */ Upkeep::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &Upkeep::gState,            /* initialState           */ Upkeep::IN_GAME,
    /* onBegin               */ Upkeep::onBegin,
    /* winnerVars            */ Upkeep::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ Upkeep::GAME_END,
    /* scoreMsgType          */ Upkeep::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ Upkeep::onScoreAnnounce,
    /* totemVars             */ Upkeep::totemVars,          /* totemVarCount          */ 12,
    /* hasTeams              */ true,
    /* teamBitmask           */ &Upkeep::teamBitmask,
};
