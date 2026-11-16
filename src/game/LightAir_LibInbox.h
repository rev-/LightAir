#pragma once
#include <stdint.h>
#include <string.h>
#include "../config.h"
#include "../radio/LightAir_Radio.h"

// ----------------------------------------------------------------
// The library inbox.
//
// Some messages belong to a LIBRARY rather than to a ruleset: the warning
// a locked-on player gets, the trace a `beacon` beam starts
// (games/lib/projector.lua).  A ruleset should not have to declare a
// handler for every projector that might turn up on the field, so the
// runner keeps the ones no ruleset handler took and the library reads
// them on its own tick (la.inbox()).
//
//   - Only message types of the library block (RadioMsg::isLibraryMsg).
//   - A handler the ruleset declares for the type in the current state
//     WINS: the message goes to it and never reaches the inbox.
//   - A message sent to one player (RadioMsg::isDirectedLibraryMsg) is
//     acknowledged by the runner as it arrives, read or not, and the
//     acknowledgement reaches the SENDER's library through its own inbox
//     as type + 1.  A broadcast one is never answered.
//   - Small and lossy on purpose.  It holds the last SIZE messages; a
//     library that is not ticking (the player is out) loses the oldest,
//     and one that comes back reads nothing older than MAX_AGE_MS.
//     Anything that must arrive exactly once does not belong here.
//   - Emptied on every state change, like a pending measurement: what
//     arrived in one state is not acted on in the next.
// ----------------------------------------------------------------
namespace LibInboxDefs {
    constexpr uint8_t  SIZE       = 8;
    constexpr uint8_t  PAYLOAD    = 6;      // bytes kept per message
    constexpr uint32_t MAX_AGE_MS = 2000;
}

struct LibInboxEntry {
    uint8_t  msgType;
    uint8_t  sender;
    uint8_t  team;
    int8_t   rssi;
    uint8_t  len;                            // payload bytes kept
    uint8_t  payload[LibInboxDefs::PAYLOAD];
    uint32_t at;                             // millis() of arrival
};

class LibInbox {
public:
    void clear() { _head = 0; _count = 0; }

    void push(const RadioPacket& pkt, int8_t rssi, uint32_t now) {
        if (_count == LibInboxDefs::SIZE) {  // full: the oldest goes
            _head = (uint8_t)((_head + 1) % LibInboxDefs::SIZE);
            _count--;
        }
        LibInboxEntry& e = _ring[(_head + _count) % LibInboxDefs::SIZE];
        e.msgType = pkt.msgType;
        e.sender  = pkt.senderId;
        e.team    = pkt.team;
        e.rssi    = rssi;
        e.len     = pkt.payloadLen < LibInboxDefs::PAYLOAD ? pkt.payloadLen
                                                           : LibInboxDefs::PAYLOAD;
        memset(e.payload, 0, sizeof(e.payload));
        memcpy(e.payload, pkt.payload, e.len);
        e.at      = now;
        _count++;
    }

    // The oldest message still fresh, or false.  Stale ones are dropped.
    bool pop(LibInboxEntry& out, uint32_t now) {
        while (_count > 0) {
            const LibInboxEntry& e = _ring[_head];
            _head = (uint8_t)((_head + 1) % LibInboxDefs::SIZE);
            _count--;
            if (now - e.at <= LibInboxDefs::MAX_AGE_MS) { out = e; return true; }
        }
        return false;
    }

    uint8_t count() const { return _count; }

private:
    LibInboxEntry _ring[LibInboxDefs::SIZE];
    uint8_t       _head  = 0;
    uint8_t       _count = 0;
};
