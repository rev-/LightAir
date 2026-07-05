// ----------------------------------------------------------------
// LightAir_TotemEncoder.cpp — serializes one role's `totems` data
// table from a game file into TotemVM program bytes.
//
// One of the three LightAir_LuaGame translation units (see
// LightAir_LuaGameInternal.h for the map).  The wire format is
// normative in docs/totem-behavior-handshake.md §3; the executable
// reference is test/host/totemvm.lua, and the decoder that must
// accept every program produced here is src/totem/LightAir_TotemVM.cpp.
//
// Runs at game-load time inside the loader's pcall: any validation
// failure raises a Lua error (luaL_error), which rejects the whole
// game file with a message — a malformed role fails on the bench,
// never in the field.
// ----------------------------------------------------------------
#include "LightAir_LuaGame.h"
#include "LightAir_LuaGameInternal.h"
#include <string.h>

/* =========================================================
 *   WIRE CONSTANTS + NAME TABLES
 * ========================================================= */

// Wire opcodes — shared with the reference encoder and the TotemVM
// (docs/totem-behavior-handshake.md).
namespace enc {
    constexpr uint8_t T_ENTER = 0, T_EVERY = 1, T_MSG = 2, T_REPLY = 3;
    constexpr uint8_t V_IMM8 = 0, V_IMM16 = 1, V_REG = 2, V_PAYLOAD = 3,
                      V_ACCLOW = 4, V_SENDER = 5, V_SENDERTEAM = 6;
    constexpr uint8_t G_PAYLOAD = 1, G_LEN = 2, G_REG = 3, G_ACCCLASS = 4,
                      G_LOW = 5, G_ELAPSED = 6, G_RSSI = 7;
    constexpr uint8_t A_GOTO = 1, A_SET = 2, A_ACCBIT = 3, A_ACCCLR = 4,
                      A_START = 5, A_BCAST = 6, A_REPLY = 7, A_ANIM = 8;
    constexpr uint8_t C_NONE = 0, C_RGB = 1, C_TEAM = 2, C_SENDER_PLAYER = 3,
                      C_SENDER_TEAM = 4, C_ARGS = 5;
}

// Aligned with TotemUIEvent.
static const char* const kAnimNames[] = {
    "Respawn", "FlagTaken", "FlagReturn", "Bonus", "Malus", "Roster",
    "Idle", "BaseIdle", "CPIdle", "FlagIdle", "BonusIdle", "MalusIdle",
    "FlagMissing", "Control", "ControlContest",
    "Custom1", "Custom2", "Custom3", "Custom4",
};
static const uint8_t kAnimCount = sizeof(kAnimNames) / sizeof(*kAnimNames);

static int cmpCode(lua_State* L, const char* op) {
    static const NamedU8 kCmp[] = {
        { "==", 0 }, { "~=", 1 }, { "<", 2 }, { ">=", 3 }, { "<=", 4 }, { ">", 5 },
    };
    int c = LOOKUP(kCmp, op);
    if (c < 0) luaL_error(L, "totem: bad comparator '%s'", op);
    return c;
}

/* =========================================================
 *   ENCODER
 * ========================================================= */

// Small byte writer over a program buffer with luaL_error overflow.
// Holds raw pieces of LightAir_LuaGame::Prog (which is private) so the
// file-scope encoder helpers don't need access to the nested type.
// Plain constructor: the target builds with gnu++11, where a default
// member initializer would make this a non-aggregate.
struct ProgWriter {
    lua_State* L;
    uint8_t*   bytes;         // Prog::bytes
    uint8_t*   patchOff;      // Prog::patchOff
    uint8_t*   patchCount;    // &Prog::patchCount
    uint8_t    patchMax;
    uint8_t    cfgDefault;    // seconds
    uint16_t   len;
    ProgWriter(lua_State* l, uint8_t* b, uint8_t* po, uint8_t* pc,
               uint8_t pm, uint8_t cd)
        : L(l), bytes(b), patchOff(po), patchCount(pc),
          patchMax(pm), cfgDefault(cd), len(0) {}
    void put(uint8_t b) {
        if (len >= TotemVMDefs::MAX_PROG)
            luaL_error(L, "totem program exceeds %d bytes", TotemVMDefs::MAX_PROG);
        bytes[len++] = b;
    }
    void put16(uint16_t v) { put((uint8_t)(v & 0xFF)); put((uint8_t)(v >> 8)); }
};

// Encode one value spec at stack index idx.
// A {"cfg"} placeholder is encoded as a seconds→deciseconds imm16
// patch site, recorded in Prog::patchOff for reply-time patching.
static void encValue(ProgWriter& w, int idx) {
    lua_State* L = w.L;
    idx = lua_absindex(L, idx);
    if (lua_isinteger(L, idx)) {
        lua_Integer v = lua_tointeger(L, idx);
        if (v < 0 || v > 0xFFFF) luaL_error(L, "totem: value %d out of range", (int)v);
        if (v <= 0xFF) { w.put(enc::V_IMM8);  w.put((uint8_t)v); }
        else           { w.put(enc::V_IMM16); w.put16((uint16_t)v); }
        return;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_rawgeti(L, idx, 1);
    const char* tag = luaL_checkstring(L, -1);
    if (strcmp(tag, "r") == 0) {
        lua_rawgeti(L, idx, 2);
        lua_Integer r = luaL_checkinteger(L, -1);
        if (r < 0 || r >= TotemVMDefs::MAX_REGS) luaL_error(L, "totem: bad register %d", (int)r);
        w.put(enc::V_REG); w.put((uint8_t)r);
        lua_pop(L, 2);
    } else if (strcmp(tag, "p") == 0) {
        lua_rawgeti(L, idx, 2);
        lua_Integer i = luaL_checkinteger(L, -1);
        if (i < 1 || i > 200) luaL_error(L, "totem: bad payload index %d", (int)i);
        w.put(enc::V_PAYLOAD); w.put((uint8_t)i);
        lua_pop(L, 2);
    } else if (strcmp(tag, "low") == 0) {
        w.put(enc::V_ACCLOW); lua_pop(L, 1);
    } else if (strcmp(tag, "sender") == 0) {
        w.put(enc::V_SENDER); lua_pop(L, 1);
    } else if (strcmp(tag, "team") == 0) {
        w.put(enc::V_SENDERTEAM); lua_pop(L, 1);
    } else if (strcmp(tag, "cfg") == 0) {
        // Resolved at reply time: emit an imm16 (deciseconds) patch site.
        w.put(enc::V_IMM16);
        if (*w.patchCount >= w.patchMax)
            luaL_error(L, "totem: too many {\"cfg\"} sites");
        w.patchOff[(*w.patchCount)++] = (uint8_t)w.len;
        w.put16((uint16_t)(w.cfgDefault * 10));
        lua_pop(L, 1);
    } else {
        luaL_error(L, "totem: unknown value spec '%s'", tag);
    }
}

static void encGuard(ProgWriter& w, int idx) {
    lua_State* L = w.L;
    idx = lua_absindex(L, idx);
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_rawgeti(L, idx, 1);
    const char* k = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    if (strcmp(k, "p") == 0) {                 // {"p", i, op, val}
        w.put(enc::G_PAYLOAD);
        lua_rawgeti(L, idx, 3); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 2); w.put((uint8_t)luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, idx, 4); encValue(w, -1); lua_pop(L, 1);
    } else if (strcmp(k, "len") == 0) {        // {"len", op, n}
        w.put(enc::G_LEN);
        lua_rawgeti(L, idx, 2); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 3); w.put((uint8_t)luaL_checkinteger(L, -1)); lua_pop(L, 1);
    } else if (strcmp(k, "r") == 0) {          // {"r", n, op, val}
        w.put(enc::G_REG);
        lua_rawgeti(L, idx, 3); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 2); w.put((uint8_t)luaL_checkinteger(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, idx, 4); encValue(w, -1); lua_pop(L, 1);
    } else if (strcmp(k, "acc") == 0) {        // {"acc", "empty"|"single"|"many"}
        static const NamedU8 kCls[] = { {"empty",0}, {"single",1}, {"many",2} };
        w.put(enc::G_ACCCLASS);
        lua_rawgeti(L, idx, 2);
        int c = LOOKUP(kCls, luaL_checkstring(L, -1));
        if (c < 0) luaL_error(L, "totem: bad acc class");
        w.put((uint8_t)c);
        lua_pop(L, 1);
    } else if (strcmp(k, "low") == 0) {        // {"low", op, val}
        w.put(enc::G_LOW);
        lua_rawgeti(L, idx, 2); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 3); encValue(w, -1); lua_pop(L, 1);
    } else if (strcmp(k, "elapsed") == 0) {    // {"elapsed", t, op, ms|{"cfg"}}
        w.put(enc::G_ELAPSED);
        lua_rawgeti(L, idx, 3); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 2);
        lua_Integer t = luaL_checkinteger(L, -1);
        if (t < 0 || t >= TotemVMDefs::MAX_TIMERS) luaL_error(L, "totem: bad timer");
        w.put((uint8_t)t);
        lua_pop(L, 1);
        lua_rawgeti(L, idx, 4);
        if (lua_isinteger(L, -1)) {            // literal ms -> imm16 ds
            lua_Integer ms = lua_tointeger(L, -1);
            w.put(enc::V_IMM16);
            w.put16((uint16_t)(ms / 100));
        } else {
            encValue(w, -1);                   // {"cfg"} (already in ds)
        }
        lua_pop(L, 1);
    } else if (strcmp(k, "rssi") == 0) {       // {"rssi", op, dbm}
        w.put(enc::G_RSSI);
        lua_rawgeti(L, idx, 2); w.put((uint8_t)cmpCode(L, luaL_checkstring(L, -1))); lua_pop(L, 1);
        lua_rawgeti(L, idx, 3); w.put((uint8_t)(int8_t)luaL_checkinteger(L, -1)); lua_pop(L, 1);
    } else {
        luaL_error(L, "totem: unknown guard '%s'", k);
    }
}

static void encAction(ProgWriter& w, int idx, int nStates) {
    lua_State* L = w.L;
    idx = lua_absindex(L, idx);
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_rawgeti(L, idx, 1);
    const char* k = luaL_checkstring(L, -1);
    lua_pop(L, 1);

    if (strcmp(k, "goto") == 0) {
        w.put(enc::A_GOTO);
        lua_rawgeti(L, idx, 2);
        lua_Integer s = luaL_checkinteger(L, -1);
        if (s < 1 || s > nStates) luaL_error(L, "totem: goto out of range");
        w.put((uint8_t)s);
        lua_pop(L, 1);
    } else if (strcmp(k, "set") == 0) {
        w.put(enc::A_SET);
        lua_rawgeti(L, idx, 2);
        lua_Integer r = luaL_checkinteger(L, -1);
        if (r < 0 || r >= TotemVMDefs::MAX_REGS) luaL_error(L, "totem: bad register");
        w.put((uint8_t)r);
        lua_pop(L, 1);
        lua_rawgeti(L, idx, 3); encValue(w, -1); lua_pop(L, 1);
    } else if (strcmp(k, "accbit") == 0) {
        w.put(enc::A_ACCBIT);
        lua_rawgeti(L, idx, 2); encValue(w, -1); lua_pop(L, 1);
    } else if (strcmp(k, "accclr") == 0) {
        w.put(enc::A_ACCCLR);
    } else if (strcmp(k, "start") == 0) {
        w.put(enc::A_START);
        lua_rawgeti(L, idx, 2);
        lua_Integer t = luaL_checkinteger(L, -1);
        if (t < 0 || t >= TotemVMDefs::MAX_TIMERS) luaL_error(L, "totem: bad timer");
        w.put((uint8_t)t);
        lua_pop(L, 1);
    } else if (strcmp(k, "bcast") == 0) {
        w.put(enc::A_BCAST);
        lua_rawgeti(L, idx, 2);
        w.put((uint8_t)luaL_checkinteger(L, -1));
        lua_pop(L, 1);
        int n = (int)lua_rawlen(L, idx) - 2;
        if (n < 0 || n > TotemVMDefs::MAX_BCAST_TPL)
            luaL_error(L, "totem: bcast payload too long");
        w.put((uint8_t)n);
        for (int i = 0; i < n; i++) {
            lua_rawgeti(L, idx, 3 + i);
            encValue(w, -1);
            lua_pop(L, 1);
        }
    } else if (strcmp(k, "reply") == 0) {
        w.put(enc::A_REPLY);
        lua_rawgeti(L, idx, 2);
        w.put((uint8_t)luaL_checkinteger(L, -1));
        lua_pop(L, 1);
    } else if (strcmp(k, "anim") == 0) {
        w.put(enc::A_ANIM);
        lua_rawgeti(L, idx, 2);
        const char* name = luaL_checkstring(L, -1);
        int ev = -1;
        for (uint8_t i = 0; i < kAnimCount; i++)
            if (strcmp(kAnimNames[i], name) == 0) { ev = i; break; }
        if (ev < 0) luaL_error(L, "totem: unknown anim '%s'", name);
        w.put((uint8_t)ev);
        lua_pop(L, 1);

        // optional colour spec (3rd) and rhythm spec (3rd or 4th)
        int rhythmTeam = 0xFF;
        lua_rawgeti(L, idx, 3);                            // colour or rhythm or nil
        bool haveColor = false;
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            const char* tag = luaL_checkstring(L, -1);
            lua_pop(L, 1);
            if (strcmp(tag, "rhythm") == 0) {
                lua_rawgeti(L, -1, 2);
                rhythmTeam = (int)luaL_checkinteger(L, -1);
                lua_pop(L, 1);
            } else {
                haveColor = true;
                if (strcmp(tag, "rgb") == 0) {
                    w.put(enc::C_RGB);
                    for (int c = 2; c <= 4; c++) {
                        lua_rawgeti(L, -1, c);
                        w.put((uint8_t)luaL_checkinteger(L, -1));
                        lua_pop(L, 1);
                    }
                } else if (strcmp(tag, "team") == 0) {
                    w.put(enc::C_TEAM);
                    lua_rawgeti(L, -1, 2);
                    encValue(w, -1);
                    lua_pop(L, 1);
                } else if (strcmp(tag, "sender_player") == 0) {
                    w.put(enc::C_SENDER_PLAYER);
                } else if (strcmp(tag, "sender_team") == 0) {
                    w.put(enc::C_SENDER_TEAM);
                } else if (strcmp(tag, "args") == 0) {
                    w.put(enc::C_ARGS);
                    int n = (int)lua_rawlen(L, -1) - 1;
                    if (n < 0 || n > 3) luaL_error(L, "totem: anim args too long");
                    w.put((uint8_t)n);
                    for (int i = 0; i < n; i++) {
                        lua_rawgeti(L, -1, 2 + i);
                        encValue(w, -1);
                        lua_pop(L, 1);
                    }
                } else {
                    luaL_error(L, "totem: unknown colour spec '%s'", tag);
                }
            }
        }
        if (!haveColor) w.put(enc::C_NONE);
        lua_pop(L, 1);                                     // colour/rhythm/nil

        if (rhythmTeam == 0xFF) {                          // maybe a 4th element
            lua_rawgeti(L, idx, 4);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                const char* tag = luaL_checkstring(L, -1);
                lua_pop(L, 1);
                if (strcmp(tag, "rhythm") == 0) {
                    lua_rawgeti(L, -1, 2);
                    rhythmTeam = (int)luaL_checkinteger(L, -1);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        w.put((uint8_t)rhythmTeam);
    } else {
        luaL_error(L, "totem: unknown action '%s'", k);
    }
}

uint8_t LightAir_LuaGame::encodeProgram(lua_State* L, int tbl, Prog& p) {
    tbl = lua_absindex(L, tbl);
    p.patchCount = 0;

    lua_getfield(L, tbl, "vm");
    if (luaL_checkinteger(L, -1) != TotemVMDefs::VERSION)
        luaL_error(L, "totem: unsupported vm version");
    lua_pop(L, 1);

    lua_getfield(L, tbl, "cfg_default");
    p.cfgDefault = (uint8_t)luaL_optinteger(L, -1, 30);
    lua_pop(L, 1);

    lua_getfield(L, tbl, "states");
    luaL_checktype(L, -1, LUA_TTABLE);
    int states = lua_absindex(L, -1);
    int nStates = (int)lua_rawlen(L, states);
    if (nStates < 1 || nStates > TotemVMDefs::MAX_STATES)
        luaL_error(L, "totem: bad state count");

    ProgWriter w(L, p.bytes, p.patchOff, &p.patchCount,
                 (uint8_t)sizeof(p.patchOff), p.cfgDefault);
    w.put(TotemVMDefs::VERSION);
    w.put((uint8_t)nStates);

    for (int s = 1; s <= nStates; s++) {
        lua_rawgeti(L, states, s);
        int st = lua_absindex(L, -1);
        luaL_checktype(L, st, LUA_TTABLE);
        int nRules = (int)lua_rawlen(L, st);
        w.put((uint8_t)nRules);

        for (int r = 1; r <= nRules; r++) {
            lua_rawgeti(L, st, r);
            int rule = lua_absindex(L, -1);
            luaL_checktype(L, rule, LUA_TTABLE);

            // trigger — exactly one of enter/every/msg/reply
            lua_getfield(L, rule, "enter");
            bool hasEnter = lua_toboolean(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, rule, "every");
            bool hasEvery = lua_isinteger(L, -1);
            lua_Integer everyMs = hasEvery ? lua_tointeger(L, -1) : 0;
            lua_pop(L, 1);
            lua_getfield(L, rule, "msg");
            bool hasMsg = lua_isinteger(L, -1);
            lua_Integer msgT = hasMsg ? lua_tointeger(L, -1) : 0;
            lua_pop(L, 1);
            lua_getfield(L, rule, "reply");
            bool hasReply = lua_isinteger(L, -1);
            lua_Integer repT = hasReply ? lua_tointeger(L, -1) : 0;
            lua_pop(L, 1);
            int trigCount = (hasEnter ? 1 : 0) + (hasEvery ? 1 : 0) +
                            (hasMsg ? 1 : 0) + (hasReply ? 1 : 0);
            if (trigCount != 1)
                luaL_error(L, "totem: rule needs exactly one trigger");

            if (hasEnter)      w.put(enc::T_ENTER);
            else if (hasEvery) { w.put(enc::T_EVERY); w.put16((uint16_t)(everyMs / 100)); }
            else if (hasMsg)   { w.put(enc::T_MSG);   w.put((uint8_t)msgT); }
            else               { w.put(enc::T_REPLY); w.put((uint8_t)repT); }

            lua_getfield(L, rule, "cont");
            w.put(lua_toboolean(L, -1) ? 1 : 0);
            lua_pop(L, 1);

            lua_getfield(L, rule, "when");
            if (lua_istable(L, -1)) {
                int when = lua_absindex(L, -1);
                int nWhen = (int)lua_rawlen(L, when);
                w.put((uint8_t)nWhen);
                for (int g = 1; g <= nWhen; g++) {
                    lua_rawgeti(L, when, g);
                    encGuard(w, -1);
                    lua_pop(L, 1);
                }
            } else {
                w.put(0);
            }
            lua_pop(L, 1);                                 // when (or nil)

            lua_getfield(L, rule, "run");
            luaL_checktype(L, -1, LUA_TTABLE);
            int run = lua_absindex(L, -1);
            int nRun = (int)lua_rawlen(L, run);
            if (nRun < 1) luaL_error(L, "totem: rule needs actions");
            w.put((uint8_t)nRun);
            for (int a = 1; a <= nRun; a++) {
                lua_rawgeti(L, run, a);
                encAction(w, -1, nStates);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);                                 // run

            lua_pop(L, 1);                                 // rule
        }
        lua_pop(L, 1);                                     // state
    }
    lua_pop(L, 1);                                         // states table
    return (uint8_t)w.len;
}
