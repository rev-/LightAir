// Host test: feed reference-encoder programs into the real TotemVM and
// assert role behaviour (beacons, animations, state changes, scoring).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ArduinoLog.h"
uint32_t g_millis = 0;
HostLog Log;

#include "totem/LightAir_TotemVM.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static std::vector<uint8_t> readFile(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { printf("cannot open %s\n", p.c_str()); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v(n);
    if (fread(v.data(), 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    return v;
}

// ---- output inspection helpers ----
static int countBcast(const LightAir_TotemOutput& o, uint8_t msg) {
    int n = 0;
    for (uint8_t i = 0; i < o.radio.count; i++)
        if (o.radio.msgs[i].isBroadcast && o.radio.msgs[i].msgType == msg) n++;
    return n;
}
static const RadioOutMsg* lastBcast(const LightAir_TotemOutput& o, uint8_t msg) {
    const RadioOutMsg* m = nullptr;
    for (uint8_t i = 0; i < o.radio.count; i++)
        if (o.radio.msgs[i].isBroadcast && o.radio.msgs[i].msgType == msg)
            m = &o.radio.msgs[i];
    return m;
}
static int countAnim(const LightAir_TotemOutput& o, TotemUIEvent ev) {
    int n = 0;
    for (uint8_t i = 0; i < o.ui.count; i++)
        if (o.ui.cmds[i].event == ev) n++;
    return n;
}
static const TotemUICmd* lastAnim(const LightAir_TotemOutput& o, TotemUIEvent ev) {
    const TotemUICmd* c = nullptr;
    for (uint8_t i = 0; i < o.ui.count; i++)
        if (o.ui.cmds[i].event == ev) c = &o.ui.cmds[i];
    return c;
}

static RadioPacket mkPkt(uint8_t msgType, uint8_t sender, uint8_t team,
                         std::vector<uint8_t> payload) {
    RadioPacket p = {};
    p.senderId = sender;
    p.team = team;
    p.msgType = msgType;
    p.payloadLen = (uint8_t)payload.size();
    memcpy(p.payload, payload.data(), payload.size());
    return p;
}

static LightAir_TotemActivation kAct = { 1, 0x42, 900, false, 0 };

// Run vm.update() across a time span, collecting output.
static void runFor(LightAir_TotemVM& vm, uint32_t ms, LightAir_TotemOutput& out) {
    uint32_t end = g_millis + ms;
    while (g_millis < end) {
        g_millis += 10;
        vm.update(out);
    }
}

int main(int argc, char** argv) {
    std::string dir = argv[1];

    // ================= BASE (team 0) =================
    {
        printf("BASE(0):\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_base0.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load base0");

        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);
        CHECK(countAnim(out, TotemUIEvent::BaseIdle) == 1, "idle anim on activate");
        const TotemUICmd* idle = lastAnim(out, TotemUIEvent::BaseIdle);
        CHECK(idle && idle->periodMs == 1800 && idle->pulseCount == 1, "team-0 rhythm");

        out = LightAir_TotemOutput();
        runFor(vm, 3050, out);
        CHECK(countBcast(out, 0x56) == 3, "3 beacons in ~3s");
        const RadioOutMsg* b = lastBcast(out, 0x56);
        CHECK(b && b->payloadLen == 1 && b->payload[0] == 0, "beacon payload = team 0");

        // Empty auto-reply (sub-type absent) must NOT trigger the respawn anim.
        out = LightAir_TotemOutput();
        RadioPacket empty = mkPkt(0x57, 3, 1, {});
        vm.onPacket(empty, -40, out);
        CHECK(countAnim(out, TotemUIEvent::Respawn) == 0, "empty reply ignored");

        // Intentional respawn reply (sub-type >= 1) triggers the anim.  A team
        // base uses the respawning *player's* colour too — its own team is
        // already on the idle ring, so the anim identifies who came back.
        out = LightAir_TotemOutput();
        RadioPacket resp = mkPkt(0x57, 3, 1, {2});
        vm.onPacket(resp, -40, out);
        CHECK(countAnim(out, TotemUIEvent::Respawn) == 1, "respawn anim");
        const TotemUICmd* r = lastAnim(out, TotemUIEvent::Respawn);
        CHECK(r && r->r == PlayerColors::kColors[3][0] &&
                   r->g == PlayerColors::kColors[3][1], "respawn in sender player colour");
    }

    // ================= BASE (teamless) =================
    {
        printf("BASE(any):\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_baseA.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load baseA");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);
        const TotemUICmd* idle = lastAnim(out, TotemUIEvent::BaseIdle);
        CHECK(idle && idle->r == 255 && idle->g == 255 && idle->b == 255,
              "teamless idle is white");
        out = LightAir_TotemOutput();
        runFor(vm, 1050, out);
        const RadioOutMsg* b = lastBcast(out, 0x56);
        CHECK(b && b->payload[0] == 0xFF, "teamless beacon payload 0xFF");
        // Respawn anim uses the player's own colour.
        out = LightAir_TotemOutput();
        RadioPacket resp = mkPkt(0x57, 6, 0, {1});
        vm.onPacket(resp, -40, out);
        const TotemUICmd* r = lastAnim(out, TotemUIEvent::Respawn);
        CHECK(r && r->r == PlayerColors::kColors[6][0] &&
                   r->g == PlayerColors::kColors[6][1], "player colour respawn");
    }

    // ================= BONUS =================
    {
        printf("BONUS:\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_bonus.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load bonus");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);
        CHECK(countAnim(out, TotemUIEvent::BonusIdle) == 1, "bonus idle");

        out = LightAir_TotemOutput();
        runFor(vm, 4050, out);
        CHECK(countBcast(out, 0x5E) == 2, "ready beacons every 2s");

        // Any reply claims it -> claim anim, then silence during cooldown.
        out = LightAir_TotemOutput();
        RadioPacket claim = mkPkt(0x5F, 4, 0, {});
        vm.onPacket(claim, -40, out);
        CHECK(countAnim(out, TotemUIEvent::Bonus) == 1, "claim anim");

        out = LightAir_TotemOutput();
        runFor(vm, 10000, out);
        CHECK(countBcast(out, 0x5E) == 0, "silent during cooldown");

        // After 30 s (cfg default) it re-arms: idle anim + beacons resume.
        // (drain per second: the real driver flushes the output every tick,
        // and the queue holds only RADIO_OUT_MAX entries)
        int idles = 0, beacons = 0;
        for (int s = 0; s < 25; s++) {
            out = LightAir_TotemOutput();
            runFor(vm, 1000, out);
            idles   += countAnim(out, TotemUIEvent::BonusIdle);
            beacons += countBcast(out, 0x5E);
        }
        CHECK(idles == 1, "re-armed idle");
        CHECK(beacons >= 1, "beacons resumed");
    }

    // ================= FLAG (team 0) =================
    {
        printf("FLAG(0):\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_flag0.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load flag0");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);
        CHECK(countAnim(out, TotemUIEvent::FlagIdle) == 1, "flag idle");

        out = LightAir_TotemOutput();
        runFor(vm, 1550, out);
        CHECK(countBcast(out, 0x58) == 3, "flag beacons every 500ms");
        const RadioOutMsg* b = lastBcast(out, 0x58);
        CHECK(b && b->payloadLen == 2 && b->payload[0] == 0 && b->payload[1] == 0,
              "beacon = FLAG_IN, team 0");

        // Wrong team's flag event: ignored.
        out = LightAir_TotemOutput();
        RadioPacket other = mkPkt(0x50, 5, 1, {1, 1});
        vm.onPacket(other, -40, out);
        CHECK(countAnim(out, TotemUIEvent::FlagTaken) == 0, "other flag ignored");

        // Our flag TAKEN -> away: FlagMissing bg + FlagTaken flash, beacons stop.
        out = LightAir_TotemOutput();
        RadioPacket taken = mkPkt(0x50, 5, 1, {1, 0});
        vm.onPacket(taken, -40, out);
        CHECK(countAnim(out, TotemUIEvent::FlagMissing) == 1, "missing bg");
        CHECK(countAnim(out, TotemUIEvent::FlagTaken) == 1, "taken flash");
        out = LightAir_TotemOutput();
        runFor(vm, 2000, out);
        CHECK(countBcast(out, 0x58) == 0, "no beacons while away");

        // SCORED returns it home: idle restored then return flash, beacons resume.
        out = LightAir_TotemOutput();
        RadioPacket scored = mkPkt(0x50, 5, 1, {3, 0});
        vm.onPacket(scored, -40, out);
        CHECK(countAnim(out, TotemUIEvent::FlagIdle) == 1, "idle restored");
        CHECK(countAnim(out, TotemUIEvent::FlagReturn) == 1, "return flash");
        out = LightAir_TotemOutput();
        runFor(vm, 1050, out);
        CHECK(countBcast(out, 0x58) == 2, "beacons resumed");
    }

    // ================= CP =================
    {
        printf("CP:\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_cp.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load cp");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);
        CHECK(countAnim(out, TotemUIEvent::CPIdle) == 1, "cp idle");

        // Empty first window: neutral beacon 0xFF.
        out = LightAir_TotemOutput();
        runFor(vm, 2050, out);
        const RadioOutMsg* b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 0xFF, "neutral beacon");

        // One team-1 player present (sub-type 2) -> owner = slot 1.
        out = LightAir_TotemOutput();
        RadioPacket pres = mkPkt(0x53, 4, 1, {2});
        vm.onPacket(pres, -40, out);
        runFor(vm, 2000, out);
        const TotemUICmd* c = lastAnim(out, TotemUIEvent::Control);
        CHECK(c && c->r == 0xFE && c->g == 1, "control anim slot form");
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1, "owner beacon slot 1");

        // Held alone for 10s (presence in every window) -> score for slot 1.
        // Fresh output per window: the driver flushes every tick.
        int scores = 0, scoreSlot = -1;
        for (int w = 0; w < 6; w++) {
            out = LightAir_TotemOutput();
            RadioPacket p2 = mkPkt(0x53, 4, 1, {2});
            vm.onPacket(p2, -40, out);
            runFor(vm, 2000, out);
            if (countBcast(out, 0x54)) {
                scores += countBcast(out, 0x54);
                scoreSlot = lastBcast(out, 0x54)->payload[0];
            }
        }
        CHECK(scores == 1, "cp score broadcast exactly once in 12s");
        CHECK(scoreSlot == 1, "score for slot 1");

        // Contested window: both sub-types -> contest anim, owner held.
        out = LightAir_TotemOutput();
        RadioPacket pa = mkPkt(0x53, 4, 1, {2});
        RadioPacket pb = mkPkt(0x53, 5, 0, {1});
        vm.onPacket(pa, -40, out);
        vm.onPacket(pb, -40, out);
        runFor(vm, 2000, out);
        CHECK(countAnim(out, TotemUIEvent::ControlContest) == 1, "contest anim");
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1, "owner held while contested");
    }

    // ================= malformed programs =================
    {
        printf("robustness:\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_cp.bin");
        CHECK(!vm.load(prog.data(), prog.size() - 1), "truncated rejected");
        std::vector<uint8_t> junk(prog.begin(), prog.end());
        junk[0] = 99;
        CHECK(!vm.load(junk.data(), junk.size()), "bad version rejected");
        junk = prog;
        junk[3] = 200;   // clobber an opcode area
        bool ok = vm.load(junk.data(), junk.size());
        CHECK(!ok, "corrupt body rejected");
    }

    printf(failures == 0 ? "\nTOTEMVM HOST TESTS PASS\n" : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
