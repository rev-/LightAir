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

    // ========= RSSI value operand + signed registers =========
    {
        printf("rssi/registers:\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_rssi.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load rssi probe");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);

        // Latch a far reading into R0 and a near one into R1, then ask which
        // was closer.  Nothing here is representable in a uint8_t register.
        RadioPacket toR0 = mkPkt(0x56, 3, 0, {1});
        RadioPacket toR1 = mkPkt(0x56, 4, 0, {2});
        RadioPacket ask  = mkPkt(0x56, 5, 0, {3});

        out = LightAir_TotemOutput();
        vm.onPacket(toR0, -80, out);          // R0 = -80 (far)
        vm.onPacket(toR1, -40, out);          // R1 = -40 (near)
        vm.onPacket(ask,  -50, out);
        CHECK(countAnim(out, TotemUIEvent::Bonus) == 1,
              "stronger RSSI in R1 compares greater than R0");

        // Reverse it: the near reading now lands in R0, so R1 > R0 is false.
        out = LightAir_TotemOutput();
        vm.onActivate(kAct, out);             // re-enter: R0 = R1 = 0
        out = LightAir_TotemOutput();
        vm.onPacket(toR0, -40, out);          // R0 = -40 (near)
        vm.onPacket(toR1, -80, out);          // R1 = -80 (far)
        vm.onPacket(ask,  -50, out);
        CHECK(countAnim(out, TotemUIEvent::Bonus) == 0,
              "weaker RSSI in R1 does not compare greater");

        // The part 8-bit registers cannot do: keep the sign.  Truncated to
        // uint8_t, -80 reads as 176 and this rule never fires.
        RadioPacket isNeg = mkPkt(0x56, 6, 0, {4});
        out = LightAir_TotemOutput();
        vm.onPacket(toR0, -80, out);
        vm.onPacket(isNeg, -50, out);
        CHECK(countAnim(out, TotemUIEvent::Malus) == 1,
              "a stored RSSI keeps its sign in an int16 register");
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
    // Conquest/hold split: a "conquest" reply (sub-type 1..16, the
    // replying player's own slot) feeds the totem's ACC and drives
    // capture/contest; a "hold" reply (the reserved sub-type 17) never
    // touches ACC and only proves the recorded owner is still around,
    // via R2, which the hold-sustain rule reads.  See std.totems.cp()
    // for why 17 rather than 0 (RadioOutput::reply() treats subType==0
    // as "no payload at all", so 0 cannot carry a real value here).
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

        // One team-1 player present (sub-type 2, "conquest") -> owner =
        // slot 1, and the capture pays a point at once: waiting a whole
        // emission period before anything happens reads as "nothing
        // happened".
        out = LightAir_TotemOutput();
        RadioPacket pres = mkPkt(0x53, 4, 1, {2});
        vm.onPacket(pres, -40, out);
        runFor(vm, 2000, out);
        const TotemUICmd* c = lastAnim(out, TotemUIEvent::Control);
        CHECK(c && c->r == 0xFE && c->g == 1, "control anim slot form");
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1, "owner beacon slot 1");
        const RadioOutMsg* cap = lastBcast(out, 0x54);
        CHECK(countBcast(out, 0x54) == 1, "capture scores immediately");
        CHECK(cap && cap->payload[0] == 1, "capture point goes to the new owner");

        // Held alone from there via "hold" replies (sub-type 17, not
        // conquest -- a real owner never sends conquest, see
        // cp_beacon_handler): the period runs from the capture, so the
        // next point lands 10 s later and no sooner.
        int scores = 0, scoreSlot = -1, firstScoreWindow = -1;
        for (int w = 0; w < 6; w++) {
            out = LightAir_TotemOutput();
            RadioPacket hold = mkPkt(0x53, 4, 1, {17});
            vm.onPacket(hold, -40, out);
            runFor(vm, 2000, out);
            if (countBcast(out, 0x54)) {
                if (firstScoreWindow < 0) firstScoreWindow = w;
                scores += countBcast(out, 0x54);
                scoreSlot = lastBcast(out, 0x54)->payload[0];
            }
        }
        CHECK(scores == 1, "cp score broadcast exactly once in the 12s after");
        CHECK(firstScoreWindow == 4, "one emission period after the capture");
        CHECK(scoreSlot == 1, "score for slot 1");

        // Two distinct conquest senders, NEITHER the recorded owner
        // (slot 1 is still holding elsewhere but sends no conquest this
        // window): a real contest -- no ownership change, no point.
        out = LightAir_TotemOutput();
        RadioPacket riv1 = mkPkt(0x53, 5, 0, {1});   // slot 0
        RadioPacket riv2 = mkPkt(0x53, 6, 0, {3});   // slot 2
        vm.onPacket(riv1, -40, out);
        vm.onPacket(riv2, -40, out);
        runFor(vm, 2000, out);
        CHECK(countAnim(out, TotemUIEvent::ControlContest) == 1, "contest anim");
        CHECK(countBcast(out, 0x54) == 0, "contested window pays no point");
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1, "owner beacon unchanged while contested");

        // Easy stealing: exactly one conquest sender takes the hill on
        // the spot, even in the SAME window the recorded owner sends
        // hold -- an owner merely holding never blocks a lone challenger,
        // however close they are (see cp_beacon_handler's own doc: only
        // combat, not proximity, defends a held hill).
        out = LightAir_TotemOutput();
        RadioPacket ownerHold = mkPkt(0x53, 4, 1, {17});  // slot 1, holding
        RadioPacket steal     = mkPkt(0x53, 6, 0, {3});   // slot 2, conquest
        vm.onPacket(ownerHold, -40, out);
        vm.onPacket(steal, -40, out);
        runFor(vm, 2000, out);
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 2, "a lone challenger steals it outright");
        CHECK(countBcast(out, 0x54) == 1, "and the steal pays its point");

        // A contest that resolves to a single conqueror who ISN'T the
        // recorded owner: an ordinary capture, same as the very first one.
        out = LightAir_TotemOutput();
        RadioPacket c1 = mkPkt(0x53, 5, 0, {1});
        RadioPacket c2 = mkPkt(0x53, 4, 1, {2});
        vm.onPacket(c1, -40, out);
        vm.onPacket(c2, -40, out);
        runFor(vm, 2000, out);                        // contest
        out = LightAir_TotemOutput();
        RadioPacket alone = mkPkt(0x53, 5, 0, {1});   // only slot 0 left
        vm.onPacket(alone, -40, out);
        runFor(vm, 2000, out);
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 0, "contest resolved to the last conqueror present");
        CHECK(countBcast(out, 0x54) == 1, "and the takeover pays its point");

        // Fully abandoned right after a contest -- no conqueror AND no
        // hold, the owner's own device having gone quiet too (shone, or
        // walked off).  Must release on the very next empty window: with
        // the dedicated "settle" rule gone, nothing else is guaranteed to
        // clear a leftover contest flag, so release must not wait on it
        // (see std.totems.cp()'s release rule).
        out = LightAir_TotemOutput();
        RadioPacket x1 = mkPkt(0x53, 5, 0, {1});
        RadioPacket x2 = mkPkt(0x53, 4, 1, {2});
        vm.onPacket(x1, -40, out);
        vm.onPacket(x2, -40, out);
        runFor(vm, 2000, out);                        // contest, R1 set
        out = LightAir_TotemOutput();
        runFor(vm, 2000, out);                        // nobody at all, incl. no hold
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 0xFF,
              "a contest that goes fully silent releases on the very next window");
        CHECK(countAnim(out, TotemUIEvent::CPIdle) == 1, "and the ring returns to idle");
    }

    // ================= CP release (owner absent) =================
    // Reported from the field: an owner who was SHONE, or simply walked
    // away, stayed painted on the ring and kept being announced as owner
    // forever — nothing in the program ever cleared R0 on its own, since
    // every existing rule needs either a new single occupant or an active
    // contest to react to.  proximity gating is entirely player-side (the
    // totem's accbit rule has no RSSI guard at all), so the totem cannot
    // tell "shone" from "walked off" — both just stop sending presence,
    // which is exactly what this exercises.
    {
        printf("CP release:\n");
        LightAir_TotemVM vm;
        auto prog = readFile(dir + "/prog_cp.bin");
        CHECK(vm.load(prog.data(), prog.size()), "load cp");
        LightAir_TotemOutput out;
        vm.onActivate(kAct, out);

        // Slot 0 captures it alone, uncontested (r1 never set).
        out = LightAir_TotemOutput();
        RadioPacket owner = mkPkt(0x53, 5, 0, {1});
        vm.onPacket(owner, -40, out);
        runFor(vm, 2000, out);
        const RadioOutMsg* b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 0, "captured by slot 0");

        // Silence for one window: the owner stopped answering (shone, or
        // simply gone) and nobody else is present either.
        out = LightAir_TotemOutput();
        runFor(vm, 2000, out);
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 0xFF,
              "an absent, uncontested owner is released on the next empty window");
        CHECK(countAnim(out, TotemUIEvent::CPIdle) == 1,
              "and the ring returns to idle");

        // A fresh occupant still has to capture it properly (single,
        // different from the now-neutral owner) -- release does not skip
        // the normal capture path.
        out = LightAir_TotemOutput();
        RadioPacket newcomer = mkPkt(0x53, 4, 1, {2});
        vm.onPacket(newcomer, -40, out);
        runFor(vm, 2000, out);
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1, "released hill still captures normally");

        // A held hill survives being contested by others without
        // releasing, even though nothing now clears the leftover contest
        // flag on its own (the dedicated "settle" rule is gone): release
        // is gated on R2==0 (no hold reply this window), so it simply
        // never fires while the owner keeps holding, contest or not.
        // Owner is slot 1 (the newcomer captured above); two OTHER
        // players (slots 0 and 2) contest it while slot 1 keeps holding.
        out = LightAir_TotemOutput();
        RadioPacket contestA = mkPkt(0x53, 6, 0, {3});   // slot 2, conquest
        RadioPacket contestB = mkPkt(0x53, 5, 0, {1});   // slot 0, conquest
        RadioPacket ownHold  = mkPkt(0x53, 4, 1, {17});  // slot 1, hold
        vm.onPacket(contestA, -40, out);
        vm.onPacket(contestB, -40, out);
        vm.onPacket(ownHold, -40, out);
        runFor(vm, 2000, out);
        CHECK(countAnim(out, TotemUIEvent::ControlContest) == 1,
              "a held hill can still be contested by others");

        out = LightAir_TotemOutput();
        RadioPacket stillHold = mkPkt(0x53, 4, 1, {17});
        vm.onPacket(stillHold, -40, out);
        runFor(vm, 2000, out);
        b = lastBcast(out, 0x52);
        CHECK(b && b->payload[0] == 1,
              "the owner is not released just because a contest flag was left set");

        // Keep holding until the score period elapses: the hold-sustain
        // rule fires normally once due, clearing the leftover contest
        // flag as a side effect (see std.totems.cp()'s R1 comment) --
        // the earlier contest cost no points and delayed nothing.
        int scores2 = 0;
        for (int w = 0; w < 4; w++) {
            out = LightAir_TotemOutput();
            RadioPacket h = mkPkt(0x53, 4, 1, {17});
            vm.onPacket(h, -40, out);
            runFor(vm, 2000, out);
            scores2 += countBcast(out, 0x54);
        }
        CHECK(scores2 == 1, "holding through a contest still scores once the period elapses");
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
