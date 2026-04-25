#pragma once
#include <stdint.h>
#include <string.h>
#include "../config.h"
#include "../radio/LightAir_Radio.h"

// ----------------------------------------------------------------
// RadioOutput — outgoing message queue for the game loop.
//
// Game rules and behaviors write to RadioOutput during step 2
// (logic phase).  GameRunner flushes it to LightAir_Radio in
// step 3 (output phase) after all logic is complete.
//
// This separates game logic from I/O: the game never calls
// LightAir_Radio directly during rule evaluation; all outgoing
// messages are collected and sent atomically at the end of each
// loop iteration.
//
// Payload is capped at GameDefaults::RADIO_OUT_PAYLOAD bytes,
// which equals RADIO_MAX_PAYLOAD (239) — the full ESP-NOW limit.
// ----------------------------------------------------------------

struct RadioOutMsg {
    bool    isBroadcast;
    uint8_t targetId;
    uint8_t msgType;
    uint8_t resend;
    uint8_t payload[GameDefaults::RADIO_OUT_PAYLOAD];
    uint8_t payloadLen;
};

// Minimal fields needed to send a reply (avoids copying the full RadioPacket).
struct RadioReplyMsg {
    uint8_t  senderId;
    uint8_t  origMsgType;
    uint32_t origTimestamp;
    uint8_t  payload[16];   // reply payload bytes (16 covers max totem activation payload)
    uint8_t  payloadLen;    // 0 = no payload
};

struct RadioOutput {
    RadioOutMsg   msgs[GameDefaults::RADIO_OUT_MAX];
    uint8_t       count = 0;
    RadioReplyMsg replies[GameDefaults::RADIO_REPLY_MAX];
    uint8_t       replyCount = 0;

    // Queue a broadcast.  resend > 0 enables mesh relay.
    void broadcast(uint8_t msgType,
                   const uint8_t* payload = nullptr, uint8_t len = 0,
                   uint8_t resend = 1) {
        if (count >= GameDefaults::RADIO_OUT_MAX) return;
        if (len > GameDefaults::RADIO_OUT_PAYLOAD) return;  // exceeds physical limit
        RadioOutMsg& m = msgs[count++];
        m.isBroadcast = true;
        m.targetId    = 0xFF;
        m.msgType     = msgType;
        m.resend      = resend;
        m.payloadLen  = len;
        if (len && payload) memcpy(m.payload, payload, len);
    }

    // Queue a unicast to a specific player ID.
    void sendTo(uint8_t targetId, uint8_t msgType,
                const uint8_t* payload = nullptr, uint8_t len = 0,
                uint8_t resend = 0) {
        if (count >= GameDefaults::RADIO_OUT_MAX) return;
        if (len > GameDefaults::RADIO_OUT_PAYLOAD) return;  // exceeds physical limit
        RadioOutMsg& m = msgs[count++];
        m.isBroadcast = false;
        m.targetId    = targetId;
        m.msgType     = msgType;
        m.resend      = resend;
        m.payloadLen  = len;
        if (len && payload) memcpy(m.payload, payload, len);
    }

    // Queue a reply to a received packet.
    // msgType will be set to original.msgType + 1; timestamp is echoed.
    // subType != 0 places that value in payload[0] of the reply,
    // letting the sender distinguish different reply semantics.
    void reply(const RadioPacket& original, uint8_t subType = 0) {
        if (replyCount >= GameDefaults::RADIO_REPLY_MAX) return;
        RadioReplyMsg& r   = replies[replyCount++];
        r.senderId         = original.senderId;
        r.origMsgType      = original.msgType;
        r.origTimestamp    = original.timestamp;
        if (subType) {
            r.payload[0] = subType;
            r.payloadLen = 1;
        } else {
            r.payloadLen = 0;
        }
    }

    // Queue a reply with an explicit multi-byte payload (up to 16 bytes).
    void replyWithPayload(const RadioPacket& original,
                          const uint8_t* pl, uint8_t len) {
        if (replyCount >= GameDefaults::RADIO_REPLY_MAX) return;
        if (len > 16) len = 16;
        RadioReplyMsg& r   = replies[replyCount++];
        r.senderId         = original.senderId;
        r.origMsgType      = original.msgType;
        r.origTimestamp    = original.timestamp;
        r.payloadLen       = len;
        if (len && pl) memcpy(r.payload, pl, len);
    }
};
