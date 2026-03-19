#pragma once
#include <Arduino.h>
#include "LightAir_RadioTransport.h"
#include "../config.h"

// ----------------------------------------------------------------
// Compile-time structural constants
// These size arrays inside RadioPacket / LightAir_Radio and must
// be identical on every device in a session.  They cannot come from
// NVS because C++ needs them at compile time.
// ----------------------------------------------------------------
#define RADIO_MAX_PAYLOAD  235  // max bytes in payload[] (250 ESP-NOW limit − 15 header)
#define RADIO_MAX_PENDING  10   // max sent msgs awaiting reply
#define RADIO_DEDUP_WINDOW 16   // flood-relay dedup history depth

// ----------------------------------------------------------------
// Packet field sentinels
//
// RadioToken::UNSET (0x00) is the token value before a session starts.
//   A device with _sessionToken == UNSET accepts all packets.
//   Once setSessionToken() is called with a non-zero value, only
//   packets with a matching token are accepted.
//
// RadioTypeId::UNIVERSAL (0x00000000) is the typeId for infrastructure
//   packets that must be accepted regardless of the active game
//   (e.g. MSG_TOTEM_BEACON, MSG_ROSTER).
//   Game-specific packets carry the game's typeId; receivers with a
//   different active typeId silently drop them.
// ----------------------------------------------------------------
namespace RadioToken {
    constexpr uint8_t UNSET = 0x00;  // token not yet assigned; accepts all
}
namespace RadioTypeId {
    constexpr uint32_t UNIVERSAL = 0x00000000;  // accepted by any device regardless of game
}


// ----------------------------------------------------------------
// Wire packet  (packed, ≤ 250 bytes — ESP-NOW hard limit)
//
// MAC convention: 1A:17:A1:00:00:[playerId]
//   senderId carries the logical player ID (= mycolor).
//   Broadcast uses FF:FF:FF:FF:FF:FF; logical ID 0xFF.
//
// Reply convention:
//   reply.msgType = request.msgType + 1  (always odd)
//   reply.timestamp echoes the original request's timestamp for matching.
//
// Flood relay:
//   If resend > 0, any receiver re-broadcasts with resend-1.
//   (senderId, timestamp) dedup suppresses relay loops.
// ----------------------------------------------------------------
struct __attribute__((packed)) RadioPacket {
    uint8_t  senderId;       // logical player ID (= mycolor)
    uint8_t  role;
    uint8_t  team;
    uint8_t  msgType;        // even = request, odd = reply
    uint8_t  sessionToken;   // packets with non-matching token are silently dropped
    uint32_t typeId;         // game type; RadioTypeId::UNIVERSAL (0) = accepted always
    uint32_t timestamp;      // millis() at send time; echoed unchanged in replies
    uint8_t  resend;         // flood hop count; receiver re-broadcasts with resend-1
    uint8_t  payloadLen;     // number of valid bytes in payload[]
    uint8_t  payload[RADIO_MAX_PAYLOAD];
};
// Wire size for a given packet: offsetof(RadioPacket, payload) + payloadLen
// Header size: 15 bytes (250 ESP-NOW limit − 15 = 235 payload bytes)

// ----------------------------------------------------------------
// Events returned by poll()
// ----------------------------------------------------------------
enum class RadioEventType : uint8_t {
    None,
    ReplyReceived,    // a reply to one of our sent requests arrived
    Timeout,          // a sent request timed out without a reply
    MessageReceived,  // an incoming request from another node
};

struct RadioEvent {
    RadioEventType type;
    RadioPacket    packet;    // received packet   (ReplyReceived / MessageReceived)
    RadioPacket    original;  // our sent packet   (ReplyReceived / Timeout)
    int8_t         rssi;      // signal strength dBm (receiver-side metadata; never transmitted)
};

// ----------------------------------------------------------------
// RadioReport — full snapshot returned by poll().
// Contains all events that occurred since the previous poll() call.
// The reference returned by poll() is valid until the next poll().
// ----------------------------------------------------------------
struct RadioReport {
    RadioEvent events[RADIO_MAX_PENDING];
    uint8_t    count = 0;
};

// ----------------------------------------------------------------
// LightAir_Radio  — platform-agnostic managing layer
//
// Depends only on LightAir_RadioTransport; no ESP-NOW symbols here.
// Inject the concrete transport at construction time:
//
//   LightAir_RadioESPNow transport;
//   LightAir_Radio       radio(transport, mycolor, token, role, team);
//   radio.begin();
// ----------------------------------------------------------------
class LightAir_Radio {
public:
    // transport   : physical layer implementation (e.g. LightAir_RadioESPNow)
    // playerId    : mycolor; used to derive own MAC (1A:17:A1:00:00:[playerId])
    // sessionToken: packets with a different token are silently dropped
    // cfg         : runtime-tuneable params (replyTimeoutMs, espNowChannel)
    LightAir_Radio(LightAir_RadioTransport& transport,
                   uint8_t playerId, uint8_t sessionToken,
                   uint8_t role, uint8_t team,
                   const RadioConfig& cfg = RadioConfig{});

    // Update active game typeId (call at game start / totem activation / reset).
    // Packets whose typeId != 0 and != _typeId are silently dropped.
    // Set to RadioTypeId::UNIVERSAL (0) to accept all game types (e.g. before game starts).
    void setTypeId(uint32_t typeId) { _typeId = typeId; }

    // Compute own MAC, call transport.begin(), add broadcast peer.
    bool begin();

    // Send a targeted packet to another player.
    // Even msgType: stored in pending queue awaiting reply.
    // payloadLen must be ≤ RADIO_MAX_PAYLOAD.
    bool sendTo(uint8_t targetId, uint8_t msgType,
                const uint8_t* payload = nullptr, uint8_t payloadLen = 0,
                uint8_t resend = 0);

    // Broadcast to all peers.  resend > 0 triggers mesh relay.
    bool broadcast(uint8_t msgType,
                   const uint8_t* payload = nullptr, uint8_t payloadLen = 0,
                   uint8_t resend = 1);

    // Broadcast with typeId forced to UNIVERSAL (0) regardless of active _typeId.
    // Use for infrastructure messages that must be received by all devices
    // (e.g. MSG_TOTEM_BEACON, MSG_ROSTER) even after a game typeId is set.
    bool broadcastUniversal(uint8_t msgType,
                            const uint8_t* payload = nullptr, uint8_t payloadLen = 0,
                            uint8_t resend = 1);

    // Send a reply to a received packet.
    //   Sets msgType = original.msgType + 1.
    //   Echoes the original timestamp so the sender can match it.
    //   Replies are never stored in the pending queue.
    bool reply(const RadioPacket& original,
               const uint8_t* payload = nullptr, uint8_t payloadLen = 0);

    // Convenience overload: reply using only the three fields needed.
    // Use this from GameRunner::flushOutput() to avoid storing full packets.
    bool replyTo(uint8_t senderId, uint8_t origMsgType, uint32_t origTimestamp,
                 const uint8_t* payload = nullptr, uint8_t payloadLen = 0);

    // Logical player ID (set at construction from NVS; last byte of own MAC).
    uint8_t playerId() const { return _playerId; }

    // Update session token (call at each game start / reset).
    void setSessionToken(uint8_t token) { _sessionToken = token; }

    // Active game typeId (read-back, e.g. for the totem driver).
    uint32_t typeId() const { return _typeId; }

    // Poll — call once per loop iteration.
    // Drains all received packets and checks all pending timeouts in one call.
    // Returns a reference valid until the next call to poll().
    const RadioReport& poll();

private:
    LightAir_RadioTransport& _transport;
    uint8_t     _playerId;
    uint8_t     _sessionToken;
    uint32_t    _typeId;    // active game typeId; 0 = universal (accept all)
    uint8_t     _role;
    uint8_t     _team;
    RadioConfig _config;

    // ---- pending sent messages awaiting replies ----
    struct PendingMsg {
        RadioPacket pkt;
        uint32_t    sentAt;
        bool        active;
    };
    PendingMsg _pending[RADIO_MAX_PENDING];

    // ---- deduplication ring for flood relay ----
    struct DedupEntry { uint8_t senderId; uint32_t timestamp; bool valid; };
    DedupEntry _dedup[RADIO_DEDUP_WINDOW];
    uint8_t    _dedupIdx;

    RadioReport _report;

    // ---- helpers ----
    void buildMac(uint8_t playerId, uint8_t mac[6]) const;
    bool sendRaw(const uint8_t mac[6], const RadioPacket& pkt);
    bool storePending(const RadioPacket& pkt);
    int  findPending(uint8_t replyMsgType, uint32_t timestamp) const;
    bool isDuplicate(uint8_t senderId, uint32_t timestamp) const;
    void recordDedup(uint8_t senderId, uint32_t timestamp);
    void processPacket(const RadioPacket& pkt, int8_t rssi);  // classify and add to _report
};
