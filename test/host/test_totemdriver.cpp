// Host test: what the totem driver handles itself, outside the program —
// the touch service (MSG_TOTEM_TOUCH, config.h).  Real driver, real radio
// (test transport), real TotemVM fed a reference-encoder program; the LED
// hardware is a recording fake.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ArduinoLog.h"
uint32_t g_millis = 1000;
HostLog Log;

#include "totem/LightAir_TotemDriver.h"
#include "totem/TotemRoleIds.h"
#include "radio/LightAir_RadioTestTransport.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

struct FakeRGB : LightAir_TotemRGB {
    void set(uint8_t, uint8_t, uint8_t) override {}
    void off() override {}
};
// Records one-shots; `pending` stands for what the strip still has to show.
struct FakeStrip : LightAir_LEDStrip {
    std::vector<StripAnimation> played;
    int pending = 0;
    void play(const StripAnimation& a) override { played.push_back(a); pending++; }
    void loop(const StripAnimation&) override {}
    void stopLoop() override {}
    void update() override {}
    bool busy() const override { return pending > 0; }
};

static std::vector<uint8_t> readFile(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { printf("cannot open %s\n", p.c_str()); exit(1); }
    std::vector<uint8_t> v;
    int c;
    while ((c = fgetc(f)) != EOF) v.push_back((uint8_t)c);
    fclose(f);
    return v;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "build";
    const std::vector<uint8_t> prog = readFile(dir + "/prog_touch.bin");

    const uint8_t TOTEM = 250, HOST = 1, TOKEN = 0x42;
    const uint16_t TYPE = 0x0007;
    LightAir_RadioTestTransport tr;
    LightAir_Radio radio(tr, TOTEM, 0, 0, 0);
    FakeRGB rgb; FakeStrip strip;
    LightAir_TotemUICtrl ui(rgb, strip);
    LightAir_TotemDriver driver(radio, ui);
    driver.begin();

    uint32_t ts = 5000;
    auto touch = [&](uint8_t player, uint8_t gate, uint8_t action, int8_t rssi) {
        RadioPacket p = {};
        p.senderId = player; p.msgType = RadioMsg::MSG_TOTEM_TOUCH;
        p.sessionToken = TOKEN; p.typeId = TYPE; p.timestamp = ts++;
        p.payloadLen = 2; p.payload[0] = gate; p.payload[1] = action;
        tr.testRssi = rssi;
        tr.push(p);
    };
    std::vector<LightAir_RadioTestTransport::SentEntry> sent;
    auto tick = [&]() {
        g_millis += 10;
        driver.loop();
        sent.clear();
        while (tr.hasSent()) sent.push_back(tr.popSent());
    };
    auto replyTo = [&](uint8_t player) -> const RadioPacket* {
        for (const auto& e : sent)
            if (e.pkt.msgType == RadioMsg::MSG_TOTEM_TOUCH + 1 && e.dstMac[5] == player)
                return &e.pkt;
        return nullptr;
    };
    auto probeFired = [&]() {
        for (const auto& e : sent)
            if (e.pkt.msgType == RadioMsg::MSG_BASE_BEACON) return true;
        return false;
    };

    // ---- Unassigned: a totem in no game answers no touch ----
    touch(3, 0, TotemTouch::ACK, -40);
    tick();
    CHECK(!replyTo(3) && strip.played.empty(), "an idle totem ignores a touch");

    // ---- Activate as a BONUS running the probe program ----
    // The activation is the host's reply to this totem's own beacon, so it
    // must echo that beacon's timestamp.
    {
        uint32_t beaconTs = 0;
        bool beaconed = false;
        for (int i = 0; i < 500 && !beaconed; i++) {
            tick();
            for (const auto& e : sent)
                if (e.pkt.msgType == RadioMsg::MSG_TOTEM_BEACON) {
                    beaconed = true; beaconTs = e.pkt.timestamp;
                }
        }
        CHECK(beaconed, "the idle totem beacons");
        RadioPacket a = {};
        a.senderId = HOST; a.msgType = RadioMsg::MSG_TOTEM_BEACON + 1;
        a.sessionToken = TOKEN; a.typeId = TYPE; a.timestamp = beaconTs;
        a.payload[0] = TotemRoleId::BONUS; a.payload[1] = TOKEN;
        a.payload[2] = 0x03; a.payload[3] = 0x84;          // 900 s left
        a.payload[4] = TotemVMDefs::VERSION;
        a.payload[5] = (uint8_t)(prog.size() & 0xFF);
        a.payload[6] = (uint8_t)(prog.size() >> 8);
        memcpy(a.payload + 7, prog.data(), prog.size());
        a.payloadLen = (uint8_t)(7 + prog.size());
        tr.testRssi = -40;
        tr.push(a);
        tick();
    }
    strip.played.clear(); strip.pending = 0;

    // ---- ACK in reach: chaser in the toucher's colour, reply, program untouched ----
    touch(3, 55, TotemTouch::ACK, -50);
    tick();
    const RadioPacket* r = replyTo(3);
    CHECK(r && r->payloadLen == 2 && r->payload[0] == TotemTouch::ACK &&
          r->payload[1] == TotemRoleId::BONUS, "an ACK in reach is answered [ACK, role]");
    CHECK(strip.played.size() == 1 && strip.played[0].effect == StripEffect::Chase &&
          strip.played[0].r == PlayerColors::kColors[3][0] &&
          strip.played[0].g == PlayerColors::kColors[3][1] &&
          strip.played[0].b == PlayerColors::kColors[3][2],
          "an ACK plays the arrival chaser in the toucher's colour");
    CHECK(!probeFired(), "the program never sees a touch");

    // ---- Out of reach: silence ----
    strip.played.clear(); strip.pending = 0;
    touch(4, 55, TotemTouch::ACK, -70);
    tick();
    CHECK(!replyTo(4) && strip.played.empty(), "a touch read below its gate gets nothing");
    touch(4, 0, TotemTouch::ACK, -95);
    tick();
    CHECK(replyTo(4) && strip.played.size() == 1, "gate 0: no distance limit");

    // ---- The chaser yields: never over another one-shot, one per tick ----
    strip.played.clear();                                  // pending stays 1: still playing
    touch(5, 55, TotemTouch::ACK, -40);
    tick();
    CHECK(replyTo(5) && strip.played.empty(),
          "while the strip is busy the touch is answered but not animated");
    strip.pending = 0;
    touch(6, 55, TotemTouch::ACK, -40);
    touch(7, 55, TotemTouch::ACK, -40);
    tick();
    CHECK(replyTo(6) && replyTo(7) && strip.played.size() == 1,
          "two touches in one tick: both answered, one chaser");

    // ---- Reserved actions: the program's, ignored until it can take them ----
    strip.played.clear(); strip.pending = 0;
    touch(8, 55, TotemTouch::FIRST_PROGRAM, -40);
    tick();
    CHECK(!replyTo(8) && strip.played.empty() && !probeFired(),
          "a reserved action gets no reply, no chaser, and does not reach the program");

    // ---- Malformed: too short ----
    {
        RadioPacket p = {};
        p.senderId = 9; p.msgType = RadioMsg::MSG_TOTEM_TOUCH;
        p.sessionToken = TOKEN; p.typeId = TYPE; p.timestamp = ts++;
        p.payloadLen = 1; p.payload[0] = 55;
        tr.push(p);
        tick();
        CHECK(!replyTo(9) && strip.played.empty() && !probeFired(), "a short touch is dropped");
    }

    // ---- Roster: back to idle, touches ignored again ----
    {
        RadioPacket ro = {};
        ro.senderId = HOST; ro.msgType = RadioMsg::MSG_TOTEM_ROSTER;
        ro.timestamp = ts++;
        tr.push(ro);
        tick();
        strip.played.clear(); strip.pending = 0;
        touch(3, 0, TotemTouch::ACK, -40);
        tick();
        CHECK(!replyTo(3) && strip.played.empty(), "after the roster a touch is ignored again");
    }

    printf(failures == 0 ? "TOTEMDRIVER TESTS PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
