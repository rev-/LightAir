// ----------------------------------------------------------------
// LightAir_LuaGame.cpp — loading a game file, synthesizing its
// LightAir_Game descriptor, and dispatching runtime callbacks into
// Lua (with the log-notify-continue fault policy).
//
// One of the three LightAir_LuaGame translation units (see
// LightAir_LuaGameInternal.h for the map).  The la.* verbs live in
// LightAir_LuaKernel.cpp; the TotemVM program serializer lives in
// LightAir_TotemEncoder.cpp.
// ----------------------------------------------------------------
#include "LightAir_LuaGame.h"
#include "LightAir_LuaGameInternal.h"
#include <Arduino.h>
#include <ArduinoLog.h>
#include <string.h>

#include "../game/LightAir_GameRunner.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../totem/TotemRoleIds.h"

#ifdef ESP32
#include <nvs.h>
#include <FS.h>
#include <LittleFS.h>
#endif

/* =========================================================
 *   NAME TABLES
 * ========================================================= */

int lookupName(const NamedU8* tab, uint8_t n, const char* name) {
    for (uint8_t i = 0; i < n; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].val;
    return -1;
}

static const NamedU8 kIcons[] = {
    { "LIGHT", ICON_LIGHT }, { "LIFE", ICON_LIFE }, { "FLAG", ICON_FLAG },
    { "HOURGLASS", ICON_HOURGLASS }, { "SCORE", ICON_SCORE },
    { "ROLE", ICON_ROLE }, { "ENERGY", ICON_ENERGY }, { "DOWN", ICON_DOWN },
    { "TIME", ICON_TIME },
};

static const NamedU8 kRoles[] = {
    { "BASE_O", TotemRoleId::BASE_O }, { "BASE_X", TotemRoleId::BASE_X },
    { "FLAG_O", TotemRoleId::FLAG_O }, { "FLAG_X", TotemRoleId::FLAG_X },
    { "CP",     TotemRoleId::CP },     { "BONUS",  TotemRoleId::BONUS },
    { "MALUS",  TotemRoleId::MALUS },  { "BASE",   TotemRoleId::BASE },
};

int lookupTotemRole(const char* name) { return LOOKUP(kRoles, name); }

/* =========================================================
 *   PROCESS-WIDE CONTEXT
 *   (one game is active at a time; set by the trampolines)
 * ========================================================= */

static LightAir_LuaGame* s_instances[LuaDefaults::MAX_LUA_GAMES] = {};
static uint8_t           s_instanceCount = 0;

LuaGameContext g_luaCtx = {};

uint8_t LightAir_LuaGame::instanceCount()            { return s_instanceCount; }
LightAir_LuaGame* LightAir_LuaGame::instance(uint8_t i) {
    return (i < s_instanceCount) ? s_instances[i] : nullptr;
}

/* =========================================================
 *   PROXY PUSH HELPERS
 * ========================================================= */

void LightAir_LuaGame::pushPkt(uint8_t which, const RadioPacket* pkt, int8_t rssi) {
    g_luaCtx.pkts[which]    = pkt;
    g_luaCtx.pktRssi[which] = rssi;
    lua_rawgeti(_engine.L(), LUA_REGISTRYINDEX, _pktUdRef[which]);
}

int LightAir_LuaGame::findSlot(const char* id) const {
    for (uint8_t i = 0; i < _slotCount; i++)
        if (strcmp(_slots[i].id, id) == 0) return i;
    return -1;
}

void LightAir_LuaGame::pushVarsProxy() {
    lua_rawgeti(_engine.L(), LUA_REGISTRYINDEX, _varsProxyRef);
}

/* =========================================================
 *   TRAMPOLINES
 * ========================================================= */

struct LuaGameTramps {
    template <uint8_t N>
    static void begin(LightAir_DisplayCtrl& d, LightAir_Radio& r,
                      LightAir_UICtrl* ui, const LightAir_GameRunner& gr) {
        if (N < s_instanceCount && s_instances[N]) {
            g_luaCtx.active = s_instances[N];
            g_luaCtx.active->doBegin(d, r, ui, gr);
        }
    }
    template <uint8_t I>
    static bool ruleWhen(const InputReport& inp, const RadioReport&) {
        g_luaCtx.inputs = &inp;
        g_luaCtx.out    = nullptr;             // conditions: direct-output fallback
        return g_luaCtx.active ? g_luaCtx.active->doRuleWhen(I) : false;
    }
    template <uint8_t I>
    static void ruleAct(LightAir_DisplayCtrl& d, GameOutput& out) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = &out;
        if (g_luaCtx.active) g_luaCtx.active->doRuleAct(I);
    }
    static void behavior(const InputReport& inp, const RadioReport&,
                         LightAir_DisplayCtrl& d, GameOutput& out) {
        g_luaCtx.inputs = &inp;
        g_luaCtx.disp   = &d;
        g_luaCtx.out    = &out;
        if (g_luaCtx.active) g_luaCtx.active->doBehavior();
    }
    static void message(const RadioPacket& pkt, int8_t rssi,
                        LightAir_DisplayCtrl& d, GameOutput& out) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = &out;
        if (g_luaCtx.active) g_luaCtx.active->doMessage(pkt, rssi, out);
    }
    static void reply(const RadioPacket& rep, const RadioPacket& orig, int8_t rssi,
                      LightAir_DisplayCtrl& d, GameOutput& out) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = &out;
        if (g_luaCtx.active) g_luaCtx.active->doReply(rep, orig, rssi);
    }
    static void timeout(const RadioPacket&, const RadioPacket& orig, int8_t,
                        LightAir_DisplayCtrl& d, GameOutput& out) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = &out;
        if (g_luaCtx.active) g_luaCtx.active->doTimeout(orig);
    }
    static void score(const ScoreTable& t, LightAir_DisplayCtrl& d) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = nullptr;
        if (g_luaCtx.active) g_luaCtx.active->doScoreAnnounce(t);
    }
    static void end(LightAir_DisplayCtrl& d) {
        g_luaCtx.disp = &d;
        g_luaCtx.out  = nullptr;
        if (g_luaCtx.active) g_luaCtx.active->doEnd();
    }
};

typedef void (*BeginFn)(LightAir_DisplayCtrl&, LightAir_Radio&,
                        LightAir_UICtrl*, const LightAir_GameRunner&);
typedef bool (*WhenFn)(const InputReport&, const RadioReport&);
typedef void (*ActFn)(LightAir_DisplayCtrl&, GameOutput&);

#define B(n) &LuaGameTramps::begin<n>
static const BeginFn kBeginTramps[LuaDefaults::MAX_LUA_GAMES] = {
    B(0), B(1), B(2), B(3)
};
#undef B
#define W(n) &LuaGameTramps::ruleWhen<n>
static const WhenFn kWhenTramps[LuaDefaults::MAX_RULES] = {
    W(0), W(1), W(2),  W(3),  W(4),  W(5),  W(6),  W(7),
    W(8), W(9), W(10), W(11), W(12), W(13), W(14), W(15)
};
#undef W
#define A(n) &LuaGameTramps::ruleAct<n>
static const ActFn kActTramps[LuaDefaults::MAX_RULES] = {
    A(0), A(1), A(2),  A(3),  A(4),  A(5),  A(6),  A(7),
    A(8), A(9), A(10), A(11), A(12), A(13), A(14), A(15)
};
#undef A

const TotemProgramEntry* LightAir_LuaGame::progTramp(uint8_t roleId) {
    return g_luaCtx.active ? g_luaCtx.active->patchedProgram(roleId) : nullptr;
}

/* =========================================================
 *   RUNTIME DISPATCH
 * ========================================================= */

// Policy: log, notify, continue.
//
// The failed callback is a no-op for this event; both the Lua state and all
// C++ state are consistent after the pcall, so the game simply proceeds
// from the previous condition on the next tick.  The one exception is
// on_begin — a game that cannot establish its starting condition refuses
// to play (forced straight into scoring_state).
//
// Everything is counted in _faults (per site + total + last message) so a
// stricter future policy — end the match, prompt "return to boxes",
// per-site circuit breaker — only has to be added in maybeEscalate().
static const char* const kFaultSiteNames[] = {
    "on_begin", "rule.when", "rule.action", "update", "on_message",
    "on_reply", "on_reply.timeout", "on_score_announce", "on_end",
};

static constexpr uint32_t kFaultNoticeCooldownMs = 10000;

void LightAir_LuaGame::luaFault(FaultSite site) {
    _faults.total++;
    _faults.perSite[(uint8_t)site]++;
    _faults.lastSite = site;
    _faults.lastAtMs = millis();
    strncpy(_faults.lastError, _engine.lastError(), sizeof(_faults.lastError) - 1);
    _faults.lastError[sizeof(_faults.lastError) - 1] = 0;

    Log.errorln("LuaGame[%s] fault #%d in %s: %s",
                _name, _faults.total,
                kFaultSiteNames[(uint8_t)site], _faults.lastError);

    // Tray notice, throttled: players should know the game file is buggy,
    // but a hot-path fault must not repaint the tray 100x per second.
    uint32_t now = millis();
    if (g_luaCtx.disp && (now - _lastFaultNoticeAt >= kFaultNoticeCooldownMs ||
                          _lastFaultNoticeAt == 0)) {
        _lastFaultNoticeAt = now;
        g_luaCtx.disp->showMessage("Lua error!", 5000);
    }

    maybeEscalate(site);
}

// The single hook for any future stricter policy.  Current policy: only a
// failed on_begin ends the match (the game never started properly).
// Everything needed to go further is already accounted in _faults, e.g.:
//   if (_faults.total > 50) { _state = _game.scoringState; }        // end
//   if (_faults.perSite[(uint8_t)site] > 8) { /* disable site */ }  // breaker
void LightAir_LuaGame::maybeEscalate(FaultSite site) {
    if (site == FaultSite::Begin && _game.scoringState != 255)
        _state = _game.scoringState;
}

// Persist the lifetime fault total to NVS so field diagnostics survive the
// end-of-match reboot.  Called from doEnd() (the A+B restart path).
void LightAir_LuaGame::persistFaultTotal() {
#ifdef ESP32
    if (_faults.total == 0) return;
    nvs_handle_t h;
    if (nvs_open("lightair", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t lifetime = 0;
    nvs_get_u32(h, "lua_faults", &lifetime);
    lifetime += _faults.total;
    nvs_set_u32(h, "lua_faults", lifetime);
    nvs_commit(h);
    nvs_close(h);
    Log.infoln("LuaGame[%s]: %d fault(s) this match, %d lifetime",
               _name, _faults.total, lifetime);
#endif
}

void LightAir_LuaGame::doBegin(LightAir_DisplayCtrl& d, LightAir_Radio& r,
                               LightAir_UICtrl* ui, const LightAir_GameRunner& gr) {
    g_luaCtx.disp   = &d;
    g_luaCtx.radio  = &r;
    g_luaCtx.ui     = ui;
    g_luaCtx.runner = &gr;
    g_luaCtx.out    = nullptr;
    _lastSecTick = millis();
    memset(&_faults, 0, sizeof(_faults));
    _lastFaultNoticeAt = 0;
    if (_beginRef == LUA_NOREF) return;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _beginRef);
    pushVarsProxy();
    if (!_engine.pcall(1, 0)) luaFault(FaultSite::Begin);
}

bool LightAir_LuaGame::doRuleWhen(uint8_t idx) {
    if (idx >= _game.ruleCount || _ruleWhenRef[idx] == LUA_NOREF) return false;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _ruleWhenRef[idx]);
    pushVarsProxy();
    if (!_engine.pcall(1, 1)) { luaFault(FaultSite::RuleWhen); return false; }
    bool ok = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}

void LightAir_LuaGame::doRuleAct(uint8_t idx) {
    if (idx >= _game.ruleCount || _ruleActRef[idx] == LUA_NOREF) return;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _ruleActRef[idx]);
    pushVarsProxy();
    if (!_engine.pcall(1, 0)) luaFault(FaultSite::RuleAction);
}

void LightAir_LuaGame::tickCountdowns() {
    uint32_t now = millis();
    while (now - _lastSecTick >= 1000) {
        _lastSecTick += 1000;
        for (uint8_t i = 0; i < _countdownCount; i++) {
            if (!(_countdowns[i].stateMask & (1u << _state))) continue;
            VarSlot& s = _slots[_countdowns[i].slot];
            if (s.val > 0) s.val--;
        }
    }
}

void LightAir_LuaGame::doBehavior() {
    tickCountdowns();
    if (_state < LuaDefaults::MAX_STATES && _updateRef[_state] != LUA_NOREF) {
        lua_State* L = _engine.L();
        lua_rawgeti(L, LUA_REGISTRYINDEX, _updateRef[_state]);
        pushVarsProxy();
        if (!_engine.pcall(1, 0)) luaFault(FaultSite::Update);
    }
    _engine.gcStep();                    // collect in the loop's slack window
}

void LightAir_LuaGame::doMessage(const RadioPacket& pkt, int8_t rssi, GameOutput& out) {
    uint8_t sub     = 0;
    bool    answers = false;
    if (_state < LuaDefaults::MAX_STATES && _msgTabRef[_state] != LUA_NOREF) {
        lua_State* L = _engine.L();
        lua_rawgeti(L, LUA_REGISTRYINDEX, _msgTabRef[_state]);
        lua_rawgeti(L, -1, pkt.msgType);
        lua_remove(L, -2);                                 // drop the table
        if (lua_isfunction(L, -1)) {
            pushVarsProxy();
            pushPkt(0, &pkt, rssi);
            if (!_engine.pcall(2, 1)) {
                luaFault(FaultSite::Message);
            } else {
                if (lua_isinteger(L, -1)) {
                    sub     = (uint8_t)lua_tointeger(L, -1);
                    answers = true;
                }
                lua_pop(L, 1);
            }
            g_luaCtx.pkts[0] = nullptr;
        } else {
            lua_pop(L, 1);                                 // non-function entry
        }
    }
    // The handler's return value IS the reply: an integer answers with that
    // sub-type, no return answers nothing.  A handler that only observes a
    // beacon (tracking a CP owner, ignoring an out-of-range base) stays
    // silent, so a totem waiting on a deliberate answer hears only the
    // players that actually acted on it.
    if (answers) out.radio.reply(pkt, sub);
}

void LightAir_LuaGame::doReply(const RadioPacket& reply, const RadioPacket& orig,
                               int8_t rssi) {
    if (_replyTabRef == LUA_NOREF) return;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _replyTabRef);
    lua_rawgeti(L, -1, orig.msgType);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    uint8_t sub = reply.payloadLen ? reply.payload[0] : 0;
    lua_rawgeti(L, -1, sub);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    pushVarsProxy();
    pushPkt(1, &reply, rssi);
    pushPkt(2, &orig, 0);          // our own sent packet: nothing was measured
    if (!_engine.pcall(3, 0)) luaFault(FaultSite::Reply);
    g_luaCtx.pkts[1] = g_luaCtx.pkts[2] = nullptr;
}

void LightAir_LuaGame::doTimeout(const RadioPacket& orig) {
    if (_replyTabRef == LUA_NOREF) return;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _replyTabRef);
    lua_rawgeti(L, -1, orig.msgType);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "timeout");
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
    pushVarsProxy();
    pushPkt(2, &orig, 0);
    if (!_engine.pcall(2, 0)) luaFault(FaultSite::Timeout);
    g_luaCtx.pkts[2] = nullptr;
}

void LightAir_LuaGame::doScoreAnnounce(const ScoreTable& t) {
    if (_scoreRef == LUA_NOREF) return;
    lua_State* L = _engine.L();
    lua_rawgeti(L, LUA_REGISTRYINDEX, _scoreRef);

    lua_newtable(L);                                       // scores array
    int n = 0;
    for (uint8_t pid = 1; pid < PlayerDefs::MAX_PLAYER_ID; pid++) {
        if (!(t.accumMask & (1u << pid))) continue;
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, pid);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, t.teamMap ? t.teamMap[pid] : 0xFF);
        lua_setfield(L, -2, "team");
        lua_createtable(L, t.winnerVarCount, 0);
        for (uint8_t v = 0; v < t.winnerVarCount; v++) {
            int32_t val;
            memcpy(&val, t.slots[pid] + v * 4, 4);
            lua_pushinteger(L, val);
            lua_rawseti(L, -2, v + 1);
        }
        lua_setfield(L, -2, "vals");
        lua_rawseti(L, -2, ++n);
    }
    if (!_engine.pcall(1, 0)) luaFault(FaultSite::Score);
}

void LightAir_LuaGame::doEnd() {
    if (_endRef != LUA_NOREF) {
        lua_State* L = _engine.L();
        lua_rawgeti(L, LUA_REGISTRYINDEX, _endRef);
        pushVarsProxy();
        if (!_engine.pcall(1, 0)) luaFault(FaultSite::End);
    }
    persistFaultTotal();
}

const TotemProgramEntry* LightAir_LuaGame::patchedProgram(uint8_t roleId) {
    for (uint8_t i = 0; i < _progCount; i++) {
        Prog& p = _progs[i];
        if (p.entry.roleId != roleId) continue;
        int secs = (p.cfgSlot >= 0) ? _slots[p.cfgSlot].val : p.cfgDefault;
        uint16_t ds = (secs < 0) ? 0
                    : (secs > 6553) ? 65535 : (uint16_t)(secs * 10);
        for (uint8_t j = 0; j < p.patchCount; j++) {
            p.bytes[p.patchOff[j]]     = (uint8_t)(ds & 0xFF);
            p.bytes[p.patchOff[j] + 1] = (uint8_t)(ds >> 8);
        }
        return &p.entry;
    }
    return nullptr;
}

/* =========================================================
 *   LOADER — table walking + validation
 *   (runs inside a pcall; validation failures use luaL_error)
 * ========================================================= */

static lua_Integer fieldInt(lua_State* L, int tbl, const char* k,
                            lua_Integer def, bool required) {
    lua_getfield(L, tbl, k);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (required) luaL_error(L, "missing field '%s'", k);
        return def;
    }
    lua_Integer v = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return v;
}

// Copies a string field into buf; errors if required and missing/too long.
static void fieldStr(lua_State* L, int tbl, const char* k,
                     char* buf, size_t bufLen, bool required) {
    lua_getfield(L, tbl, k);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (required) luaL_error(L, "missing field '%s'", k);
        buf[0] = 0;
        return;
    }
    const char* s = luaL_checkstring(L, -1);
    if (strlen(s) >= bufLen) luaL_error(L, "field '%s' too long", k);
    strcpy(buf, s);
    lua_pop(L, 1);
}

// Takes a function field as a registry ref (LUA_NOREF if absent).
static int fieldFnRef(lua_State* L, int tbl, const char* k) {
    lua_getfield(L, tbl, k);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return LUA_NOREF; }
    luaL_checktype(L, -1, LUA_TFUNCTION);
    return luaL_ref(L, LUA_REGISTRYINDEX);                 // pops
}

void LightAir_LuaGame::noteState(lua_State* L, int s) {
    if (s < 0 || s >= LuaDefaults::MAX_STATES)
        luaL_error(L, "state %d out of range (max %d)", s, LuaDefaults::MAX_STATES - 1);
    if ((uint8_t)s > _stateMax) _stateMax = (uint8_t)s;
}

int LightAir_LuaGame::addSlot(lua_State* L, const char* id, bool text) {
    if (_slotCount >= LuaDefaults::MAX_VARS)
        luaL_error(L, "too many vars (max %d)", LuaDefaults::MAX_VARS);
    if (strlen(id) >= LuaDefaults::MAX_VAR_ID)
        luaL_error(L, "var id '%s' too long", id);
    if (findSlot(id) >= 0)
        luaL_error(L, "duplicate var id '%s'", id);
    VarSlot& s = _slots[_slotCount];
    strcpy(s.id, id);
    s.isText  = text;
    s.val     = 0;
    s.text[0] = 0;
    return _slotCount++;
}

int LightAir_LuaGame::loaderBody(lua_State* L) {
    LightAir_LuaGame* g = (LightAir_LuaGame*)lua_touserdata(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    g->loadFromTable(L, 2);
    return 0;
}

void LightAir_LuaGame::loadFromTable(lua_State* L, int tbl) {
    tbl = lua_absindex(L, tbl);

    // ---- manifest ----
    if (fieldInt(L, tbl, "api", 0, true) != LuaDefaults::API_VERSION)
        luaL_error(L, "unsupported api version");
    lua_Integer typeId = fieldInt(L, tbl, "type_id", 0, true);
    if (typeId <= 0 || typeId > 0xFFFF) luaL_error(L, "bad type_id");
    _game.typeId = (uint16_t)typeId;
    fieldStr(L, tbl, "name", _name, sizeof(_name), true);
    _game.name = _name;

    _game.initialState = (uint8_t)fieldInt(L, tbl, "initial_state", 0, false);
    _game.scoringState = (uint8_t)fieldInt(L, tbl, "scoring_state", 255, false);
    _game.scoreMsgType = (uint8_t)fieldInt(L, tbl, "score_msg",
                                           RadioMsg::MSG_SCORE_COLLECT, false);
    noteState(L, _game.initialState);
    if (_game.scoringState != 255) noteState(L, _game.scoringState);

    // ---- config vars ----
    uint8_t configCount = 0;
    lua_getfield(L, tbl, "config");
    if (lua_istable(L, -1)) {
        int cfg = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, cfg);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, cfg, i);
            int e = lua_absindex(L, -1);
            char id[LuaDefaults::MAX_VAR_ID];
            fieldStr(L, e, "id", id, sizeof(id), true);
            int slot = addSlot(L, id, false);
            fieldStr(L, e, "name", _cfgNames[configCount],
                     LuaDefaults::MAX_CFG_NAME, true);
            _slots[slot].val = (int)fieldInt(L, e, "default", 0, false);
            _configVars[configCount].name  = _cfgNames[configCount];
            _configVars[configCount].value = &_slots[slot].val;
            _configVars[configCount].min   = (int)fieldInt(L, e, "min", 0, true);
            _configVars[configCount].max   = (int)fieldInt(L, e, "max", 0, true);
            _configVars[configCount].step  = (int)fieldInt(L, e, "step", 1, false);
            configCount++;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ---- game vars ----
    lua_getfield(L, tbl, "vars");
    if (lua_istable(L, -1)) {
        int vars = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, vars);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, vars, i);
            int e = lua_absindex(L, -1);
            char id[LuaDefaults::MAX_VAR_ID];
            fieldStr(L, e, "id", id, sizeof(id), true);
            lua_getfield(L, e, "text");
            bool isText = lua_toboolean(L, -1);
            lua_pop(L, 1);
            int slot = addSlot(L, id, isText);
            if (isText) {
                lua_getfield(L, e, "default");
                if (lua_isstring(L, -1)) {
                    strncpy(_slots[slot].text, lua_tostring(L, -1),
                            LuaDefaults::MAX_TEXT_LEN - 1);
                }
                lua_pop(L, 1);
            } else {
                _slots[slot].val = (int)fieldInt(L, e, "default", 0, false);
            }
            // countdown_in = { states... }
            lua_getfield(L, e, "countdown_in");
            if (lua_istable(L, -1)) {
                if (isText) luaL_error(L, "text var cannot count down");
                if (_countdownCount >= LuaDefaults::MAX_COUNTDOWNS)
                    luaL_error(L, "too many countdown vars");
                uint32_t mask = 0;
                int cn = (int)lua_rawlen(L, -1);
                for (int c = 1; c <= cn; c++) {
                    lua_rawgeti(L, -1, c);
                    int st = (int)luaL_checkinteger(L, -1);
                    noteState(L, st);
                    mask |= (1u << st);
                    lua_pop(L, 1);
                }
                _countdowns[_countdownCount].slot      = (uint8_t)slot;
                _countdowns[_countdownCount].stateMask = mask;
                _countdownCount++;
            }
            lua_pop(L, 1);                                 // countdown_in
            lua_pop(L, 1);                                 // entry
        }
    }
    lua_pop(L, 1);

    // ---- monitor ----
    uint8_t monitorCount = 0;
    lua_getfield(L, tbl, "monitor");
    if (lua_istable(L, -1)) {
        int mon = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, mon);
        if (n > LuaDefaults::MAX_MONITOR) luaL_error(L, "too many monitor entries");
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, mon, i);
            int e = lua_absindex(L, -1);
            char id[LuaDefaults::MAX_VAR_ID];
            fieldStr(L, e, "var", id, sizeof(id), true);
            int slot = findSlot(id);
            if (slot < 0) luaL_error(L, "monitor var '%s' not declared", id);
            char iconName[12];
            fieldStr(L, e, "icon", iconName, sizeof(iconName), true);
            int icon = LOOKUP(kIcons, iconName);
            if (icon < 0) luaL_error(L, "unknown icon '%s'", iconName);
            uint8_t col = (uint8_t)fieldInt(L, e, "col", 0, true);
            uint8_t row = (uint8_t)fieldInt(L, e, "row", 0, true);
            uint32_t mask = 0;
            lua_getfield(L, e, "states");
            luaL_checktype(L, -1, LUA_TTABLE);
            int sn = (int)lua_rawlen(L, -1);
            for (int s = 1; s <= sn; s++) {
                lua_rawgeti(L, -1, s);
                int st = (int)luaL_checkinteger(L, -1);
                noteState(L, st);
                mask |= (1u << st);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);                                 // states

            // A `bar` row shows its number until the value reaches `bar_at`,
            // and a filling bar while it sits there.  Both the duration and
            // the instant the wait began come from other game vars, because
            // the timing belongs to whoever owns the wait — for energy that
            // is the projector, whose recharge starts on the trigger's
            // release rather than when the pool hit zero.
            lua_getfield(L, e, "bar");
            const bool isBar = lua_toboolean(L, -1);
            lua_pop(L, 1);

            if (isBar) {
                if (_slots[slot].isText)
                    luaL_error(L, "monitor bar '%s' is a text var", id);
                char fillId[LuaDefaults::MAX_VAR_ID];
                fieldStr(L, e, "fill_var", fillId, sizeof(fillId), true);
                int fillSlot = findSlot(fillId);
                if (fillSlot < 0 || _slots[fillSlot].isText)
                    luaL_error(L, "monitor bar fill_var '%s' invalid", fillId);

                // start_var is optional: without one the display self-starts
                // its clock when the value arrives at the trigger.
                const int* startPtr = nullptr;
                char startId[LuaDefaults::MAX_VAR_ID];
                lua_getfield(L, e, "start_var");
                if (lua_isstring(L, -1)) {
                    lua_pop(L, 1);
                    fieldStr(L, e, "start_var", startId, sizeof(startId), true);
                    int ss = findSlot(startId);
                    if (ss < 0 || _slots[ss].isText)
                        luaL_error(L, "monitor bar start_var '%s' invalid", startId);
                    startPtr = &_slots[ss].val;
                } else {
                    lua_pop(L, 1);
                }

                _monitorVars[monitorCount] = MonitorVar::Bar(
                    _slots[slot].id, &_slots[slot].val, mask, (IconType)icon, col, row,
                    (int)fieldInt(L, e, "bar_at", 0, false),
                    &_slots[fillSlot].val, startPtr,
                    (uint8_t)fieldInt(L, e, "width", 0, false));
            } else if (_slots[slot].isText) {
                _monitorVars[monitorCount] = MonitorVar::Str(
                    _slots[slot].id, _slots[slot].text, mask, (IconType)icon, col, row);
            } else {
                _monitorVars[monitorCount] = MonitorVar::Int(
                    _slots[slot].id, &_slots[slot].val, mask, (IconType)icon, col, row);
            }
            monitorCount++;
            lua_pop(L, 1);                                 // entry
        }
    }
    lua_pop(L, 1);

    // ---- winners ----
    uint8_t winnerCount = 0;
    lua_getfield(L, tbl, "winners");
    if (lua_istable(L, -1)) {
        int win = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, win);
        if (n > GameDefaults::MAX_WINNER_VARS) luaL_error(L, "too many winner vars");
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, win, i);
            int e = lua_absindex(L, -1);
            char id[LuaDefaults::MAX_VAR_ID];
            fieldStr(L, e, "var", id, sizeof(id), true);
            int slot = findSlot(id);
            if (slot < 0 || _slots[slot].isText)
                luaL_error(L, "winner var '%s' invalid", id);
            char dir[6];
            fieldStr(L, e, "dir", dir, sizeof(dir), true);
            _winnerVars[winnerCount].value = &_slots[slot].val;
            _winnerVars[winnerCount].dir =
                (strcmp(dir, "min") == 0) ? WinnerDir::MIN : WinnerDir::MAX;
            winnerCount++;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ---- teams ----
    uint8_t teams = (uint8_t)fieldInt(L, tbl, "teams", 0, false);
    if (teams > TeamColors::kCount) luaL_error(L, "too many teams");
    memset(_teamMap, 0xFF, sizeof(_teamMap));

    // ---- totem_slots (requirements) + per-role config slot ----
    uint8_t totReqCount = 0;
    int8_t  roleCfgSlot[TotemDefs::MAX_TOTEM_ROLES];
    uint8_t roleIds[TotemDefs::MAX_TOTEM_ROLES];
    lua_getfield(L, tbl, "totem_slots");
    if (lua_istable(L, -1)) {
        int ts = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, ts);
        if (n > TotemDefs::MAX_TOTEM_ROLES) luaL_error(L, "too many totem roles");
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, ts, i);
            int e = lua_absindex(L, -1);
            char roleName[12];
            fieldStr(L, e, "role", roleName, sizeof(roleName), true);
            int roleId = lookupTotemRole(roleName);
            if (roleId < 0) luaL_error(L, "unknown totem role '%s'", roleName);
            _totReqs[totReqCount].roleId   = (uint8_t)roleId;
            _totReqs[totReqCount].minCount = (uint8_t)fieldInt(L, e, "min", 0, false);
            _totReqs[totReqCount].maxCount = (uint8_t)fieldInt(L, e, "max", 1, false);
            _totReqs[totReqCount].configSecs = nullptr;
            roleCfgSlot[totReqCount] = -1;
            roleIds[totReqCount]     = (uint8_t)roleId;
            char cfgVar[LuaDefaults::MAX_VAR_ID];
            fieldStr(L, e, "config_var", cfgVar, sizeof(cfgVar), false);
            if (cfgVar[0]) {
                int slot = findSlot(cfgVar);
                if (slot < 0 || _slots[slot].isText)
                    luaL_error(L, "totem config_var '%s' invalid", cfgVar);
                _totReqs[totReqCount].configSecs = &_slots[slot].val;
                roleCfgSlot[totReqCount] = (int8_t)slot;
            }
            totReqCount++;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ---- time_left_var ----
    {
        char id[LuaDefaults::MAX_VAR_ID];
        fieldStr(L, tbl, "time_left_var", id, sizeof(id), false);
        if (id[0]) {
            int slot = findSlot(id);
            if (slot < 0 || _slots[slot].isText)
                luaL_error(L, "time_left_var '%s' invalid", id);
            _game.gameTimeLeft = &_slots[slot].val;
        }
    }

    // ---- lifecycle handlers ----
    _beginRef = fieldFnRef(L, tbl, "on_begin");
    _scoreRef = fieldFnRef(L, tbl, "on_score_announce");
    _endRef   = fieldFnRef(L, tbl, "on_end");

    // ---- on_message: per-state handler tables + DirectRadioRule rows ----
    uint8_t directCount = 0;
    lua_getfield(L, tbl, "on_message");
    if (lua_istable(L, -1)) {
        int om = lua_absindex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, om) != 0) {
            // key = state, value = { [msgType] = fn }
            int st = (int)luaL_checkinteger(L, -2);
            noteState(L, st);
            luaL_checktype(L, -1, LUA_TTABLE);
            int sub = lua_absindex(L, -1);
            // enumerate msgTypes for the rule rows
            lua_pushnil(L);
            while (lua_next(L, sub) != 0) {
                int msgType = (int)luaL_checkinteger(L, -2);
                luaL_checktype(L, -1, LUA_TFUNCTION);
                if (directCount >= LuaDefaults::MAX_MSG_RULES)
                    luaL_error(L, "too many message handlers");
                _directRules[directCount].fromState    = (uint8_t)st;
                _directRules[directCount].msgType      = (uint8_t)msgType;
                _directRules[directCount].condition    = nullptr;
                _directRules[directCount].replySubType = DirectRadioRule::DYNAMIC_REPLY;
                _directRules[directCount].onReceive    = &LuaGameTramps::message;
                directCount++;
                lua_pop(L, 1);                             // value; keep key
            }
            lua_pushvalue(L, sub);
            _msgTabRef[st] = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pop(L, 1);                                 // value; keep key
        }
    }
    lua_pop(L, 1);

    // ---- on_reply ----
    uint8_t replyRuleCount = 0;
    lua_getfield(L, tbl, "on_reply");
    if (lua_istable(L, -1)) {
        _replyTabRef = luaL_ref(L, LUA_REGISTRYINDEX);     // pops
    } else {
        lua_pop(L, 1);
    }

    // ---- rules ----
    uint8_t ruleCount = 0;
    lua_getfield(L, tbl, "rules");
    if (lua_istable(L, -1)) {
        int rl = lua_absindex(L, -1);
        int n = (int)lua_rawlen(L, rl);
        if (n > LuaDefaults::MAX_RULES) luaL_error(L, "too many rules");
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, rl, i);
            int e = lua_absindex(L, -1);
            int from = (int)fieldInt(L, e, "from", 0, true);
            int to   = (int)fieldInt(L, e, "to", 0, true);
            noteState(L, from);
            noteState(L, to);
            _ruleWhenRef[ruleCount] = fieldFnRef(L, e, "when");
            _ruleActRef[ruleCount]  = fieldFnRef(L, e, "action");
            _rules[ruleCount].fromState = (uint8_t)from;
            _rules[ruleCount].toState   = (uint8_t)to;
            _rules[ruleCount].condition =
                (_ruleWhenRef[ruleCount] != LUA_NOREF) ? kWhenTramps[ruleCount] : nullptr;
            _rules[ruleCount].onTransition =
                (_ruleActRef[ruleCount] != LUA_NOREF) ? kActTramps[ruleCount] : nullptr;
            ruleCount++;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // ---- update ----
    lua_getfield(L, tbl, "update");
    if (lua_istable(L, -1)) {
        int up = lua_absindex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, up) != 0) {
            int st = (int)luaL_checkinteger(L, -2);
            noteState(L, st);
            luaL_checktype(L, -1, LUA_TFUNCTION);
            _updateRef[st] = luaL_ref(L, LUA_REGISTRYINDEX);   // pops value
        }
    }
    lua_pop(L, 1);

    // ---- totems: serialize TotemVM programs ----
    lua_getfield(L, tbl, "totems");
    if (lua_istable(L, -1)) {
        int tt = lua_absindex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, tt) != 0) {
            const char* roleName = luaL_checkstring(L, -2);
            int roleId = lookupTotemRole(roleName);
            if (roleId < 0) luaL_error(L, "unknown totem role '%s'", roleName);
            if (_progCount >= TotemDefs::MAX_TOTEM_ROLES)
                luaL_error(L, "too many totem programs");
            luaL_checktype(L, -1, LUA_TTABLE);
            Prog& p = _progs[_progCount];
            p.entry.roleId = (uint8_t)roleId;
            p.cfgSlot = -1;
            for (uint8_t r = 0; r < totReqCount; r++)
                if (roleIds[r] == (uint8_t)roleId) p.cfgSlot = roleCfgSlot[r];
            p.entry.len   = encodeProgram(L, -1, p);
            p.entry.bytes = p.bytes;
            _progCount++;
            lua_pop(L, 1);                                 // value; keep key
        }
    }
    lua_pop(L, 1);

    // ---- synthesize reply rules (need the final state mask) ----
    uint32_t allStates = (uint32_t)((1u << (_stateMax + 1)) - 1);
    uint32_t replyMask = allStates;
    if (_game.scoringState != 255) replyMask &= ~(1u << _game.scoringState);
    if (_replyTabRef != LUA_NOREF) {
        _replyRules[0] = { replyMask, RadioEventType::ReplyReceived, 0,
                           nullptr, &LuaGameTramps::reply };
        _replyRules[1] = { replyMask, RadioEventType::Timeout, 0,
                           nullptr, &LuaGameTramps::timeout };
        replyRuleCount = 2;
    }

    // ---- per-state behaviours (countdowns + update + GC every tick) ----
    for (uint8_t s = 0; s <= _stateMax; s++)
        _behaviors[s] = { s, &LuaGameTramps::behavior };

    // ---- assemble the descriptor ----
    _game.configVars           = _configVars;
    _game.configCount          = configCount;
    _game.monitorVars          = _monitorVars;
    _game.monitorCount         = monitorCount;
    _game.directRadioRules     = directCount ? _directRules : nullptr;
    _game.directRadioRuleCount = directCount;
    _game.replyRadioRules      = replyRuleCount ? _replyRules : nullptr;
    _game.replyRadioRuleCount  = replyRuleCount;
    _game.rules                = ruleCount ? _rules : nullptr;
    _game.ruleCount            = ruleCount;
    _game.behaviors            = _behaviors;
    _game.behaviorCount        = (uint8_t)(_stateMax + 1);
    _game.currentState         = &_state;
    _game.onBegin              = kBeginTramps[_slotIdx];
    _game.winnerVars           = winnerCount ? _winnerVars : nullptr;
    _game.winnerVarCount       = winnerCount;
    _game.onScoreAnnounce      = (_scoreRef != LUA_NOREF) ? &LuaGameTramps::score : nullptr;
    _game.totemRequirements    = totReqCount ? _totReqs : nullptr;
    _game.totemRequirementCount = totReqCount;
    _game.teamCount            = teams;
    _game.teamMap              = teams ? _teamMap : nullptr;
    _game.onEnd                = (_endRef != LUA_NOREF) ? &LuaGameTramps::end : nullptr;
    _game.totemProgram         = _progCount ? &LightAir_LuaGame::progTramp : nullptr;
}

/* =========================================================
 *   LOAD / UNLOAD
 * ========================================================= */

// ----------------------------------------------------------------
// loadChunk — compile the file at `path` into a chunk left on the
// Lua stack.  Returns LUA_OK, or an error code with the message on
// the stack (same contract as luaL_loadfile).
//
// On the device this must NOT go through luaL_loadfile: that calls
// plain fopen(), which resolves against the ESP-IDF VFS, while the
// Arduino core mounts LittleFS under its own base path.  A LittleFS
// path such as "/games/flag.lua" is invisible to fopen(), so every
// game file failed to open and no game ever reached the menu.  Read
// through the LittleFS object instead — exactly what la.lib() already
// does for library modules — and compile from the buffer.
// ----------------------------------------------------------------
#ifdef ESP32
static int loadChunk(lua_State* L, const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        lua_pushfstring(L, "cannot open %s", path);
        return LUA_ERRFILE;
    }
    size_t size = f.size();
    // Scratch buffer as a userdatum: GC-managed, so a load error cannot
    // leak it, and it needs no heap fragmentation-prone malloc/free pair.
    char*  buf  = (char*)lua_newuserdatauv(L, size ? size : 1, 0);
    size_t got  = f.read((uint8_t*)buf, size);
    f.close();
    if (got != size) {
        lua_pop(L, 1);                       // drop scratch buffer
        lua_pushfstring(L, "read error on %s (%d/%d bytes)",
                        path, (int)got, (int)size);
        return LUA_ERRFILE;
    }
    int rc = luaL_loadbuffer(L, buf, size, path);
    lua_remove(L, -2);                       // drop scratch buffer, keep result
    return rc;
}
#else
// Host builds (tests) read real files from the working directory.
static int loadChunk(lua_State* L, const char* path) {
    return luaL_loadfile(L, path);
}
#endif

bool LightAir_LuaGame::load(const char* path) {
    unload();

    // Claim a trampoline slot once; reloads keep it (the menu realizes
    // different games on the same instance as the user browses).
    if (_slotIdx == 0xFF) {
        if (s_instanceCount >= LuaDefaults::MAX_LUA_GAMES) {
            Log.errorln("LuaGame: instance pool full");
            return false;
        }
        _slotIdx = s_instanceCount;
        s_instances[s_instanceCount++] = this;
    }
    if (!_engine.begin()) {
        Log.errorln("LuaGame: lua_State allocation failed");
        return false;
    }

    for (uint8_t i = 0; i < LuaDefaults::MAX_STATES; i++) {
        _msgTabRef[i] = LUA_NOREF;
        _updateRef[i] = LUA_NOREF;
    }
    for (uint8_t i = 0; i < LuaDefaults::MAX_RULES; i++) {
        _ruleWhenRef[i] = LUA_NOREF;
        _ruleActRef[i]  = LUA_NOREF;
    }
    for (uint8_t i = 0; i < 3; i++) _pktUdRef[i] = LUA_NOREF;

    registerKernel();

    lua_State* L = _engine.L();

    // Compile + run the chunk (its top-level code executes here).
    if (loadChunk(L, path) != LUA_OK) {
        Log.errorln("LuaGame: %s: %s", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        _engine.end();
        return false;
    }
    if (!_engine.pcall(0, 1)) {
        Log.errorln("LuaGame: %s failed to run", path);
        _engine.end();
        return false;
    }
    if (!lua_istable(L, -1)) {
        Log.errorln("LuaGame: %s did not return a table", path);
        lua_settop(L, 0);
        _engine.end();
        return false;
    }

    // Walk + validate in protected mode (kBeginTramps[_slotIdx] is wired
    // by loadFromTable from the slot claimed above).
    lua_pushcfunction(L, loaderBody);
    lua_pushlightuserdata(L, this);
    lua_pushvalue(L, -3);                                  // the game table
    if (!_engine.pcall(2, 0)) {
        Log.errorln("LuaGame: %s rejected: %s", path, _engine.lastError());
        lua_settop(L, 0);
        _engine.end();
        return false;
    }

    _gameRef = luaL_ref(L, LUA_REGISTRYINDEX);             // keep the table alive
    _loaded = true;
    Log.infoln("LuaGame: loaded '%s' (typeId 0x%x) from %s", _name, _game.typeId, path);
    return true;
}

bool LightAir_LuaGame::peekManifest(const char* path, char* nameOut,
                                    size_t nameCap, uint16_t* typeIdOut) {
    unload();
    if (!_engine.begin()) return false;
    for (uint8_t i = 0; i < 3; i++) _pktUdRef[i] = LUA_NOREF;
    registerKernel();                       // chunks index `la` at top level

    lua_State* L = _engine.L();
    bool ok = false;

    // Each failure mode reports itself: they all mean "no game here", but
    // a missing file, a syntax error and a bad return value need different
    // fixes, and only the first two leave a message on the Lua stack
    // (pcall() pops its own error into lastError()).
    int rc = loadChunk(L, path);
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        Log.errorln("LuaGame: cannot load %s: %s", path, err ? err : "(no message)");
        lua_settop(L, 0);
    } else if (!_engine.pcall(0, 1)) {
        Log.errorln("LuaGame: %s failed to run: %s", path, _engine.lastError());
        lua_settop(L, 0);
    } else if (!lua_istable(L, -1)) {
        Log.errorln("LuaGame: %s did not return a table", path);
        lua_settop(L, 0);
    } else {
        lua_getfield(L, -1, "api");
        bool apiOk = lua_isinteger(L, -1) &&
                     lua_tointeger(L, -1) == LuaDefaults::API_VERSION;
        lua_pop(L, 1);
        lua_getfield(L, -1, "type_id");
        bool idOk = lua_isinteger(L, -1);
        if (idOk && typeIdOut) *typeIdOut = (uint16_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "name");
        bool nameOk = lua_isstring(L, -1);
        if (nameOk && nameOut) {
            strncpy(nameOut, lua_tostring(L, -1), nameCap - 1);
            nameOut[nameCap - 1] = 0;
        }
        lua_pop(L, 1);
        ok = apiOk && idOk && nameOk;
        if (!ok)
            Log.errorln("LuaGame: %s has a bad manifest (api/type_id/name)", path);
    }
    _engine.end();
    return ok;
}

void LightAir_LuaGame::unload() {
    // The pool slot (if claimed) survives an unload so the instance can
    // reload a different file; closing the engine frees all Lua memory
    // and invalidates every registry ref with it.
    if (g_luaCtx.active == this) g_luaCtx.active = nullptr;
    _loaded = false;
    _engine.end();
    _slotCount = 0;
    _countdownCount = 0;
    _progCount = 0;
    _stateMax = 0;
    memset(&_game, 0, sizeof(_game));
}
