// Host end-to-end test: load every real game file through the real
// LightAir_LuaGame binding, then drive freeforall through begin /
// messages / replies / rules / update ticks using the synthesized
// LightAir_Game descriptor exactly the way GameRunner would.
#include <cstdio>
#include <cstring>
#include <string>

#include "Arduino.h"
#include "ArduinoLog.h"
uint32_t g_millis = 1000;
HostLog Log;

#include "lua/LightAir_LuaGame.h"
#include "game/LightAir_GameRunner.h"
#include "radio/LightAir_RadioTestTransport.h"
#include "enlight/Enlight.h"

// ---- Enlight stub (link-time): the verbs guard on enlightPtr, but the
// test also exercises la.shine paths through a scripted instance.
static EnlightStatus g_shineStatus = EnlightStatus::NO_HIT;
static uint8_t       g_shineId     = 0;
Enlight::Enlight(const EnlightCalib&) {}
Enlight::~Enlight() {}
bool Enlight::run() { return true; }
EnlightResult Enlight::poll() { return EnlightResult(g_shineStatus, g_shineId); }
static EnlightCalib g_calib;
static Enlight g_enlight(g_calib);
Enlight* enlightPtr = &g_enlight;

// ---- fake hardware behind the abstract interfaces ----
// Records the tray band (the top of the screen) so a test can assert
// that a ruleset's la.show() reached the glass, and that it left when it
// expired: a fill over a tray row erases it, a print puts text back.
struct FakeDisplay : LightAir_Display {
    char tray[DisplayDefaults::TRAY_MAX_MESSAGES][32] = {};

    static bool trayRow(uint8_t y, uint8_t& row) {
        if (y >= DisplayDefaults::TRAY_HEIGHT) return false;
        row = (uint8_t)(y / DisplayDefaults::FONT_HEIGHT);
        return row < DisplayDefaults::TRAY_MAX_MESSAGES;
    }
    void clear() override { memset(tray, 0, sizeof(tray)); }
    void setColor(bool) override {}
    void fillRect(uint8_t, uint8_t y, uint8_t, uint8_t) override {
        uint8_t row;
        if (trayRow(y, row)) tray[row][0] = '\0';
    }
    void drawRect(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void drawBitmap(uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) override {}
    void print(uint8_t, uint8_t y, const char* t) override {
        uint8_t row;
        if (!trayRow(y, row)) return;
        strncpy(tray[row], t, sizeof(tray[row]) - 1);
        tray[row][sizeof(tray[row]) - 1] = '\0';
    }
    uint16_t textWidth(const char* t) override { return (uint16_t)(6 * strlen(t)); }
    void flush() override {}
};
struct FakeAudio : LightAir_Audio {
    void play(int) override {}
    void stop() override {}
};
struct FakeVib : LightAir_Vibration {
    void vibrate(int) override {}
    void stop() override {}
};
struct FakeRGB : LightAir_RGB {
    void setColor(uint8_t, uint8_t, uint8_t) override {}
    void off() override {}
};

uint8_t TotemRoleId_BONUS();
uint8_t TotemRoleId_CP();
static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

// Find a monitor var's backing int by (partial) name.
static int* slotOf(const LightAir_Game& g, const char* name) {
    for (uint8_t i = 0; i < g.monitorCount; i++)
        if (g.monitorVars[i].type == VarType::INT &&
            strcmp(g.monitorVars[i].name, name) == 0)
            return g.monitorVars[i].asInt;
    return nullptr;
}

// Stand-in receive-side signal strength for the packets the scripted
// session feeds in; well inside every proximity gate in the games.
static constexpr int8_t kNearRssi = -40;

int main() {
    // ---- 1. Every game file loads and validates — sequentially on ONE
    // shared instance, exactly like the device's lazy realize path
    // (GameStore keeps a single loaded game and reloads on selection).
    static const struct { const char* file; uint16_t typeId; } kGames[] = {
        { "games/freeforall.lua", 0x0001 },
        { "games/teams.lua",      0x0002 },
        { "games/flag.lua",       0x0003 },
        { "games/outflow.lua",    0x0004 },
        { "games/upkeep.lua",     0x0005 },
        { "games/kingofhill.lua", 0x0006 },
        { "games/virus.lua",      0x0007 },
        { "games/festasportsasso.lua", 0x0008 },
    };
    static LightAir_LuaGame shared;   // the one loaded-game instance
    static LightAir_LuaGame scanner;  // manifest-scan scratch instance
    for (auto& kg : kGames) {
        // Boot-scan path: peekManifest must read name/typeId without
        // loading (and without consuming a trampoline slot).
        char name[16] = {0};
        uint16_t tid = 0;
        bool mok = scanner.peekManifest(kg.file, name, sizeof(name), &tid);
        CHECK(mok, "manifest peek");
        CHECK(mok && tid == kg.typeId, "manifest typeId matches registry");
        CHECK(mok && name[0] != 0, "manifest has a name");
        CHECK(!scanner.loaded(), "peek leaves the scanner unloaded");

        // Selection path: full (re)load on the shared instance.
        bool ok = shared.load(kg.file);
        printf("load %-24s %s (%s)\n", kg.file, ok ? "OK" : "FAILED", name);
        CHECK(ok, kg.file);
        if (!ok) continue;
        CHECK(shared.typeId() == kg.typeId, "typeId matches registry");
    }
    CHECK(LightAir_LuaGame::instanceCount() == 1,
          "8 sequential loads used one registry slot");

    // ---- 1b. Manifest rejection paths.  Each must report and return
    // false; none may read past an empty Lua stack.  The device hits the
    // first one for real whenever a game file cannot be opened, and a
    // scan that rejects everything leaves the menu with zero games.
    {
        char name[16] = {0};
        uint16_t tid = 0;
        CHECK(!scanner.peekManifest("games/no_such_game.lua", name, sizeof(name), &tid),
              "missing file rejected");
        CHECK(!scanner.peekManifest("test/host/fixtures/notagame.lua",
                                    name, sizeof(name), &tid),
              "chunk that returns a non-table rejected");
        CHECK(!scanner.peekManifest("test/host/fixtures/badmanifest.lua",
                                    name, sizeof(name), &tid),
              "wrong api version rejected");
        CHECK(!scanner.loaded(), "rejected peeks leave the scanner unloaded");
    }

    // ---- 1c. A manifest peek must not need the libraries -------------
    // Every ruleset pulls its libraries in at file scope, and a peek runs
    // file scope.  If it loaded them for real it would compile tens of
    // kilobytes of Lua per file into a state thrown away immediately —
    // which is what ran a device out of memory mid-scan and left one game
    // in the menu.  The fixture asks for a library that does not exist and
    // then indexes, calls and chains it the way a ruleset does.
    {
        char name[16] = {0};
        uint16_t tid = 0;
        CHECK(scanner.peekManifest("test/host/fixtures/libatscope.lua",
                                   name, sizeof(name), &tid),
              "a peek reads the manifest without loading any library");
        CHECK(tid == 0x7F06, "peek got the typeId past the library use");
        CHECK(strcmp(name, "LibScope") == 0, "peek got the name past the library use");

        // Loading it for real must NOT paper over the missing library: the
        // stand-in exists for the peek and nowhere else.  Reuses the shared
        // instance (the pool is deliberately small, and the deep test below
        // reloads it straight afterwards).
        CHECK(!shared.load("test/host/fixtures/libatscope.lua"),
              "a real load still fails on a missing library");
    }

    // Deep-test freeforall: realize it again on the same instance.
    if (!shared.load("games/freeforall.lua")) {
        printf("no FFA, aborting\n");
        return 1;
    }
    LightAir_LuaGame* ffa = &shared;

    const LightAir_Game& game = ffa->descriptor();
    CHECK(game.configCount == 5, "ffa config count");
    CHECK(game.monitorCount == 8, "ffa monitor count");
    CHECK(game.ruleCount == 4, "ffa rule count");
    CHECK(game.behaviorCount == 3, "ffa behaviour rows (states 0..2)");
    CHECK(game.directRadioRuleCount == 4,
          "ffa direct rules (LIT in 2 states + the 2 pickup beacons)");
    CHECK(game.replyRadioRuleCount == 2, "ffa reply rules (any + timeout)");
    CHECK(game.winnerVarCount == 2, "ffa winner vars");
    CHECK(game.totemRequirementCount == 2, "ffa totem slots");
    CHECK(game.totemProgram != nullptr, "ffa has totem programs");
    CHECK(game.gameTimeLeft != nullptr, "ffa time_left wired");
    CHECK(game.scoringState == 2, "ffa scoring state");

    // ---- 2. Config slots editable through the descriptor (menu path) ----
    int* lives = slotOf(game, "lives");
    CHECK(lives, "lives slot visible");
    for (uint8_t i = 0; i < game.configCount; i++)
        if (strcmp(game.configVars[i].name, "Lives") == 0)
            *game.configVars[i].value = 5;                 // DM sets Lives=5

    // ---- 3. Begin the game the way GameRunner does ----
    FakeDisplay rawDisp;
    LightAir_DisplayCtrl disp(rawDisp);
    FakeAudio audio; FakeVib vib; FakeRGB rgb;
    LightAir_UICtrl ui(audio, vib, rgb);
    LightAir_RadioTestTransport transport;
    LightAir_Radio radio(transport, /*playerId*/ 2, 0x42, 0, 0);
    LightAir_GameRunner runner;
    runner.clearRoster();
    runner.addToRoster(2);
    runner.addToRoster(3);
    runner.addToRoster(4);

    *game.currentState = game.initialState;
    game.onBegin(disp, radio, &ui, runner);
    CHECK(*lives == 5, "on_begin applied Lives config");
    CHECK(*game.gameTimeLeft == 900, "time_left reset from config");

    // ---- 4. Incoming lit (DirectRadioRule path) ----
    const DirectRadioRule* litRule = nullptr;
    for (uint8_t i = 0; i < game.directRadioRuleCount; i++)
        if (game.directRadioRules[i].fromState == 0 &&
            game.directRadioRules[i].msgType == 0x10)
            litRule = &game.directRadioRules[i];
    CHECK(litRule, "IN_GAME LIT rule exists");
    CHECK(litRule->replySubType == DirectRadioRule::DYNAMIC_REPLY, "dynamic reply");

    RadioPacket lit = {};
    lit.senderId = 3; lit.team = 1; lit.msgType = 0x10;
    GameOutput out;
    litRule->onReceive(lit, kNearRssi, disp, out);
    CHECK(*lives == 4, "lit decremented lives");
    CHECK(out.radio.replyCount == 1, "reply queued by handler");
    CHECK(out.radio.replies[0].payloadLen == 1 &&
          out.radio.replies[0].payload[0] == 1, "reply sub-type TAKEN");

    // Immunity: same sender again within 3s -> IMMUNE (4), no life lost.
    out = GameOutput();
    litRule->onReceive(lit, kNearRssi, disp, out);
    CHECK(*lives == 4, "immunity blocked second lit");
    CHECK(out.radio.replies[0].payload[0] == 4, "reply sub-type IMMUNE");

    // ---- 5. Reply path: target confirmed SHONE -> points++ ----
    int* points = slotOf(game, "points");
    CHECK(points && *points == 0, "points start 0");
    RadioPacket orig = {};  orig.msgType = 0x10; orig.senderId = 2;
    RadioPacket rep  = {};  rep.msgType = 0x11; rep.senderId = 3;
    rep.payloadLen = 1; rep.payload[0] = 2;                // SHONE
    out = GameOutput();
    game.replyRadioRules[0].onReply(rep, orig, kNearRssi, disp, out);
    CHECK(*points == 1, "SHONE reply scored a point");

    // …and the shiner is told who went down.  The beam is invisible and
    // the UI cue cannot name a player, so this line is the whole feedback:
    // it has to survive the trip la.show -> DisplayCtrl tray -> screen.
    disp.update();
    CHECK(!strcmp(rawDisp.tray[0], "YLW SHONE!"), "shone line reached the tray");
    g_millis += 3001;                                  // the 3 s expiry
    disp.update();
    CHECK(rawDisp.tray[0][0] == '\0', "the line leaves when it expires");

    // ---- 6. Behaviour tick: trigger held -> shine, energy drops ----
    int* energy = slotOf(game, "energy");
    CHECK(energy && *energy == 50, "energy from config");
    InputReport inputs = {};
    inputs.buttonCount = 1;
    inputs.buttons[0].id = 0;                              // TRIG_1
    inputs.buttons[0].state = ButtonState::HELD;
    RadioReport rr = {};
    out = GameOutput();
    game.behaviors[0].onUpdate(inputs, rr, disp, out);
    CHECK(*energy == 49, "shine spent energy");
    CHECK(out.ui.count == 1, "enlight UI feedback");

    // Confirmed lit target -> unicast LIT queued.
    g_shineStatus = EnlightStatus::PLAYER_HIT;
    g_shineId = 4;
    out = GameOutput();
    game.behaviors[0].onUpdate(inputs, rr, disp, out);
    bool sent = false;
    for (uint8_t i = 0; i < out.radio.count; i++)
        if (!out.radio.msgs[i].isBroadcast && out.radio.msgs[i].targetId == 4 &&
            out.radio.msgs[i].msgType == 0x10) sent = true;
    CHECK(sent, "confirmed target got MSG_LIT");
    g_shineStatus = EnlightStatus::NO_HIT;

    // ---- 7. Countdown service: one game-second per real second ----
    int before = *game.gameTimeLeft;
    for (int t = 0; t < 300; t++) {                        // 3 s of ticks
        g_millis += 10;
        out = GameOutput();
        game.behaviors[0].onUpdate(inputs, rr, disp, out);
    }
    CHECK(*game.gameTimeLeft == before - 3, "countdown ticked 3s");

    // ---- 8. Rules: lives -> 0 fires IN_GAME->OUT_GAME transition ----
    *lives = 0;
    const StateRule* shoneRule = nullptr;
    for (uint8_t i = 0; i < game.ruleCount; i++)
        if (game.rules[i].fromState == 0 && game.rules[i].toState == 1)
            shoneRule = &game.rules[i];
    CHECK(shoneRule && shoneRule->condition, "shone rule exists");
    CHECK(shoneRule->condition(inputs, rr), "condition fires at 0 lives");
    out = GameOutput();
    shoneRule->onTransition(disp, out);                    // shone_times++
    int* shone = slotOf(game, "shone_times");
    CHECK(shone && *shone == 1, "shone_times incremented");

    // ---- 9. Totem program provider (patched {"cfg"}) ----
    const TotemProgramEntry* bonus = game.totemProgram(TotemRoleId_BONUS());
    CHECK(bonus && bonus->len > 0 && bonus->len <= 225, "bonus program provided");

    // ---- 10. Teams: who a beacon gets answered by, and from how far ----
    // Realized on the same shared instance; nothing above needs FFA any more.
    {
        CHECK(shared.load("games/teams.lua"), "teams realizes on the shared slot");
        const LightAir_Game& tg = shared.descriptor();

        *tg.currentState = tg.initialState;
        tg.onBegin(disp, radio, &ui, runner);

        // Down the player so the OUT_GAME handlers are the live ones.
        int* tlives = slotOf(tg, "lives");
        CHECK(tlives, "teams lives slot");
        *tlives = 0;
        const StateRule* down = nullptr;
        for (uint8_t i = 0; i < tg.ruleCount; i++)
            if (tg.rules[i].fromState == 0 && tg.rules[i].toState == 1 &&
                tg.rules[i].condition && tg.rules[i].condition(inputs, rr))
                down = &tg.rules[i];
        CHECK(down, "teams down rule fires at 0 lives");
        if (down) {
            *tg.currentState = 1;
            out = GameOutput();
            down->onTransition(disp, out);
        }
        g_millis += 31000;      // past the 30 s default respawn wait

        // RSSI reaches the Lua handler, and the answer is the whole signal:
        // a base out of range is not answered at all, so the only reply a
        // BASE ever hears is from a player actually respawning at it.
        // Until RSSI was plumbed through, handlers saw a constant 0 dBm and
        // every proximity gate passed.
        const DirectRadioRule* beacon = nullptr;
        for (uint8_t i = 0; i < tg.directRadioRuleCount; i++)
            if (tg.directRadioRules[i].fromState == 1 &&
                tg.directRadioRules[i].msgType == RadioMsg::MSG_BASE_BEACON)
                beacon = &tg.directRadioRules[i];
        CHECK(beacon && beacon->onReceive, "teams handles BASE_BEACON while down");

        RadioPacket bcn = {};
        bcn.senderId = 254; bcn.msgType = RadioMsg::MSG_BASE_BEACON;
        bcn.payloadLen = 1; bcn.payload[0] = 0;            // team-O base
        if (beacon && beacon->onReceive) {
            out = GameOutput();
            beacon->onReceive(bcn, /*rssi*/ -80, disp, out);
            CHECK(out.radio.replyCount == 0, "distant base is not answered at all");

            out = GameOutput();
            beacon->onReceive(bcn, /*rssi*/ -40, disp, out);
            CHECK(out.radio.replyCount == 1 && out.radio.replies[0].payloadLen == 1 &&
                  out.radio.replies[0].payload[0] >= 1,
                  "base in range gets the respawn sub-type that drives its anim");
        }

        // A pickup beacon is claimed only from arm's length, for the same
        // reason: the totem hands itself to whoever answers.
        const DirectRadioRule* bonus = nullptr;
        for (uint8_t i = 0; i < tg.directRadioRuleCount; i++)
            if (tg.directRadioRules[i].fromState == 0 &&
                tg.directRadioRules[i].msgType == RadioMsg::MSG_BONUS_BEACON)
                bonus = &tg.directRadioRules[i];
        CHECK(bonus && bonus->onReceive, "teams handles BONUS_BEACON in play");

        *tg.currentState = 0;
        RadioPacket bon = {};
        bon.senderId = 253; bon.msgType = RadioMsg::MSG_BONUS_BEACON;
        bon.payloadLen = 1; bon.payload[0] = 0;            // 0 = ready
        if (bonus && bonus->onReceive) {
            out = GameOutput();
            bonus->onReceive(bon, /*rssi*/ -80, disp, out);
            CHECK(out.radio.replyCount == 0, "distant pickup is not claimed");

            out = GameOutput();
            bonus->onReceive(bon, /*rssi*/ -40, disp, out);
            CHECK(out.radio.replyCount == 1 && out.radio.replies[0].payloadLen == 1,
                  "pickup in range is claimed by this player");
        }
    }

    // ---- 11. Fault policy: log, notify, continue ----
    {
        LightAir_LuaGame* faulty = new LightAir_LuaGame();
        if (!faulty->load("test/host/fixtures/faulty.lua")) {
            CHECK(false, "faulty fixture loads");
            return 1;
        }
        const LightAir_Game& fg = faulty->descriptor();
        *fg.currentState = fg.initialState;
        fg.onBegin(disp, radio, &ui, runner);
        CHECK(faulty->faultStats().total == 0, "clean begin, zero faults");

        // The update handler errors every tick: the game must stay in its
        // state and keep counting, never ending the match.
        for (int t = 0; t < 5; t++) {
            out = GameOutput();
            fg.behaviors[0].onUpdate(inputs, rr, disp, out);
        }
        CHECK(*fg.currentState == fg.initialState, "faults do not change state");
        CHECK(faulty->faultStats().total == 5, "every fault counted");
        CHECK(faulty->faultStats().perSite[(uint8_t)LightAir_LuaGame::FaultSite::Update] == 5,
              "counted per site");
        CHECK(faulty->faultStats().lastError[0] != 0, "last error recorded");

        // A handler that dies after a partial mutation: the pre-error
        // effect stands (documented caveat) and the empty reply still
        // goes out so the sender never times out.
        int* flives = nullptr;
        for (uint8_t i = 0; i < fg.monitorCount; i++)
            if (strcmp(fg.monitorVars[i].name, "lives") == 0)
                flives = fg.monitorVars[i].asInt;
        CHECK(flives && *flives == 3, "fixture lives start at 3");
        out = GameOutput();
        fg.directRadioRules[0].onReceive(lit, kNearRssi, disp, out);
        CHECK(*flives == 2, "partial effect before the error stands");
        // The handler's return value is the reply, and a handler that threw
        // produced none — so the fault is silent on the wire too, rather than
        // inventing an answer nobody can act on.
        CHECK(out.radio.replyCount == 0, "no reply invented after a handler fault");
        CHECK(faulty->faultStats().perSite[(uint8_t)LightAir_LuaGame::FaultSite::Message] == 1,
              "message fault counted");

        // The healthy parts keep working: the transition rule still fires.
        *flives = 0;
        CHECK(fg.rules[0].condition(inputs, rr), "healthy rule still evaluates");

        // on_begin failure is the one fatal case: refuse to play.
        LightAir_LuaGame* fb = new LightAir_LuaGame();
        if (!fb->load("test/host/fixtures/faulty_begin.lua")) {
            CHECK(false, "begin fixture loads");
            return 1;
        }
        const LightAir_Game& bg = fb->descriptor();
        *bg.currentState = bg.initialState;
        bg.onBegin(disp, radio, &ui, runner);
        CHECK(*bg.currentState == bg.scoringState, "failed on_begin refuses to play");
        CHECK(fb->faultStats().perSite[(uint8_t)LightAir_LuaGame::FaultSite::Begin] == 1,
              "begin fault counted");
    }

    // ---- 12. A CP point is announced, not just counted ----------------
    // King of Hill's whole score comes from CP_SCORE broadcasts; before
    // this the only sign one had landed was a digit changing on the LCD.
    {
        runner.clearTotems();
        runner.addTotem(200, TotemRoleId_CP());        // one hill, id 200
        CHECK(shared.load("games/kingofhill.lua"), "kingofhill loads");
        const LightAir_Game& kh = shared.descriptor();
        *kh.currentState = kh.initialState;
        kh.onBegin(disp, radio, &ui, runner);

        const DirectRadioRule* score = nullptr;
        for (uint8_t i = 0; i < kh.directRadioRuleCount; i++)
            if (kh.directRadioRules[i].fromState == 0 &&
                kh.directRadioRules[i].msgType == RadioMsg::MSG_CP_SCORE)
                score = &kh.directRadioRules[i];
        CHECK(score && score->onReceive, "CP_SCORE handled in play");

        int* khPoints = slotOf(kh, "points");
        RadioPacket sc = {};
        sc.senderId = 200; sc.msgType = RadioMsg::MSG_CP_SCORE;
        sc.payloadLen = 1; sc.payload[0] = (uint8_t)(radio.playerId() - 1);  // our slot
        out = GameOutput();
        if (score) score->onReceive(sc, kNearRssi, disp, out);
        disp.update();
        CHECK(khPoints && *khPoints == 1, "CP point counted");
        CHECK(!strcmp(rawDisp.tray[0], "CP 1 +1"), "CP point named its hill on the tray");
        CHECK(out.ui.count == 1, "CP point rang a cue");

        // Another player's point is silent here: no cue, no line, no score.
        sc.payload[0] = (uint8_t)(radio.playerId() + 3);
        out = GameOutput();
        rawDisp.tray[0][0] = '\0';
        if (score) score->onReceive(sc, kNearRssi, disp, out);
        disp.update();
        CHECK(*khPoints == 1, "someone else's CP point is not ours");
        CHECK(rawDisp.tray[0][0] == '\0' && out.ui.count == 0, "and says nothing");
        runner.clearTotems();
    }

    // ---- 13. The keypad half of the input stack, through a fixture ----
    // A ruleset can name a key, ask its state, or walk what the report
    // holds without knowing how the keypad is built.  Nothing here
    // depends on the six-key matrix this firmware happens to register.
    {
        LightAir_LuaGame* kg = new LightAir_LuaGame();
        CHECK(kg->load("test/host/fixtures/keys.lua"), "keys fixture loads");
        const LightAir_Game& kd = kg->descriptor();
        *kd.currentState = kd.initialState;
        kd.onBegin(disp, radio, &ui, runner);

        int* active = slotOf(kd, "active");
        int* aDown  = slotOf(kd, "a_down");
        const char* first   = nullptr;
        const char* bState  = nullptr;
        for (uint8_t i = 0; i < kd.monitorCount; i++) {
            if (kd.monitorVars[i].type != VarType::CHARS) continue;
            if (!strcmp(kd.monitorVars[i].name, "first"))   first  = kd.monitorVars[i].asChars;
            if (!strcmp(kd.monitorVars[i].name, "b_state")) bState = kd.monitorVars[i].asChars;
        }
        CHECK(active && aDown && first && bState, "fixture slots bound");

        RadioReport krr = {};
        InputReport keys = {};

        // Nothing pressed: an empty report reads as every key up.
        out = GameOutput();
        kd.behaviors[0].onUpdate(keys, krr, disp, out);
        CHECK(*active == 0 && *aDown == 0, "empty report: no keys");
        CHECK(!strcmp(bState, "off"), "a key the report omits is off");
        CHECK(!strcmp(first, "-"), "key_at past the end is nil");

        // Three keys at once, each in a different state — including the
        // one-poll release edge a ruleset would trigger on.
        keys.keyEventCount = 3;
        keys.keyEvents[0] = { 0, 'A', KeyState::HELD };
        keys.keyEvents[1] = { 0, 'B', KeyState::RELEASED_HELD };
        keys.keyEvents[2] = { 0, 'V', KeyState::PRESSED };
        out = GameOutput();
        kd.behaviors[0].onUpdate(keys, krr, disp, out);
        CHECK(*active == 3, "every active key is walkable");
        CHECK(*aDown == 1, "held counts as down");
        CHECK(!strcmp(first, "A:held"), "key_at reports key and state");
        CHECK(!strcmp(bState, "released_held"), "the release edge is visible");
        CHECK(slotOf(kd, "pad") && *slotOf(kd, "pad") == 0, "key_at reports the keypad id");

        // A released key is not down, and a chord needs both halves.
        keys.keyEventCount = 1;
        keys.keyEvents[0] = { 0, 'B', KeyState::RELEASED };
        out = GameOutput();
        kd.behaviors[0].onUpdate(keys, krr, disp, out);
        CHECK(*aDown == 0, "a key that stopped being listed is up");
        CHECK(!kd.rules[0].condition(keys, krr), "no chord: rule holds");
        keys.keyEventCount = 2;
        keys.keyEvents[0] = { 0, '<', KeyState::PRESSED };
        keys.keyEvents[1] = { 0, '>', KeyState::PRESSED };
        CHECK(kd.rules[0].condition(keys, krr), "both halves down: chord fires");
        delete kg;
    }

    // ---- 14. FestaSportSasso: an endless match made of 500 s turns ----
    // Drives one whole turn on the real binding: welcome screen -> BASE
    // start -> shone -> clock out -> stats screen -> the staff's A+B
    // chord, which hands the projector on and welcomes the next visitor.
    {
        bool ok = shared.load("games/festasportsasso.lua");
        CHECK(ok, "festasportsasso loads");
        const LightAir_Game& fs = shared.descriptor();
        // The two structural consequences of a game that never ends.
        CHECK(fs.scoringState == 255, "no scoring state: score collection never arms");
        CHECK(fs.gameTimeLeft == nullptr, "no time reported to totems: no self-revert");

        *fs.currentState = fs.initialState;
        fs.onBegin(disp, radio, &ui, runner);
        CHECK(*fs.currentState == 0, "turn starts in PRE_START");

        int* counter = slotOf(fs, "counter");
        int* clock   = slotOf(fs, "time_left");
        int* fLives  = slotOf(fs, "lives");
        int* fPoints = slotOf(fs, "points");
        const char* tally = nullptr;
        for (uint8_t i = 0; i < fs.monitorCount; i++)
            if (fs.monitorVars[i].type == VarType::CHARS &&
                strcmp(fs.monitorVars[i].name, "tally") == 0)
                tally = fs.monitorVars[i].asChars;
        CHECK(counter && *counter == 2, "counter = 0 played before + projector digit 2");
        CHECK(clock && *clock == 500, "turn clock loaded from SubTime");
        CHECK(tally != nullptr, "tally text slot bound");

        // Rule lookup by (from,to) — the turn's four transitions.
        const StateRule *start = nullptr, *down = nullptr,
                        *over = nullptr, *restart = nullptr;
        for (uint8_t i = 0; i < fs.ruleCount; i++) {
            const StateRule& r = fs.rules[i];
            if (r.fromState == 0 && r.toState == 1) start   = &r;
            if (r.fromState == 1 && r.toState == 2) down    = &r;
            if (r.fromState == 2 && r.toState == 3) over    = &r;
            if (r.fromState == 3 && r.toState == 0) restart = &r;
        }
        CHECK(start && down && over && restart, "turn transitions present");

        // PRE_START: a lit costs nothing — the visitor is not playing yet.
        const DirectRadioRule* preLit = nullptr;
        const DirectRadioRule* preBase = nullptr;
        for (uint8_t i = 0; i < fs.directRadioRuleCount; i++) {
            const DirectRadioRule& r = fs.directRadioRules[i];
            if (r.fromState != 0) continue;
            if (r.msgType == RadioMsg::MSG_LIT)         preLit  = &r;
            if (r.msgType == RadioMsg::MSG_BASE_BEACON) preBase = &r;
        }
        CHECK(preLit && preBase, "PRE_START handles lit and BASE beacons");
        RadioPacket p = {};
        p.senderId = 3; p.msgType = RadioMsg::MSG_LIT;
        out = GameOutput();
        if (preLit) preLit->onReceive(p, kNearRssi, disp, out);
        CHECK(*fLives == 3, "PRE_START: no lives lost");
        CHECK(out.radio.replyCount == 1 && out.radio.replies[0].payload[0] == 3,
              "PRE_START: reply sub-type DOWN");

        // A teamless BASE hands the turn over to the visitor.
        InputReport keys = {};
        RadioReport nrr = {};
        CHECK(start && !start->condition(keys, nrr), "no BASE yet: still PRE_START");
        p = RadioPacket();
        p.senderId = 200; p.msgType = RadioMsg::MSG_BASE_BEACON;
        p.payloadLen = 1; p.payload[0] = 0xFF;              // teamless base
        out = GameOutput();
        if (preBase) preBase->onReceive(p, /*rssi*/ -80, disp, out);
        CHECK(out.radio.replyCount == 0, "a base heard from across the field is ignored");
        CHECK(start && !start->condition(keys, nrr), "distant base does not start a turn");
        out = GameOutput();
        if (preBase) preBase->onReceive(p, kNearRssi, disp, out);
        CHECK(out.radio.replyCount == 1 && out.radio.replies[0].payload[0] == 2,
              "BASE reply carries slot+1 so it animates");
        CHECK(start && start->condition(keys, nrr), "BASE respawn starts the turn");
        // The clock belongs to the turn, not to the hand-over: however
        // long the projector waited on the welcome screen, the visitor
        // starts on a full sub_time.
        *clock = 7;
        out = GameOutput();
        if (start) start->onTransition(disp, out);
        *fs.currentState = 1;
        CHECK(*clock == 500 && *fLives == 3, "the BASE starts a full turn");

        // One confirmed lit on another player, then our own shone — the
        // real way, three incoming lits from the same sender, so the
        // third one runs std.lit_target's on_shone and actually sets who
        // gets credited on the DOWN screen.
        const ReplyRadioRule* shoneReply = nullptr;
        for (uint8_t i = 0; i < fs.replyRadioRuleCount; i++)
            if (fs.replyRadioRules[i].eventType == RadioEventType::ReplyReceived)
                shoneReply = &fs.replyRadioRules[i];
        CHECK(shoneReply, "lit reply rule present");
        RadioPacket orig2 = {}; orig2.msgType = RadioMsg::MSG_LIT; orig2.senderId = 2;
        RadioPacket rep2  = {}; rep2.msgType = RadioMsg::MSG_LIT + 1; rep2.senderId = 4;
        rep2.payloadLen = 1; rep2.payload[0] = 2;           // SHONE
        out = GameOutput();
        if (shoneReply) shoneReply->onReply(rep2, orig2, kNearRssi, disp, out);

        const DirectRadioRule* activeLit = nullptr;
        for (uint8_t i = 0; i < fs.directRadioRuleCount; i++)
            if (fs.directRadioRules[i].fromState == 1 &&
                fs.directRadioRules[i].msgType == RadioMsg::MSG_LIT)
                activeLit = &fs.directRadioRules[i];
        CHECK(activeLit, "ACTIVE handles incoming lit");
        // Three different senders, so the per-sender immunity window
        // (3 s) never blocks a hit; the last one is who takes the credit.
        RadioPacket lit3 = {}; lit3.msgType = RadioMsg::MSG_LIT;
        uint8_t litSenders[3] = { 2, 5, 6 };
        for (uint8_t s : litSenders) {
            lit3.senderId = s;
            out = GameOutput();
            if (activeLit) activeLit->onReceive(lit3, kNearRssi, disp, out);
        }
        CHECK(*fLives == 0, "three lits from different senders empty the lives");
        CHECK(down && down->condition(keys, nrr), "0 lives sends the visitor down");
        out = GameOutput();
        if (down) down->onTransition(disp, out);
        *fs.currentState = 2;
        CHECK(fs.winnerVarCount == 2 && *fs.winnerVars[1].value == 1,
              "shone_times counted");
        disp.update();
        CHECK(!strcmp(rawDisp.tray[0], "SHONE by RED"), "down screen credits the shiner");
        CHECK(!strcmp(rawDisp.tray[1], "GO TO BASE"), "...and what to do about it");

        // The turn clock runs out while down: stats screen, frozen.
        *clock = 0;
        CHECK(over && over->condition(keys, nrr), "clock out ends the turn");
        out = GameOutput();
        if (over) over->onTransition(disp, out);
        *fs.currentState = 3;
        CHECK(tally && strcmp(tally, "1/1") == 0, "stats screen shows lit/shone");
        // "Time up!" flashes on top for 3 s; once it expires the points
        // line — the number the visitor actually cares about — takes the
        // top row for the rest of the screen's indefinite wait.  No CP
        // point landed in this script, so the score is players_lit alone.
        g_millis += 3001;
        disp.update();
        CHECK(!strcmp(rawDisp.tray[0], "#2 POINTS: 1"),
              "stats screen leads with player number and turn score");

        // The scoring formula itself: 10 per CP totem point, 1 per player
        // lit.  5 CP points + 3 lit = 53, the worked example from the spec.
        // players_lit has no monitor binding to poke directly, so reach 3
        // the real way: two more confirmed SHONE replies (already at 1).
        if (shoneReply) {
            shoneReply->onReply(rep2, orig2, kNearRssi, disp, out);
            shoneReply->onReply(rep2, orig2, kNearRssi, disp, out);
        }
        *fPoints = 5;
        out = GameOutput();
        if (over) over->onTransition(disp, out);
        g_millis += 3001;
        disp.update();
        CHECK(!strcmp(rawDisp.tray[0], "#2 POINTS: 53"),
              "score = 10*totem points + players lit");

        // Only the staff's A+B chord starts the next visitor.
        keys.keyEventCount = 1;
        keys.keyEvents[0] = { 0, 'A', KeyState::PRESSED };
        CHECK(restart && !restart->condition(keys, nrr), "A alone does not restart");
        keys.keyEventCount = 2;
        keys.keyEvents[1] = { 0, 'B', KeyState::HELD };
        CHECK(restart && restart->condition(keys, nrr), "A+B restarts the turn");
        out = GameOutput();
        if (restart) restart->onTransition(disp, out);
        *fs.currentState = 0;
        CHECK(*counter == 12, "counter's first part bumped, projector digit kept");
        CHECK(*clock == 500 && *fLives == 3 && *fPoints == 0, "turn stats reset");
        CHECK(*fs.winnerVars[1].value == 0, "shone_times reset with the turn");
    }

    // ---- 15. `bar` monitor rows reach the display with live pointers ----
    // A bar's timing belongs to whoever owns the wait, not to the display, so
    // what matters here is that both pointers land on the right SLOTS and
    // still read through after the owner writes them.  A copied value would
    // pass a first assertion and then freeze.
    {
        // Realized on the shared slot, as every game after the first is: the
        // instance pool is deliberately small and a reload is how the menu
        // switches games anyway.
        CHECK(shared.load("test/host/fixtures/bar.lua"), "bar fixture loads");
        const LightAir_Game& bd = shared.descriptor();
        *bd.currentState = bd.initialState;
        bd.onBegin(disp, radio, &ui, runner);

        const MonitorVar* owned = nullptr;   // energy, with a start_var
        const MonitorVar* selfT = nullptr;   // respawn_zero, without one
        const MonitorVar* plain = nullptr;   // the ordinary row on the other screen
        for (uint8_t i = 0; i < bd.monitorCount; i++) {
            const MonitorVar& m = bd.monitorVars[i];
            if (m.type == VarType::BAR && !strcmp(m.name, "energy"))       owned = &m;
            if (m.type == VarType::BAR && !strcmp(m.name, "respawn_zero")) selfT = &m;
            if (m.type == VarType::INT && !strcmp(m.name, "energy"))       plain = &m;
        }
        CHECK(owned && selfT, "both bar rows synthesized as VarType::BAR");
        CHECK(plain, "a non-bar row beside them stays VarType::INT");

        if (owned && selfT && plain) {
            CHECK(owned->barTrigger == 0, "bar_at reached the descriptor");
            CHECK(owned->barFill && *owned->barFill == 10000, "fill_var points at reload_ms");
            CHECK(owned->barStart && *owned->barStart == 0, "start_var points at reload");
            CHECK(selfT->barStart == nullptr, "no start_var leaves the display self-starting");
            CHECK(selfT->barWidth == 30, "width reached the descriptor");
            CHECK(owned->barWidth == 0, "an unset width defers to the display default");
            CHECK(owned->asInt == plain->asInt,
                  "the bar and the plain row address the same energy slot");

            // The owner writes; the binding must see it, because these are
            // pointers into the slots and not copies taken at load.
            *owned->asInt = 0;
            int* reload     = slotOf(bd, "reload");
            int* reloadMs   = slotOf(bd, "reload_ms");
            CHECK(reload == owned->barStart, "start_var resolved to the reload slot");
            CHECK(reloadMs == owned->barFill, "fill_var resolved to the reload_ms slot");
            if (reload && reloadMs && owned->barStart && owned->barFill) {
                *reload = 4321;
                *reloadMs = 7000;
                CHECK(*owned->barStart == 4321, "the start pointer reads the owner's write");
                CHECK(*owned->barFill  == 7000, "the fill pointer reads the owner's write");
            } else {
                CHECK(false, "bar fixture vars reachable through the descriptor");
            }
        }
    }

    // ---- 16. A shine action's TOTAL length is the burst, not each note ----
    // Several projectors have to be tellable apart by their pattern, not by
    // the pitch of one note — so a multi-step action has to fit inside the
    // beam rather than stretching to N times its length.
    {
        typedef LightAir_UICtrl::UIAction A;
        const uint16_t burst = 300;

        auto totalOf = [&](const A& a) {
            uint32_t t = 0;
            for (uint8_t i = 0; i < a.stepCount; i++)
                t += LightAir_UICtrl::burstStepMs(a, i, burst);
            return t;
        };

        // One note: the whole burst, as before.
        A one = {}; one.stepCount = 1; one.durations[0] = 10;
        CHECK(LightAir_UICtrl::burstStepMs(one, 0, burst) == burst,
              "a single-step action still lasts the whole burst");

        // Three EVEN notes: one burst between them, not three.
        A three = {}; three.stepCount = 3;
        three.durations[0] = three.durations[1] = three.durations[2] = 1;
        CHECK(totalOf(three) == burst, "three even notes total exactly one burst");
        CHECK(LightAir_UICtrl::burstStepMs(three, 0, burst) == 100,
              "even notes divide the burst evenly");

        // A declared SHAPE is preserved as a ratio: 1:3 stays 1:3.
        A shaped = {}; shaped.stepCount = 2;
        shaped.durations[0] = 1; shaped.durations[1] = 3;
        CHECK(totalOf(shaped) == burst, "a shaped action totals exactly one burst");
        CHECK(LightAir_UICtrl::burstStepMs(shaped, 0, burst) == 75 &&
              LightAir_UICtrl::burstStepMs(shaped, 1, burst) == 225,
              "the declared durations are kept as a ratio");

        // Rounding must not drift: 3 notes into 100 ms still totals 100.
        CHECK(LightAir_UICtrl::burstStepMs(three, 0, 100) +
              LightAir_UICtrl::burstStepMs(three, 1, 100) +
              LightAir_UICtrl::burstStepMs(three, 2, 100) == 100,
              "an indivisible burst still totals exactly, with no drift");

        // No shape declared at all: fall back to an even split.
        A flat = {}; flat.stepCount = 2;
        CHECK(totalOf(flat) == burst, "zero durations split the burst evenly");

        // A note squeezed out by a tiny burst still advances the ticker.
        A many = {}; many.stepCount = 4;
        for (uint8_t i = 0; i < 4; i++) many.durations[i] = 1;
        for (uint8_t i = 0; i < 4; i++)
            CHECK(LightAir_UICtrl::burstStepMs(many, i, 2) > 0,
                  "no step is ever zero-length");

        CHECK(LightAir_UICtrl::burstStepMs(one, 3, burst) == 0,
              "a step past the end is nothing");

        // ...and that executeStep actually USES it.  Checking the arithmetic
        // alone would pass even if the call site still handed every step the
        // whole burst, which is exactly the bug being fixed.
        {
            LightAir_UICtrl u2(audio, vib, rgb);
            A shaped2 = {};
            shaped2.stepCount   = 2;
            shaped2.durations[0] = 1;   shaped2.durations[1] = 3;
            shaped2.soundFreqs[0] = 1000; shaped2.soundFreqs[1] = 500;
            shaped2.priority    = 2;
            u2.setEnlightAction(&shaped2);

            hostTicker().lastMs = 0;
            u2.triggerEnlight(burst);
            CHECK(hostTicker().lastMs == 75,
                  "the first note of a 1:3 action is scheduled for its share of "
                  "the burst, not the whole of it");

            // And the standard single-step action still takes the lot.
            LightAir_UICtrl u3(audio, vib, rgb);
            hostTicker().lastMs = 0;
            u3.triggerEnlight(burst);
            CHECK(hostTicker().lastMs == burst,
                  "a single-step action is still scheduled for the whole burst");
        }
    }

    printf(failures == 0 ? "\nLUAGAME HOST TESTS PASS\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}

// avoid dragging TotemRoleIds include ordering issues into the test
#include "totem/TotemRoleIds.h"
uint8_t TotemRoleId_BONUS() { return TotemRoleId::BONUS; }
uint8_t TotemRoleId_CP()    { return TotemRoleId::CP; }
