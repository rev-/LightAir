#pragma once
#include <stdint.h>
#include "../radio/LightAir_Radio.h"

// Forward declarations
struct LightAir_DisplayCtrl;
struct GameOutput;

// ----------------------------------------------------------------
// DirectRadioRule — table-driven handler for incoming radio messages.
//
// GameRunner evaluates these rules before StateRules each cycle,
// once per MessageReceived event.  First matching rule fires;
// remaining rules are skipped for that event.
//
// If no rule matches an incoming message the runner sends the
// standard reply (msgType + 1, empty payload) so the sender does
// not time out needlessly.
//
// Fields:
//   fromState    — state in which this rule is active.
//
//   msgType      — even msgType of the incoming request to match.
//
//   condition    — optional extra guard on the packet content or
//                  game state.  nullptr = always matches.
//
//   replySubType — value placed in payload[0] of the reply.
//                  0 = no payload (standard empty reply).
//                  Non-zero values distinguish reply semantics on
//                  the sender side via ReplyRadioRule.
//
//   onReceive    — optional side-effect (modify game state, queue
//                  UI events, etc.).  Called before the reply is
//                  queued.  nullptr = no effect.
//
// Example:
//   static bool hitAndAlive(const RadioPacket&) { return lives > 1; }
//   static void onLitTaken(const RadioPacket&, LightAir_DisplayCtrl&,
//                          GameOutput&) { lives--; }
//
//   static const DirectRadioRule directRadioRules[] = {
//       { IN_GAME, MSG_LIT, hitAndAlive, REPLY_TAKEN, onLitTaken },
//   };
// ----------------------------------------------------------------
struct DirectRadioRule {
    uint8_t fromState;    // state in which this rule is active
    uint8_t msgType;      // incoming even msgType to match

    bool (*condition)(const RadioPacket& pkt); // nullptr = always true

    uint8_t replySubType; // payload[0] of reply; 0 = no payload

    // Called when the rule fires, before the reply is queued.
    void (*onReceive)(const RadioPacket& pkt,
                      LightAir_DisplayCtrl&, GameOutput&); // nullptr = no effect
};
