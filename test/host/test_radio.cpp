// Host test for LightAir_Radio's reply bookkeeping.
//
// The interesting asymmetry is between the two ways a request goes out:
// a unicast has exactly one expected responder, while a broadcast — every
// totem beacon — is answered by every device in range.  Closing a
// broadcast's reply window on the first answer swallows all the rest,
// which is how a respawning player's BASE_BEACON reply used to get lost
// behind the empty auto-replies of everyone still in the game.
#include <cstdio>
#include <cstring>

#include "Arduino.h"
#include "ArduinoLog.h"
uint32_t g_millis = 1000;
HostLog Log;

#include "radio/LightAir_RadioTestTransport.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

// Count events of one type in a report.
static int countType(const RadioReport& r, RadioEventType t) {
    int n = 0;
    for (uint8_t i = 0; i < r.count; i++)
        if (r.events[i].type == t) n++;
    return n;
}

// The timestamp LightAir_Radio stamped on the packet it just sent, which a
// reply has to echo back for the two to be matched.
static uint32_t lastSentTimestamp(LightAir_RadioTestTransport& tr) {
    return tr.peekSent(tr.sentCount() - 1).pkt.timestamp;
}

// Push a reply to a request `sender` never saw: msgType+1, echoed timestamp.
static void pushReply(LightAir_RadioTestTransport& tr, uint8_t sender,
                      uint8_t reqMsgType, uint32_t timestamp,
                      uint8_t subType) {
    tr.push(sender, /*role*/ 0, /*team*/ 0, (uint8_t)(reqMsgType + 1),
            /*token*/ 0, timestamp, /*resend*/ 0, &subType, 1);
}

int main() {
    // ---- 1. Every reply to a broadcast is delivered ----
    {
        printf("broadcast fan-in:\n");
        LightAir_RadioTestTransport tr;
        LightAir_Radio radio(tr, /*playerId*/ 254, /*token*/ 0, 0, 0);
        radio.begin();
        radio.poll();                                  // drain the join/boot state

        radio.broadcast(RadioMsg::MSG_BASE_BEACON);
        uint32_t ts = lastSentTimestamp(tr);

        // Three players answer the same beacon; only the third carries a
        // respawn sub-type, the other two are the empty auto-replies.
        pushReply(tr, 3, RadioMsg::MSG_BASE_BEACON, ts, 0);
        pushReply(tr, 4, RadioMsg::MSG_BASE_BEACON, ts, 0);
        pushReply(tr, 5, RadioMsg::MSG_BASE_BEACON, ts, 2);

        const RadioReport& rep = radio.poll();
        CHECK(countType(rep, RadioEventType::ReplyReceived) == 3,
              "all three replies to one broadcast delivered");
        bool sawRespawn = false;
        for (uint8_t i = 0; i < rep.count; i++)
            if (rep.events[i].type == RadioEventType::ReplyReceived &&
                rep.events[i].packet.senderId == 5 &&
                rep.events[i].packet.payloadLen == 1 &&
                rep.events[i].packet.payload[0] == 2)
                sawRespawn = true;
        CHECK(sawRespawn, "the respawn reply survives the empty ones ahead of it");
    }

    // ---- 2. A broadcast raises no Timeout when nobody answers ----
    {
        printf("broadcast timeout:\n");
        LightAir_RadioTestTransport tr;
        LightAir_Radio radio(tr, /*playerId*/ 254, /*token*/ 0, 0, 0);
        radio.begin();
        radio.poll();

        radio.broadcast(RadioMsg::MSG_BASE_BEACON);
        g_millis += 5000;                              // well past replyTimeoutMs
        const RadioReport& rep = radio.poll();
        CHECK(countType(rep, RadioEventType::Timeout) == 0,
              "unanswered broadcast is not a timeout");

        // The window did close, though: a late reply finds no pending entry.
        pushReply(tr, 3, RadioMsg::MSG_BASE_BEACON, lastSentTimestamp(tr), 1);
        const RadioReport& late = radio.poll();
        CHECK(countType(late, RadioEventType::ReplyReceived) == 0,
              "reply after the window closed is dropped");
    }

    // ---- 3. A unicast still expects exactly one answer ----
    {
        printf("unicast:\n");
        LightAir_RadioTestTransport tr;
        LightAir_Radio radio(tr, /*playerId*/ 2, /*token*/ 0, 0, 0);
        radio.begin();
        radio.poll();

        radio.sendTo(/*targetId*/ 3, RadioMsg::MSG_LIT);
        uint32_t ts = lastSentTimestamp(tr);

        pushReply(tr, 3, RadioMsg::MSG_LIT, ts, 1);
        const RadioReport& first = radio.poll();
        CHECK(countType(first, RadioEventType::ReplyReceived) == 1,
              "the answer is delivered");

        // A second reply echoing the same request has nothing left to match.
        pushReply(tr, 4, RadioMsg::MSG_LIT, ts, 1);
        const RadioReport& second = radio.poll();
        CHECK(countType(second, RadioEventType::ReplyReceived) == 0,
              "unicast slot released by the first answer");
    }

    // ---- 4. An unanswered unicast still times out ----
    {
        printf("unicast timeout:\n");
        LightAir_RadioTestTransport tr;
        LightAir_Radio radio(tr, /*playerId*/ 2, /*token*/ 0, 0, 0);
        radio.begin();
        radio.poll();

        radio.sendTo(/*targetId*/ 3, RadioMsg::MSG_LIT);
        g_millis += 5000;
        const RadioReport& rep = radio.poll();
        CHECK(countType(rep, RadioEventType::Timeout) == 1,
              "unanswered unicast raises a timeout");
        CHECK(rep.count == 0 || rep.events[0].rssi == 0,
              "timeout carries no measured signal strength");
    }

    printf("\n%s\n", failures ? "RADIO TESTS FAILED" : "RADIO TESTS PASS");
    return failures ? 1 : 0;
}
