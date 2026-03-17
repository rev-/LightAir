#include <LightAir.h>
#include <string.h>

// ================================================================
// Teams — two-team game: O vs X.
//
// States
//   IN_GAME  (0) : player is active; can shine; display: lives/energy/time/points.
//   OUT_GAME (1) : player is down; display: time.
//   GAME_END (2) : game over; display: time/points/energySpent/shoneTimes.
//
// Radio messages (even = request, odd = reply)
//   MSG_LIT          (0x30) : unicast hit to a target player.
//   MSG_BASE_BEACON  (0x32) : broadcast by BASE totems periodically (received only).
//   MSG_SCORE_COLLECT (0x34): broadcast per-player scores during GAME_END.
//
// Reply sub-types (payload[0] of the 0x31 reply)
//   REPLY_TAKEN  (1) : target absorbed the hit; lives > 0 after decrement.
//   REPLY_SHONE  (2) : target eliminated; lives reached 0.
//   REPLY_DOWN   (3) : target was already OUT_GAME; hit ignored.
//   REPLY_FRIEND (4) : hit rejected — sender and receiver are on the same team
//                      and friendly fire is disabled.
//
// Respawn flow
//   On being shone: respawnAt = millis() + respawnSecs*1000; canRespawn = false.
//   doOutGame polls incoming MSG_BASE_BEACON events.  If the beacon comes from
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
//   numBases     : total BASE totems (must be even; step 2; default 2).
//   friendlyFire : 0 = off (default), 1 = on.
// ================================================================

extern Enlight* enlightPtr;

namespace Teams {

// ---- States ----
enum State : uint8_t { IN_GAME, OUT_GAME, GAME_END };

// ---- Message types ----
enum Msg : uint8_t {
    MSG_LIT           = 0x30,
    MSG_BASE_BEACON   = 0x32,
    MSG_SCORE_COLLECT = 0x34,
};

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
static int numBases     = 2;
static int friendlyFire = 0;

// ---- Runtime variables ----
static int lives        = 3;
static int energy       = 50;
static int gameTimeLeft = 900;
static int points       = 0;
static int energySpent  = 0;
static int shoneTimes   = 0;

static uint8_t  gState;
static uint32_t lastTickAt;
static uint32_t respawnAt;    // millis() before which BASE beacons are ignored
static bool     canRespawn;   // set true by doOutGame when timer + RSSI condition met
static uint8_t  myTeam;       // 0=O, 1=X; loaded from PlayerConfig in onBegin
static int      teamBitmask;  // bit i=1 → player i on team X; filled from config blob

// ---- Totem device-ID slots (pointed to by TotemVars) ----
// The setup menu writes the assigned device ID into these via TotemVar::id pointers.
static int baseO_ids[4] = {0, 0, 0, 0};   // team-O base device IDs
static int baseX_ids[4] = {0, 0, 0, 0};   // team-X base device IDs

// ---- Config vars (startup menu) ----
static const ConfigVar configVars[] = {
    //name           value           min   max   step
    { "Lives",      &startLives,    1,    5,    1  },
    { "Respawn",    &respawnSecs,   5,    120,  5  },
    { "Energy",     &startEnergy,   10,   100,  10 },
    { "Recharge",   &rechargeSecs,  0,    20,   5  },
    { "Time",       &gameTime,      60,   900,  60 },
    { "Bases",      &numBases,      2,    8,    2  },
    { "FriendlyFire", &friendlyFire, 0,   1,    1  },
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

// ---- TotemVars — 4 team-O slots + 4 team-X slots, all optional ----
static const TotemVar totemVars[] = {
    //  name        id ptr         team  required
    { "Base O1", &baseO_ids[0],  0,    false },
    { "Base O2", &baseO_ids[1],  0,    false },
    { "Base O3", &baseO_ids[2],  0,    false },
    { "Base O4", &baseO_ids[3],  0,    false },
    { "Base X1", &baseX_ids[0],  1,    false },
    { "Base X2", &baseX_ids[1],  1,    false },
    { "Base X3", &baseX_ids[2],  1,    false },
    { "Base X4", &baseX_ids[3],  1,    false },
};

// ---- Helper: is senderId one of this player's active team base totems? ----
static bool isMyTeamBase(uint8_t senderId) {
    const int* ids     = (myTeam == 0) ? baseO_ids : baseX_ids;
    int        active  = numBases / 2;   // half the bases belong to each team
    if (active > 4) active = 4;
    for (int i = 0; i < active; i++) {
        if (ids[i] != 0 && (uint8_t)ids[i] == senderId)
            return true;
    }
    return false;
}

// ---- Helper: is targetId on the opposing team? ----
static bool isOpponent(uint8_t targetId) {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return false;
    bool targetOnX = (teamBitmask >> targetId) & 1;
    // myTeam==0 (O) → opponent is on X (targetOnX true)
    // myTeam==1 (X) → opponent is on O (targetOnX false)
    return (myTeam == 0) ? targetOnX : !targetOnX;
}

// ---- UIAction for friendly-fire feedback (UIEvent::Custom1) ----
static const LightAir_UICtrl::UIAction kFriendlyFireAction = {
    /* durations     */ { 200, 0, 0, 0 },
    /* stepCount     */ 1,
    /* soundFreqs    */ { 200, 0, 0, 0 },
    /* vibIntensity  */ { 60,  0, 0, 0 },
    /* rgbColors     */ { {255, 100, 0}, {0,0,0}, {0,0,0}, {0,0,0} },
    /* lcdText       */ "Teammate!",
    /* lcdTotalMs    */ 800,
    /* priority      */ 3,
};

// ---- onBegin ----
static void onBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl* ui) {
    lives        = startLives;
    energy       = startEnergy;
    gameTimeLeft = gameTime;
    points       = 0;
    energySpent  = 0;
    shoneTimes   = 0;
    canRespawn   = false;
    respawnAt    = 0;
    lastTickAt   = millis();

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

static const DirectRadioRule directRadioRules[] = {
    //  state     msgType   condition           replySubType  onReceive
    { IN_GAME,  MSG_LIT,  litAndTakenAndValid, REPLY_TAKEN,  onLitTaken },
    { IN_GAME,  MSG_LIT,  litAndShoneAndValid, REPLY_SHONE,  onLitShone },
    { IN_GAME,  MSG_LIT,  litButFriendly,      REPLY_FRIEND, nullptr    },
    { OUT_GAME, MSG_LIT,  nullptr,             REPLY_DOWN,   nullptr    },
};

// ---- ReplyRadioRule handlers ----
static void onReplyTaken(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
}
static void onReplyShone(const RadioPacket&, const RadioPacket&,
                         LightAir_DisplayCtrl&, GameOutput& out) {
    points++;
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
    { IN_GAME,  gameTimeExpired, GAME_END, onGameEnd  },
    { IN_GAME,  shone,           OUT_GAME, onShone    },
    { OUT_GAME, gameTimeExpired, GAME_END, onGameEnd  },
    { OUT_GAME, canRespawnReady, IN_GAME,  onRespawn  },
};

// ---- Per-state behaviors ----
static void doInGame(const InputReport& inp, const RadioReport&,
                     LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();

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
                      LightAir_DisplayCtrl&, GameOutput&) {
    tickGameTime();

    // Minimum respawn timer: ignore beacons until the wait has elapsed.
    if (millis() < respawnAt) return;

    // Scan for a qualifying BASE beacon: must come from this player's team
    // base and have RSSI above the proximity threshold (~2 m indoors).
    for (uint8_t e = 0; e < radio.count; e++) {
        const RadioEvent& ev = radio.events[e];
        if (ev.type       != RadioEventType::MessageReceived) continue;
        if (ev.packet.msgType != MSG_BASE_BEACON)             continue;
        if (!isMyTeamBase(ev.packet.senderId))                continue;
        if (ev.rssi < NEAR_RSSI_THRESHOLD)                    continue;
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
const LightAir_Game game_teams = {
    /* typeId                */ 0x00000002,
    /* name                  */ "Teams",
    /* configVars            */ Teams::configVars,         /* configCount            */ 7,
    /* monitorVars           */ Teams::monitorVars,        /* monitorCount           */ 8,
    /* directRadioRules      */ Teams::directRadioRules,   /* directRadioRuleCount   */ 4,
    /* replyRadioRules       */ Teams::replyRadioRules,    /* replyRadioRuleCount    */ 3,
    /* rules                 */ Teams::rules,              /* ruleCount              */ 4,
    /* behaviors             */ Teams::behaviors,          /* behaviorCount          */ 3,
    /* currentState          */ &Teams::gState,            /* initialState           */ Teams::IN_GAME,
    /* onBegin               */ Teams::onBegin,
    /* winnerVars            */ Teams::winnerVars,         /* winnerVarCount         */ 2,
    /* scoringState          */ Teams::GAME_END,
    /* scoreMsgType          */ Teams::MSG_SCORE_COLLECT,
    /* onScoreAnnounce       */ Teams::onScoreAnnounce,
    /* totemVars             */ Teams::totemVars,          /* totemVarCount          */ 8,
    /* hasTeams              */ true,
    /* teamBitmask           */ &Teams::teamBitmask,
};
