#pragma once
#include "LightAir_RadioTransport.h"
#include "LightAir_Radio.h"   // RadioPacket, RADIO_MAX_PAYLOAD
#include <string.h>
#include <stddef.h>

// ----------------------------------------------------------------
// LightAir_RadioTestTransport
//
// In-memory fake transport for unit-testing LightAir_Radio without
// any hardware or ESP-NOW dependency.
//
// Pattern:
//   1. Construct and inject into LightAir_Radio.
//   2. Call transport.push(...)  to simulate incoming packets.
//   3. Call radio.poll()          to run managing-layer logic.
//   4. Call transport.popSent()   to assert what was transmitted.
//
// Example:
//   LightAir_RadioTestTransport tr;
//   LightAir_Radio radio(tr, /*playerId*/1, /*token*/0xAB, 0, 0);
//   radio.begin();
//
//   // Simulate an incoming HIT_NOTIFY from player 2
//   tr.push(/*senderId*/2, 0, 0, RadioMsg::HIT_NOTIFY, 0xAB, 1000);
//
//   RadioEvent e = radio.poll();
//   assert(e.type == RadioEventType::MessageReceived);
//   assert(e.packet.senderId == 2);
//
//   // Verify the radio sent nothing yet (we haven't replied)
//   assert(!tr.hasSent());
// ----------------------------------------------------------------

#define TEST_TRANSPORT_QUEUE 16

class LightAir_RadioTestTransport : public LightAir_RadioTransport {
public:
    // ---- Sent-entry: what LightAir_Radio put on the wire ----
    struct SentEntry {
        uint8_t     dstMac[6];
        RadioPacket pkt;
        size_t      wireLen;   // actual bytes in the wire frame
    };

    // ============================================================
    // LightAir_RadioTransport interface
    // ============================================================
    bool begin(const uint8_t selfMac[6]) override {
        memcpy(_selfMac, selfMac, 6);
        _beginCalled = true;
        return !forceBeginFail;
    }

    bool send(const uint8_t mac[6], const uint8_t* data, size_t len) override {
        if (forceSendFail) return false;
        if (len == 0 || len > sizeof(RadioPacket)) return false;

        // Track unique destination MACs (mirrors ESP-NOW peer registration).
        bool found = false;
        for (int i = 0; i < _peerCount; i++)
            if (memcmp(_peers[i], mac, 6) == 0) { found = true; break; }
        if (!found && _peerCount < MAX_TEST_PEERS)
            memcpy(_peers[_peerCount++], mac, 6);

        int next = (_sentHead + 1) % TEST_TRANSPORT_QUEUE;
        if (next == _sentTail) return false;  // sent queue full — drop

        SentEntry& e = _sentQueue[_sentHead];
        memcpy(e.dstMac, mac, 6);
        memset(&e.pkt, 0, sizeof(e.pkt));
        memcpy(&e.pkt, data, len);
        e.wireLen = len;
        _sentHead = next;
        return true;
    }

    bool receive(uint8_t* data, int& len, int maxLen) override {
        if (_recvHead == _recvTail) return false;
        const RawEntry& r = _recvQueue[_recvTail];
        int copyLen = (r.len < maxLen) ? r.len : maxLen;
        memcpy(data, r.data, copyLen);
        len = copyLen;
        _recvTail = (_recvTail + 1) % TEST_TRANSPORT_QUEUE;
        return true;
    }

    // ============================================================
    // Test helpers — inject incoming packets
    // ============================================================

    // Push a pre-built RadioPacket into the receive queue.
    // wireLen defaults to header + pkt.payloadLen (the normal wire size).
    bool push(const RadioPacket& pkt, int wireLen = -1) {
        int next = (_recvHead + 1) % TEST_TRANSPORT_QUEUE;
        if (next == _recvTail) return false;  // queue full
        RawEntry& e = _recvQueue[_recvHead];
        int sz = (wireLen < 0)
                   ? (int)(offsetof(RadioPacket, payload) + pkt.payloadLen)
                   : wireLen;
        memset(e.data, 0, sizeof(e.data));
        memcpy(e.data, &pkt, sz);
        e.len = sz;
        _recvHead = next;
        return true;
    }

    // Build and push a RadioPacket from individual fields.
    // This is the easy-pack helper for common test scenarios.
    bool push(uint8_t senderId,
              uint8_t role,
              uint8_t team,
              uint8_t msgType,
              uint8_t sessionToken,
              uint32_t timestamp,
              uint8_t  resend      = 0,
              const uint8_t* payload  = nullptr,
              uint8_t  payloadLen  = 0) {
        RadioPacket pkt = {};
        pkt.senderId     = senderId;
        pkt.role         = role;
        pkt.team         = team;
        pkt.msgType      = msgType;
        pkt.sessionToken = sessionToken;
        pkt.timestamp    = timestamp;
        pkt.resend       = resend;
        pkt.payloadLen   = payloadLen;
        if (payload && payloadLen)
            memcpy(pkt.payload, payload, payloadLen);
        return push(pkt);
    }

    // ============================================================
    // Test helpers — inspect sent packets
    // ============================================================

    // Number of packets waiting in the sent queue.
    int sentCount() const {
        return (_sentHead - _sentTail + TEST_TRANSPORT_QUEUE) % TEST_TRANSPORT_QUEUE;
    }
    bool hasSent() const { return sentCount() > 0; }

    // Remove and return the oldest sent entry.
    // Returns a zeroed entry (wireLen == 0) when the queue is empty.
    SentEntry popSent() {
        SentEntry empty = {};
        if (_sentHead == _sentTail) return empty;
        SentEntry e = _sentQueue[_sentTail];
        _sentTail = (_sentTail + 1) % TEST_TRANSPORT_QUEUE;
        return e;
    }

    // Peek at a sent entry without removing it (index 0 = oldest).
    // Returns a zeroed entry if index is out of range.
    SentEntry peekSent(int index = 0) const {
        if (index >= sentCount()) return {};
        return _sentQueue[(_sentTail + index) % TEST_TRANSPORT_QUEUE];
    }

    // Discard all sent entries.
    void clearSent() { _sentHead = _sentTail = 0; }

    // ============================================================
    // Test helpers — state inspection
    // ============================================================
    bool wasBeginCalled() const { return _beginCalled; }
    void resetBeginCalled()     { _beginCalled = false; }

    // Own MAC address set via begin().
    void getSelfMac(uint8_t out[6]) const { memcpy(out, _selfMac, 6); }

    // Number of registered peers.
    int  peerCount() const { return _peerCount; }

    // Check whether a specific MAC was registered as a peer.
    bool hasPeer(const uint8_t mac[6]) const {
        for (int i = 0; i < _peerCount; i++)
            if (memcmp(_peers[i], mac, 6) == 0) return true;
        return false;
    }

    // ============================================================
    // Fault-injection flags
    // ============================================================
    bool forceBeginFail = false;
    bool forceSendFail  = false;

private:
    bool    _beginCalled = false;
    uint8_t _selfMac[6]  = {};

    struct RawEntry { uint8_t data[sizeof(RadioPacket)]; int len; };
    RawEntry _recvQueue[TEST_TRANSPORT_QUEUE] = {};
    int      _recvHead = 0;
    int      _recvTail = 0;

    SentEntry _sentQueue[TEST_TRANSPORT_QUEUE] = {};
    int       _sentHead = 0;
    int       _sentTail = 0;

    static constexpr int MAX_TEST_PEERS = 20;
    uint8_t _peers[MAX_TEST_PEERS][6] = {};
    int     _peerCount = 0;
};
