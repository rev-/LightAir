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
struct FakeDisplay : LightAir_Display {
    void clear() override {}
    void setColor(bool) override {}
    void fillRect(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void drawRect(uint8_t, uint8_t, uint8_t, uint8_t) override {}
    void drawBitmap(uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) override {}
    void print(uint8_t, uint8_t, const char*) override {}
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
          "7 sequential loads used one registry slot");

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
    CHECK(game.directRadioRuleCount == 2, "ffa direct rules (LIT in 2 states)");
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
    litRule->onReceive(lit, disp, out);
    CHECK(*lives == 4, "lit decremented lives");
    CHECK(out.radio.replyCount == 1, "reply queued by handler");
    CHECK(out.radio.replies[0].payloadLen == 1 &&
          out.radio.replies[0].payload[0] == 1, "reply sub-type TAKEN");

    // Immunity: same sender again within 3s -> IMMUNE (4), no life lost.
    out = GameOutput();
    litRule->onReceive(lit, disp, out);
    CHECK(*lives == 4, "immunity blocked second lit");
    CHECK(out.radio.replies[0].payload[0] == 4, "reply sub-type IMMUNE");

    // ---- 5. Reply path: target confirmed SHONE -> points++ ----
    int* points = slotOf(game, "points");
    CHECK(points && *points == 0, "points start 0");
    RadioPacket orig = {};  orig.msgType = 0x10; orig.senderId = 2;
    RadioPacket rep  = {};  rep.msgType = 0x11; rep.senderId = 3;
    rep.payloadLen = 1; rep.payload[0] = 2;                // SHONE
    out = GameOutput();
    game.replyRadioRules[0].onReply(rep, orig, disp, out);
    CHECK(*points == 1, "SHONE reply scored a point");

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

    // ---- 10. Fault policy: log, notify, continue ----
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
        fg.directRadioRules[0].onReceive(lit, disp, out);
        CHECK(*flives == 2, "partial effect before the error stands");
        CHECK(out.radio.replyCount == 1 && out.radio.replies[0].payloadLen == 0,
              "empty reply still sent after handler fault");
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

    printf(failures == 0 ? "\nLUAGAME HOST TESTS PASS\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}

// avoid dragging TotemRoleIds include ordering issues into the test
#include "totem/TotemRoleIds.h"
uint8_t TotemRoleId_BONUS() { return TotemRoleId::BONUS; }
