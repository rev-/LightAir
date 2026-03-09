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
// Payload is capped at GameDefaults::RADIO_OUT_PAYLOAD (32 bytes).
// Most game messages carry 0–8 bytes; use LightAir_Radio::broadcast
// directly if you need a larger payload (rare in game logic).
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
        if (len > GameDefaults::RADIO_OUT_PAYLOAD) len = GameDefaults::RADIO_OUT_PAYLOAD;
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
        if (len > GameDefaults::RADIO_OUT_PAYLOAD) len = GameDefaults::RADIO_OUT_PAYLOAD;
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
    void reply(const RadioPacket& original) {
        if (replyCount >= GameDefaults::RADIO_REPLY_MAX) return;
        RadioReplyMsg& r   = replies[replyCount++];
        r.senderId         = original.senderId;
        r.origMsgType      = original.msgType;
        r.origTimestamp    = original.timestamp;
    }
};
