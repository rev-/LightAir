#include "LightAir_TotemVM.h"
#include <Arduino.h>
#include <string.h>
#include <ArduinoLog.h>

// Wire opcodes — normative encoding in docs/totem-behavior-handshake.md.
namespace {
    // triggers
    constexpr uint8_t T_ENTER = 0, T_EVERY = 1, T_MSG = 2, T_REPLY = 3;
    // value tags
    constexpr uint8_t V_IMM8 = 0, V_IMM16 = 1, V_REG = 2, V_PAYLOAD = 3,
                      V_ACCLOW = 4, V_SENDER = 5, V_SENDERTEAM = 6;
    // guards
    constexpr uint8_t G_PAYLOAD = 1, G_LEN = 2, G_REG = 3, G_ACCCLASS = 4,
                      G_LOW = 5, G_ELAPSED = 6, G_RSSI = 7;
    // actions
    constexpr uint8_t A_GOTO = 1, A_SET = 2, A_ACCBIT = 3, A_ACCCLR = 4,
                      A_START = 5, A_BCAST = 6, A_REPLY = 7, A_ANIM = 8;
    // anim colour sources
    constexpr uint8_t C_NONE = 0, C_RGB = 1, C_TEAM = 2, C_SENDER_PLAYER = 3,
                      C_SENDER_TEAM = 4, C_ARGS = 5;
    // ACC classes
    constexpr uint8_t ACC_EMPTY = 0, ACC_SINGLE = 1, ACC_MANY = 2;

    constexpr uint8_t CMP_COUNT = 6;   // == ~= < >= <= >
    constexpr uint8_t ANIM_COUNT = (uint8_t)TotemUIEvent::Custom4 + 1;
}

/* =========================================================
 *   LOAD — decode + structural validation
 * ========================================================= */

// Every skip* helper advances `off` past one encoded element and
// returns false if it would run past the program or uses an unknown
// opcode/operand — load() rejects the whole program in that case.

bool LightAir_TotemVM::skipValue(uint16_t& off) const {
    if (off >= _progLen) return false;
    switch (_prog[off++]) {
        case V_IMM8:    return (off += 1) <= _progLen;
        case V_IMM16:   return (off += 2) <= _progLen;
        case V_REG:
            if (off >= _progLen || _prog[off] >= TotemVMDefs::MAX_REGS) return false;
            off += 1;  return true;
        case V_PAYLOAD:
            if (off >= _progLen || _prog[off] < 1) return false;   // 1-based
            off += 1;  return true;
        case V_ACCLOW:
        case V_SENDER:
        case V_SENDERTEAM: return true;
        default:           return false;
    }
}

bool LightAir_TotemVM::skipGuard(uint16_t& off) const {
    if (off >= _progLen) return false;
    uint8_t op = _prog[off++];
    switch (op) {
        case G_PAYLOAD:                     // [cmp][idx][value]
            if (off + 2 > _progLen) return false;
            if (_prog[off] >= CMP_COUNT || _prog[off + 1] < 1) return false;
            off += 2;  return skipValue(off);
        case G_LEN:                         // [cmp][n]
            if (off + 2 > _progLen || _prog[off] >= CMP_COUNT) return false;
            off += 2;  return true;
        case G_REG:                         // [cmp][reg][value]
            if (off + 2 > _progLen) return false;
            if (_prog[off] >= CMP_COUNT || _prog[off + 1] >= TotemVMDefs::MAX_REGS) return false;
            off += 2;  return skipValue(off);
        case G_ACCCLASS:                    // [class]
            if (off >= _progLen || _prog[off] > ACC_MANY) return false;
            off += 1;  return true;
        case G_LOW:                         // [cmp][value]
            if (off >= _progLen || _prog[off] >= CMP_COUNT) return false;
            off += 1;  return skipValue(off);
        case G_ELAPSED:                     // [cmp][timer][value(ds)]
            if (off + 2 > _progLen) return false;
            if (_prog[off] >= CMP_COUNT || _prog[off + 1] >= TotemVMDefs::MAX_TIMERS) return false;
            off += 2;  return skipValue(off);
        case G_RSSI:                        // [cmp][i8]
            if (off + 2 > _progLen || _prog[off] >= CMP_COUNT) return false;
            off += 2;  return true;
        default: return false;
    }
}

bool LightAir_TotemVM::skipAction(uint16_t& off, uint8_t nStates) const {
    if (off >= _progLen) return false;
    switch (_prog[off++]) {
        case A_GOTO:                        // [state 1-based]
            if (off >= _progLen) return false;
            if (_prog[off] < 1 || _prog[off] > nStates) return false;
            off += 1;  return true;
        case A_SET:                         // [reg][value]
            if (off >= _progLen || _prog[off] >= TotemVMDefs::MAX_REGS) return false;
            off += 1;  return skipValue(off);
        case A_ACCBIT: return skipValue(off);
        case A_ACCCLR: return true;
        case A_START:                       // [timer]
            if (off >= _progLen || _prog[off] >= TotemVMDefs::MAX_TIMERS) return false;
            off += 1;  return true;
        case A_BCAST: {                     // [msg][n][value*n]
            if (off + 2 > _progLen) return false;
            uint8_t n = _prog[off + 1];
            if (n > TotemVMDefs::MAX_BCAST_TPL) return false;
            off += 2;
            for (uint8_t i = 0; i < n; i++)
                if (!skipValue(off)) return false;
            return true;
        }
        case A_REPLY:                       // [sub]
            return (off += 1) <= _progLen;
        case A_ANIM: {                      // [animId][color...][rhythm]
            if (off >= _progLen || _prog[off] >= ANIM_COUNT) return false;
            off += 1;
            if (off >= _progLen) return false;
            uint8_t ct = _prog[off++];
            switch (ct) {
                case C_NONE:                                    break;
                case C_RGB:   if ((off += 3) > _progLen) return false; break;
                case C_TEAM:  if (!skipValue(off)) return false;       break;
                case C_SENDER_PLAYER:
                case C_SENDER_TEAM:                             break;
                case C_ARGS: {
                    if (off >= _progLen) return false;
                    uint8_t n = _prog[off++];
                    if (n > 3) return false;
                    for (uint8_t i = 0; i < n; i++)
                        if (!skipValue(off)) return false;
                    break;
                }
                default: return false;
            }
            return (off += 1) <= _progLen;  // rhythm byte
        }
        default: return false;
    }
}

bool LightAir_TotemVM::load(const uint8_t* prog, uint16_t len) {
    _ruleCount = 0;
    _stateCount = 0;
    if (!prog || len < 3 || len > TotemVMDefs::MAX_PROG) return false;
    memcpy(_prog, prog, len);
    _progLen = len;

    uint16_t off = 0;
    if (_prog[off++] != TotemVMDefs::VERSION) return false;
    uint8_t nStates = _prog[off++];
    if (nStates < 1 || nStates > TotemVMDefs::MAX_STATES) return false;

    for (uint8_t s = 0; s < nStates; s++) {
        _stateStart[s] = _ruleCount;
        if (off >= _progLen) return false;
        uint8_t nRules = _prog[off++];
        for (uint8_t r = 0; r < nRules; r++) {
            if (_ruleCount >= TotemVMDefs::MAX_RULES) return false;
            Rule& rule = _rules[_ruleCount];
            if (off >= _progLen) return false;
            rule.trig = _prog[off++];
            switch (rule.trig) {
                case T_ENTER: rule.operand = 0; break;
                case T_EVERY:
                    if (off + 2 > _progLen) return false;
                    rule.operand = (uint16_t)(_prog[off] | (_prog[off + 1] << 8));
                    if (rule.operand == 0) return false;
                    off += 2;
                    break;
                case T_MSG:
                case T_REPLY:
                    if (off >= _progLen) return false;
                    rule.operand = _prog[off++];
                    break;
                default: return false;
            }
            if (off >= _progLen) return false;
            rule.flags = _prog[off++];

            if (off >= _progLen) return false;
            rule.whenCount = _prog[off++];
            rule.whenOff   = off;
            for (uint8_t g = 0; g < rule.whenCount; g++)
                if (!skipGuard(off)) return false;

            if (off >= _progLen) return false;
            rule.runCount = _prog[off++];
            if (rule.runCount == 0) return false;
            rule.runOff = off;
            for (uint8_t a = 0; a < rule.runCount; a++)
                if (!skipAction(off, nStates)) return false;

            rule.nextDue = 0;
            _ruleCount++;
        }
    }
    if (off != _progLen) return false;     // trailing garbage
    _stateStart[nStates] = _ruleCount;
    _stateCount = nStates;
    return true;
}

/* =========================================================
 *   RUNTIME — value / guard evaluation
 * ========================================================= */

int32_t LightAir_TotemVM::readValue(uint16_t& off) const {
    switch (_prog[off++]) {
        case V_IMM8:  return _prog[off++];
        case V_IMM16: { int32_t v = _prog[off] | (_prog[off + 1] << 8); off += 2; return v; }
        case V_REG:   return _regs[_prog[off++]];
        case V_PAYLOAD: {
            uint8_t i = _prog[off++];                       // 1-based
            if (!_pkt || i > _pkt->payloadLen) return -1;   // defensive: no match
            return _pkt->payload[i - 1];
        }
        case V_ACCLOW: {
            if (_acc == 0) return -1;
            uint8_t n = 0;
            while (!((_acc >> n) & 1u)) n++;
            return n;
        }
        case V_SENDER:     return _pkt ? _pkt->senderId : -1;
        case V_SENDERTEAM: return _pkt ? _pkt->team : -1;
        default:           return -1;                       // unreachable after load()
    }
}

bool LightAir_TotemVM::cmp(uint8_t op, int32_t a, int32_t b) {
    switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a <  b;
        case 3: return a >= b;
        case 4: return a <= b;
        case 5: return a >  b;
        default: return false;
    }
}

bool LightAir_TotemVM::evalGuards(const Rule& r) const {
    uint16_t off = r.whenOff;
    uint32_t now = millis();
    for (uint8_t g = 0; g < r.whenCount; g++) {
        bool ok = false;
        switch (_prog[off++]) {
            case G_PAYLOAD: {
                uint8_t op = _prog[off++], idx = _prog[off++];
                int32_t v = readValue(off);
                if (_pkt && idx <= _pkt->payloadLen)
                    ok = cmp(op, _pkt->payload[idx - 1], v);
                break;
            }
            case G_LEN: {
                uint8_t op = _prog[off++], n = _prog[off++];
                ok = _pkt && cmp(op, _pkt->payloadLen, n);
                break;
            }
            case G_REG: {
                uint8_t op = _prog[off++], reg = _prog[off++];
                int32_t v = readValue(off);
                ok = cmp(op, _regs[reg], v);
                break;
            }
            case G_ACCCLASS: {
                uint8_t cls = _prog[off++];
                uint8_t actual = (_acc == 0) ? ACC_EMPTY
                               : ((_acc & (_acc - 1)) == 0 ? ACC_SINGLE : ACC_MANY);
                ok = (cls == actual);
                break;
            }
            case G_LOW: {
                uint8_t op = _prog[off++];
                int32_t v = readValue(off);
                int32_t low = -1;
                if (_acc != 0) { low = 0; while (!((_acc >> low) & 1u)) low++; }
                ok = (low >= 0) && cmp(op, low, v);
                break;
            }
            case G_ELAPSED: {
                uint8_t op = _prog[off++], t = _prog[off++];
                int32_t ds = readValue(off);                 // deciseconds
                uint32_t elapsed = now - _timers[t];
                ok = cmp(op, (int32_t)(elapsed / 100u), ds);
                break;
            }
            case G_RSSI: {
                uint8_t op = _prog[off++];
                int8_t  v  = (int8_t)_prog[off++];
                ok = _pkt && cmp(op, _rssi, v);
                break;
            }
        }
        if (!ok) return false;
    }
    return true;
}

/* =========================================================
 *   RUNTIME — actions
 * ========================================================= */

void LightAir_TotemVM::doAnim(uint16_t& off, LightAir_TotemOutput& out) {
    TotemUIEvent ev = (TotemUIEvent)_prog[off++];
    uint8_t r = 0, g = 0, b = 0;
    uint8_t ct = _prog[off++];
    switch (ct) {
        case C_NONE: break;
        case C_RGB:  r = _prog[off]; g = _prog[off + 1]; b = _prog[off + 2]; off += 3; break;
        case C_TEAM: {
            int32_t team = readValue(off);
            if (team == 0xFF || team < 0) { r = g = b = 255; }         // teamless = white
            else {
                uint8_t t = (team < TeamColors::kCount) ? (uint8_t)team : 0;
                r = TeamColors::kColors[t][0];
                g = TeamColors::kColors[t][1];
                b = TeamColors::kColors[t][2];
            }
            break;
        }
        case C_SENDER_PLAYER: {
            uint8_t pid = (_pkt && _pkt->senderId < PlayerDefs::MAX_PLAYER_ID)
                          ? _pkt->senderId : 0;
            r = PlayerColors::kColors[pid][0];
            g = PlayerColors::kColors[pid][1];
            b = PlayerColors::kColors[pid][2];
            break;
        }
        case C_SENDER_TEAM: {
            uint8_t t = (_pkt && _pkt->team < TeamColors::kCount) ? _pkt->team : 0;
            r = TeamColors::kColors[t][0];
            g = TeamColors::kColors[t][1];
            b = TeamColors::kColors[t][2];
            break;
        }
        case C_ARGS: {
            uint8_t n = _prog[off++];
            int32_t vals[3] = {0, 0, 0};
            for (uint8_t i = 0; i < n; i++) vals[i] = readValue(off);
            r = (uint8_t)vals[0]; g = (uint8_t)vals[1]; b = (uint8_t)vals[2];
            break;
        }
    }
    uint8_t rhythmTeam = _prog[off++];
    uint16_t period = 0;
    uint8_t  pulses = 1;
    if (rhythmTeam != 0xFF) {
        TeamLedRhythm::Rhythm rh = TeamLedRhythm::forTeam(rhythmTeam);
        period = rh.periodMs;
        pulses = rh.pulseCount;
    }
    out.ui.trigger(ev, r, g, b, period, pulses);
}

void LightAir_TotemVM::enterState(uint8_t s, LightAir_TotemOutput& out, uint8_t depth) {
    _state = s;
    uint32_t now = millis();
    // Re-phase periodic rules of the new state.
    for (uint8_t i = _stateStart[s]; i < _stateStart[s + 1]; i++)
        if (_rules[i].trig == T_EVERY)
            _rules[i].nextDue = now + (uint32_t)_rules[i].operand * 100u;
    if (depth >= TotemVMDefs::MAX_ENTER_DEPTH) return;
    for (uint8_t i = _stateStart[s]; i < _stateStart[s + 1]; i++) {
        Rule& r = _rules[i];
        if (r.trig != T_ENTER) continue;
        if (!evalGuards(r)) continue;
        if (!runActions(r, out, depth + 1)) return;   // goto ends the event
        if (!(r.flags & 1)) break;                    // consumed
    }
}

bool LightAir_TotemVM::runActions(const Rule& r, LightAir_TotemOutput& out, uint8_t depth) {
    uint16_t off = r.runOff;
    bool wentTo = false;
    for (uint8_t a = 0; a < r.runCount; a++) {
        switch (_prog[off++]) {
            case A_GOTO: {
                uint8_t target = _prog[off++] - 1;    // wire is 1-based
                enterState(target, out, depth);
                wentTo = true;
                break;
            }
            case A_SET: {
                uint8_t reg = _prog[off++];
                _regs[reg] = (uint8_t)readValue(off);
                break;
            }
            case A_ACCBIT: {
                int32_t v = readValue(off);
                if (v >= 1 && v <= 16) _acc |= (uint16_t)(1u << (v - 1));
                break;
            }
            case A_ACCCLR: _acc = 0; break;
            case A_START:  _timers[_prog[off++]] = millis(); break;
            case A_BCAST: {
                uint8_t msg = _prog[off++];
                uint8_t n   = _prog[off++];
                uint8_t pl[TotemVMDefs::MAX_BCAST_TPL];
                for (uint8_t i = 0; i < n; i++) pl[i] = (uint8_t)readValue(off);
                out.radio.broadcast(msg, pl, n);
                break;
            }
            case A_REPLY: _replySub = _prog[off++]; _replyPending = true; break;
            case A_ANIM:  doAnim(off, out); break;
        }
    }
    return !wentTo;
}

/* =========================================================
 *   RUNTIME — event dispatch
 * ========================================================= */

void LightAir_TotemVM::dispatchPacket(uint8_t kind, LightAir_TotemOutput& out) {
    bool consumed = false;
    for (uint8_t i = _stateStart[_state]; i < _stateStart[_state + 1] && !consumed; i++) {
        Rule& r = _rules[i];
        if (r.trig != kind) continue;
        // Wire convention: even msgType = request, odd = its reply
        // (request+1).  A `reply` rule is authored with the *request*
        // type it broadcast, so match the incoming odd type against
        // operand+1.
        uint8_t match = (kind == T_REPLY) ? (uint8_t)(r.operand + 1) : (uint8_t)r.operand;
        if (!_pkt || _pkt->msgType != match) continue;
        if (!evalGuards(r)) continue;
        if (!runActions(r, out, 0)) return;            // goto: event ends
        if (!(r.flags & 1)) consumed = true;
    }
}

void LightAir_TotemVM::onActivate(const LightAir_TotemActivation&,
                                  LightAir_TotemOutput& out) {
    memset(_regs, 0, sizeof(_regs));
    _acc = 0;
    uint32_t now = millis();
    for (uint8_t t = 0; t < TotemVMDefs::MAX_TIMERS; t++) _timers[t] = now;
    enterState(0, out, 0);
}

void LightAir_TotemVM::onMessage(const RadioPacket& msg, LightAir_TotemOutput& out) {
    onPacket(msg, 0, out);
}

void LightAir_TotemVM::onPacket(const RadioPacket& msg, int8_t rssi,
                                LightAir_TotemOutput& out) {
    if (!loaded()) return;
    _pkt  = &msg;
    _rssi = rssi;
    _replyPending = false;
    _replySub     = 0;

    if (msg.msgType & 1) {
        dispatchPacket(T_REPLY, out);       // a reply to one of our broadcasts
    } else {
        dispatchPacket(T_MSG, out);         // an incoming request
        // A request is answered only when the program executed a `reply`
        // action (most totem-bound requests are broadcasts that expect no
        // answer, matching the native role runners' behaviour).
        if (_replyPending) out.radio.reply(msg, _replySub);
    }
    _pkt = nullptr;
}

void LightAir_TotemVM::update(LightAir_TotemOutput& out) {
    if (!loaded()) return;
    uint32_t now = millis();
    bool consumed = false;
    for (uint8_t i = _stateStart[_state]; i < _stateStart[_state + 1]; i++) {
        Rule& r = _rules[i];
        if (r.trig != T_EVERY) continue;
        if ((int32_t)(now - r.nextDue) < 0) continue;
        // Advance drift-free even when the firing is consumed or guarded out.
        uint32_t period = (uint32_t)r.operand * 100u;
        while ((int32_t)(now - r.nextDue) >= 0) r.nextDue += period;
        if (consumed) continue;
        if (!evalGuards(r)) continue;
        if (!runActions(r, out, 0)) return;            // goto: pass ends
        if (!(r.flags & 1)) consumed = true;
    }
}

void LightAir_TotemVM::reset() {
    _state = 0;
    _acc = 0;
    memset(_regs, 0, sizeof(_regs));
    _replyPending = false;
    // Program stays loaded; the next activation replaces it anyway.
}
