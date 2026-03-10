#pragma once
#include <stdint.h>
#include "../radio/LightAir_Radio.h"

// Forward declarations
struct LightAir_DisplayCtrl;
struct GameOutput;

// ----------------------------------------------------------------
// ReplyRadioRule — table-driven handler for radio replies and timeouts.
//
// GameRunner evaluates these rules after DirectRadioRules and before
// StateRules each cycle, once per ReplyReceived or Timeout event.
// First matching rule fires; remaining rules are skipped for that event.
//
// Fields:
//   activeInStateMask — bitmask of states in which this rule is active.
//                       Bit N = process in state N.
//                       e.g. (1u<<IN_GAME)|(1u<<OUT_GAME) to skip GAME_END.
//
//   eventType         — RadioEventType::ReplyReceived or ::Timeout.
//                       For Timeout events, the `reply` packet passed
//                       to callbacks is zeroed; only `original` is valid.
//
//   replySubType      — payload[0] value to match in a ReplyReceived
//                       packet.  0 = match any sub-type.
//                       Ignored for Timeout events.
//
//   condition         — optional extra guard on the packets or game state.
//                       nullptr = always matches.
//
//   onReply           — side-effect callback.  For ReplyReceived, both
//                       packets are valid.  For Timeout, `reply` is zeroed.
//                       nullptr = no effect.
//
// Example:
//   static void onReplyShone(const RadioPacket&, const RadioPacket&,
//                            LightAir_DisplayCtrl&, GameOutput& out) {
//       points++;
//       out.ui.trigger(LightAir_UICtrl::UIEvent::Lit);
//   }
//
//   static const ReplyRadioRule replyRadioRules[] = {
//       { (1u<<IN_GAME)|(1u<<OUT_GAME),
//         RadioEventType::ReplyReceived, REPLY_SHONE,
//         nullptr, onReplyShone },
//   };
// ----------------------------------------------------------------
struct ReplyRadioRule {
    uint32_t       activeInStateMask; // bit N = active in state N

    RadioEventType eventType;         // ReplyReceived or Timeout

    uint8_t        replySubType;      // payload[0] to match; 0 = any (ReplyReceived only)

    bool (*condition)(const RadioPacket& reply,
                      const RadioPacket& original); // nullptr = always true

    // For Timeout events, `reply` is zeroed; only `original` is valid.
    void (*onReply)(const RadioPacket& reply,
                    const RadioPacket& original,
                    LightAir_DisplayCtrl&, GameOutput&); // nullptr = no effect
};
