#pragma once
#include "LightAir_TotemRunner.h"
#include "../config.h"

// ----------------------------------------------------------------
// LightAir_TotemVM — fixed state-machine interpreter for totem
// behaviour received over the radio.
//
// The program (a few dozen bytes; normative encoding in
// docs/totem-behavior-handshake.md) arrives inside the 0xF1
// activation reply and is decoded/validated by load().  The VM then
// runs it through the standard LightAir_TotemRunner lifecycle: the
// driver calls onActivate() once, onPacket() per incoming packet
// and update() per tick; reset() returns the totem to stateless.
//
// Model:
//   - up to 8 states; state 1 (index 0 internally) is initial
//   - 8 byte registers R0..R7
//   - one u16 accumulator ACC with derived CLASS (empty/single/many)
//     and LOW (lowest set bit index)
//   - 4 ms timers
//   - ordered rules per state; a firing rule consumes the event
//     unless flagged `cont`; goto enters the new state immediately
//     (its enter rules run inline), then finishes the current
//     rule's actions and stops further rules for this event.
//
// A malformed program is rejected by load(); the driver then stays
// IDLE — a broken game file can never brick a totem.
// ----------------------------------------------------------------
class LightAir_TotemVM : public LightAir_TotemRunner {
public:
    // Decode + validate a program.  Returns false (and logs) on any
    // structural error; the VM is unusable until a successful load.
    bool load(const uint8_t* prog, uint16_t len);

    bool loaded() const { return _ruleCount > 0; }

    // ---- LightAir_TotemRunner lifecycle ----
    void onActivate(const LightAir_TotemActivation& info,
                    LightAir_TotemOutput& out) override;

    // Base-interface entry (no RSSI available); forwards to onPacket().
    void onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) override;

    // Full entry used by the driver: RSSI-aware (for the rssi guard).
    void onPacket(const RadioPacket& msg, int8_t rssi, LightAir_TotemOutput& out);

    void update(LightAir_TotemOutput& out) override;
    void reset() override;

private:
    // ---- Decoded program ----
    struct Rule {
        uint8_t  trig;       // 0 enter, 1 every, 2 msg, 3 reply
        uint8_t  flags;      // bit0 = cont
        uint16_t operand;    // every: period ds; msg/reply: msgType
        uint16_t whenOff;    // guard bytes offset into _prog
        uint8_t  whenCount;
        uint16_t runOff;     // action bytes offset into _prog
        uint8_t  runCount;
        uint32_t nextDue;    // EVERY scheduling (runtime)
    };

    uint8_t  _prog[TotemVMDefs::MAX_PROG];
    uint16_t _progLen = 0;
    Rule     _rules[TotemVMDefs::MAX_RULES];
    uint8_t  _ruleCount = 0;
    uint8_t  _stateStart[TotemVMDefs::MAX_STATES + 1] = {0}; // rule index ranges
    uint8_t  _stateCount = 0;

    // ---- Runtime state ----
    uint8_t  _state = 0;                       // current state index (0-based)
    // Signed and 16-bit wide on purpose: a register has to hold both the
    // 0xFF "neutral owner" sentinel the CP role uses and a negative RSSI
    // reading, which 8 bits cannot represent distinctly either way round.
    int16_t  _regs[TotemVMDefs::MAX_REGS] = {0};
    uint16_t _acc = 0;
    uint32_t _timers[TotemVMDefs::MAX_TIMERS] = {0};

    // Context of the packet being handled (null outside onPacket).
    const RadioPacket* _pkt  = nullptr;
    int8_t             _rssi = 0;

    // Reply bookkeeping for the auto-reply convention.
    bool    _replyPending = false;
    uint8_t _replySub     = 0;

    // ---- Decode/validate helpers (load time) ----
    bool skipValue(uint16_t& off) const;
    bool skipGuard(uint16_t& off) const;
    bool skipAction(uint16_t& off, uint8_t nStates) const;

    // ---- Evaluation helpers (run time) ----
    int32_t readValue(uint16_t& off) const;
    bool    evalGuards(const Rule& r) const;
    // Runs a rule's actions.  Returns false if a goto fired (event ends).
    bool    runActions(const Rule& r, LightAir_TotemOutput& out, uint8_t depth);
    void    enterState(uint8_t s, LightAir_TotemOutput& out, uint8_t depth);
    void    doAnim(uint16_t& off, LightAir_TotemOutput& out);
    // Dispatch one event across the current state's rules.
    // kind: 0 enter (handled by enterState), 1 every-tick, 2 msg, 3 reply.
    void    dispatchPacket(uint8_t kind, LightAir_TotemOutput& out);

    static bool cmp(uint8_t op, int32_t a, int32_t b);
};
