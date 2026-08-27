// ----------------------------------------------------------------
// LightAir_LuaKernel.cpp — the `la` verb kernel and the `vars` /
// `pkt` proxies: everything a game file can call.
//
// One of the three LightAir_LuaGame translation units (see
// LightAir_LuaGameInternal.h for the map).  The verbs read the
// callback context (g_luaCtx) that the trampolines in
// LightAir_LuaGame.cpp populate before dispatching into Lua.
//
// To add a new la.* verb, follow docs/lua-embedding-guide.md §"Adding
// a verb": write the lua_CFunction here, add it to kVerbs in
// registerKernel(), document it in docs/lua-games-design.md, and
// cover it in test/host/test_luagame.cpp.  Mind the admission rule
// (design doc §"API layering"): only capabilities belong here,
// never game policies.
// ----------------------------------------------------------------
#include "LightAir_LuaGame.h"
#include "LightAir_LuaGameInternal.h"
#include <Arduino.h>
#include <string.h>

#include "../game/LightAir_GameRunner.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../enlight/Enlight.h"

#ifdef ESP32
#include <FS.h>
#include <LittleFS.h>
#endif

// Enlight singleton, owned by the sketch (player path only).
extern Enlight* enlightPtr;

/* =========================================================
 *   NAME TABLES
 * ========================================================= */

// Aligned with LightAir_UICtrl::UIEvent.
static const char* const kUIEventNames[] = {
    "Enlight", "Lit", "Taken", "GotLit", "Immune", "Friend", "AlreadyDown",
    "Down", "Up", "EndGame", "GameStart", "FlagGain", "FlagTaken",
    "FlagReturn", "ControlGain", "ControlLoss", "RoleChange", "Stop",
    "Bonus", "Malus", "Special1", "Special2",
    "Custom1", "Custom2", "Custom3", "Custom4",
};
static const uint8_t kUIEventCount = sizeof(kUIEventNames) / sizeof(*kUIEventNames);

// la.msg — the RadioMsg registry exposed to game files.
static const NamedU8 kMsgConsts[] = {
    { "LIT",           RadioMsg::MSG_LIT },
    { "SCORE_COLLECT", RadioMsg::MSG_SCORE_COLLECT },
    { "POINT_REPORT",  RadioMsg::MSG_POINT_REPORT },
    { "FLAG_EVENT",    RadioMsg::MSG_FLAG_EVENT },
    { "CP_BEACON",     RadioMsg::MSG_CP_BEACON },
    { "CP_SCORE",      RadioMsg::MSG_CP_SCORE },
    { "BASE_BEACON",   RadioMsg::MSG_BASE_BEACON },
    { "FLAG_BEACON",   RadioMsg::MSG_FLAG_BEACON },
    { "BONUS_BEACON",  RadioMsg::MSG_BONUS_BEACON },
    { "MALUS_BEACON",  RadioMsg::MSG_MALUS_BEACON },
};

/* =========================================================
 *   PACKET PROXY  (reusable userdata; zero alloc per event)
 * ========================================================= */

int LightAir_LuaGame::l_pkt_byte(lua_State* L) {
    uint8_t which = *(uint8_t*)luaL_checkudata(L, 1, "LA_PKT");
    lua_Integer i = luaL_checkinteger(L, 2);        // 1-based
    const RadioPacket* p = g_luaCtx.pkts[which];
    if (!p || i < 1 || i > p->payloadLen) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, p->payload[i - 1]);
    return 1;
}

int LightAir_LuaGame::l_pkt_index(lua_State* L) {
    LightAir_LuaGame* g = self(L);
    uint8_t which = *(uint8_t*)luaL_checkudata(L, 1, "LA_PKT");
    const char* k = luaL_checkstring(L, 2);
    const RadioPacket* p = g_luaCtx.pkts[which];
    if (strcmp(k, "byte") == 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g->_pktByteFnRef);
        return 1;
    }
    if (!p) return luaL_error(L, "pkt used outside its handler");
    if      (strcmp(k, "sender") == 0) lua_pushinteger(L, p->senderId);
    else if (strcmp(k, "team")   == 0) lua_pushinteger(L, p->team);
    else if (strcmp(k, "role")   == 0) lua_pushinteger(L, p->role);
    else if (strcmp(k, "msg")    == 0) lua_pushinteger(L, p->msgType);
    else if (strcmp(k, "len")    == 0) lua_pushinteger(L, p->payloadLen);
    else if (strcmp(k, "rssi")   == 0) lua_pushinteger(L, g_luaCtx.pktRssi[which]);
    else lua_pushnil(L);
    return 1;
}

/* =========================================================
 *   VARS PROXY  (blackboard slots)
 * ========================================================= */

LightAir_LuaGame* LightAir_LuaGame::self(lua_State* L) {
    return (LightAir_LuaGame*)lua_touserdata(L, lua_upvalueindex(1));
}

int LightAir_LuaGame::l_vars_index(lua_State* L) {
    LightAir_LuaGame* g = self(L);
    const char* k = luaL_checkstring(L, 2);
    int i = g->findSlot(k);
    if (i < 0) return luaL_error(L, "unknown var '%s'", k);
    if (g->_slots[i].isText) lua_pushstring(L, g->_slots[i].text);
    else                     lua_pushinteger(L, g->_slots[i].val);
    return 1;
}

int LightAir_LuaGame::l_vars_newindex(lua_State* L) {
    LightAir_LuaGame* g = self(L);
    const char* k = luaL_checkstring(L, 2);
    int i = g->findSlot(k);
    if (i < 0) return luaL_error(L, "unknown var '%s'", k);
    if (g->_slots[i].isText) {
        const char* v = luaL_checkstring(L, 3);
        strncpy(g->_slots[i].text, v, LuaDefaults::MAX_TEXT_LEN - 1);
        g->_slots[i].text[LuaDefaults::MAX_TEXT_LEN - 1] = 0;
    } else {
        g->_slots[i].val = (int)luaL_checkinteger(L, 3);
    }
    return 0;
}

/* =========================================================
 *   la VERBS
 * ========================================================= */

static int l_now(lua_State* L)      { lua_pushinteger(L, (lua_Integer)(int32_t)millis()); return 1; }
static int l_my_id(lua_State* L)    { lua_pushinteger(L, g_luaCtx.radio ? g_luaCtx.radio->playerId() : 0); return 1; }
static int l_my_team(lua_State* L)  {
    lua_pushinteger(L, (g_luaCtx.runner && g_luaCtx.radio)
                       ? g_luaCtx.runner->teamOf(g_luaCtx.radio->playerId()) : 0xFF);
    return 1;
}
static int l_team_of(lua_State* L)  {
    lua_pushinteger(L, g_luaCtx.runner
                       ? g_luaCtx.runner->teamOf((uint8_t)luaL_checkinteger(L, 1)) : 0xFF);
    return 1;
}
static int l_player_count(lua_State* L) {
    lua_pushinteger(L, g_luaCtx.runner ? g_luaCtx.runner->rosterCount() : 0);
    return 1;
}
static int l_player_short(lua_State* L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id < 0 || id >= PlayerDefs::MAX_PLAYER_ID) id = 0;
    lua_pushstring(L, PlayerDefs::playerShort[id]);
    return 1;
}
// Team label ("O", "X", …) from the one table in config.h, so a game file
// never spells the names out for itself.  Out-of-range clamps to team 0.
static int l_team_short(lua_State* L) {
    lua_Integer t = luaL_checkinteger(L, 1);
    if (t < 0 || t >= TeamColors::kCount) t = 0;
    lua_pushstring(L, TeamNames::forTeam((uint8_t)t));
    return 1;
}
static int l_totem_for_role(lua_State* L) {
    int roleId;
    if (lua_type(L, 1) == LUA_TSTRING) {
        roleId = lookupTotemRole(lua_tostring(L, 1));
        if (roleId < 0) return luaL_error(L, "unknown totem role '%s'", lua_tostring(L, 1));
    } else {
        roleId = (int)luaL_checkinteger(L, 1);
    }
    uint8_t idx = (uint8_t)luaL_optinteger(L, 2, 0);
    lua_pushinteger(L, g_luaCtx.runner
                       ? g_luaCtx.runner->totemIdForRole((uint8_t)roleId, idx) : 0);
    return 1;
}
// ---- inputs ----
static const InputReport::ButtonEntry* findButton(uint8_t id) {
    if (!g_luaCtx.inputs) return nullptr;
    for (uint8_t i = 0; i < g_luaCtx.inputs->buttonCount; i++)
        if (g_luaCtx.inputs->buttons[i].id == id) return &g_luaCtx.inputs->buttons[i];
    return nullptr;
}
static int l_trigger_down(lua_State* L) {
    uint8_t id = (uint8_t)(luaL_optinteger(L, 1, 1) - 1);   // la counts from 1
    const InputReport::ButtonEntry* b = findButton(id);
    lua_pushboolean(L, b && (b->state == ButtonState::PRESSED ||
                             b->state == ButtonState::HELD));
    return 1;
}
static int l_trigger_state(lua_State* L) {
    uint8_t id = (uint8_t)(luaL_optinteger(L, 1, 1) - 1);
    const InputReport::ButtonEntry* b = findButton(id);
    static const char* const kNames[] = { "off", "pressed", "held", "released", "released_held" };
    lua_pushstring(L, b ? kNames[(uint8_t)b->state] : "off");
    return 1;
}

// ---- Enlight optics ----
static int l_shine(lua_State* L) {
    lua_pushboolean(L, enlightPtr ? enlightPtr->run() : false);
    return 1;
}
static int l_shine_lit(lua_State* L) {
    if (!enlightPtr) { lua_pushnil(L); return 1; }
    EnlightResult r = enlightPtr->poll();
    if (r.status == EnlightStatus::PLAYER_HIT) lua_pushinteger(L, r.id);
    else lua_pushnil(L);
    return 1;
}
static int l_shine_ms(lua_State* L) {
    lua_pushinteger(L, enlightPtr ? (lua_Integer)enlightPtr->cycleTime() : 0);
    return 1;
}
static int l_shine_config(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!enlightPtr) return 0;
    lua_getfield(L, 1, "cooldown_ms");
    if (lua_isinteger(L, -1)) enlightPtr->setCooldown(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "reps");
    if (lua_isinteger(L, -1)) enlightPtr->setRepetitions((uint32_t)lua_tointeger(L, -1));
    lua_pop(L, 1);
    return 0;
}

// ---- radio out ----
static int collectPayload(lua_State* L, int first, uint8_t* buf, uint8_t max) {
    int top = lua_gettop(L);
    uint8_t n = 0;
    for (int i = first; i <= top && n < max; i++)
        buf[n++] = (uint8_t)luaL_checkinteger(L, i);
    return n;
}
static int l_send(lua_State* L) {
    uint8_t target = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t msg    = (uint8_t)luaL_checkinteger(L, 2);
    uint8_t pl[16];
    uint8_t n = collectPayload(L, 3, pl, sizeof(pl));
    if (g_luaCtx.out)        g_luaCtx.out->radio.sendTo(target, msg, pl, n);
    else if (g_luaCtx.radio) g_luaCtx.radio->sendTo(target, msg, pl, n);
    return 0;
}
static int doBroadcast(lua_State* L, uint8_t resend) {
    uint8_t msg = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t pl[16];
    uint8_t n = collectPayload(L, 2, pl, sizeof(pl));
    if (g_luaCtx.out)        g_luaCtx.out->radio.broadcast(msg, pl, n, resend);
    else if (g_luaCtx.radio) g_luaCtx.radio->broadcast(msg, pl, n, resend);
    return 0;
}
static int l_broadcast(lua_State* L)       { return doBroadcast(L, 0); }
static int l_broadcast_relay(lua_State* L) { return doBroadcast(L, 2); }

// ---- UI out ----
static int l_ui(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (uint8_t i = 0; i < kUIEventCount; i++) {
        if (strcmp(kUIEventNames[i], name) == 0) {
            LightAir_UICtrl::UIEvent ev = (LightAir_UICtrl::UIEvent)i;
            if (g_luaCtx.out)     g_luaCtx.out->ui.trigger(ev);
            else if (g_luaCtx.ui) g_luaCtx.ui->trigger(ev);
            return 0;
        }
    }
    return luaL_error(L, "unknown UI event '%s'", name);
}
static int l_ui_enlight(lua_State* L) {
    uint16_t ms = (uint16_t)luaL_checkinteger(L, 1);
    if (g_luaCtx.out)     g_luaCtx.out->ui.triggerEnlight(ms);
    else if (g_luaCtx.ui) g_luaCtx.ui->triggerEnlight(ms);
    return 0;
}
static int l_show(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    uint32_t ms = (uint32_t)luaL_optinteger(L, 2, 0);
    if (g_luaCtx.disp) g_luaCtx.disp->showMessage(text, ms);
    return 0;
}
static int l_clear_tray(lua_State* L) {
    (void)L;
    if (g_luaCtx.disp) g_luaCtx.disp->clearTray();
    return 0;
}
static int l_background(lua_State* L) {
    if (!g_luaCtx.ui) return 0;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        g_luaCtx.ui->clearBackground();
        return 0;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    LightAir_UICtrl::UIAction a = {};
    lua_getfield(L, 1, "priority");
    a.priority = (uint8_t)luaL_optinteger(L, -1, 1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "steps");
    luaL_checktype(L, -1, LUA_TTABLE);
    int nSteps = (int)lua_rawlen(L, -1);
    if (nSteps > 4) nSteps = 4;
    for (int i = 1; i <= nSteps; i++) {
        lua_rawgeti(L, -1, i);                    // step table
        lua_getfield(L, -1, "ms");
        a.durations[i - 1] = (uint16_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "freq");
        a.soundFreqs[i - 1] = (uint16_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "vib");
        a.vibIntensity[i - 1] = (uint8_t)luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_getfield(L, -1, "rgb");
        if (lua_istable(L, -1)) {
            for (int c = 1; c <= 3; c++) {
                lua_rawgeti(L, -1, c);
                a.rgbColors[i - 1][c - 1] = (uint8_t)luaL_optinteger(L, -1, 0);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 2);                            // rgb (or nil) + step table
    }
    a.stepCount = (uint8_t)nSteps;
    lua_pop(L, 1);                                // steps table
    g_luaCtx.ui->setBackground(a);
    return 0;
}

// ---- library loader ----
int LightAir_LuaGame::l_lib(lua_State* L) {
    LightAir_LuaGame* g = self(L);
    const char* name = luaL_checkstring(L, 1);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g->_libCacheRef);   // cache
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;                                          // cached result
    }
    lua_pop(L, 1);                                         // nil; keep cache

#ifdef ESP32
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.lua", LuaDefaults::LIB_DIR, name);
    File f = LittleFS.open(path, "r");
    if (!f) return luaL_error(L, "library '%s' not found", name);
    size_t size = f.size();
    char* buf = (char*)lua_newuserdatauv(L, size, 0);      // scratch, GC-managed
    size_t got = f.read((uint8_t*)buf, size);
    f.close();
    if (got != size) return luaL_error(L, "library '%s' read error", name);
    if (luaL_loadbuffer(L, buf, size, name) != LUA_OK)
        return lua_error(L);                               // message already on top
    lua_remove(L, -2);                                     // drop scratch buffer
    lua_call(L, 0, 1);                                     // run chunk -> module
#else
    // Host builds (tests): read from the working directory.
    char path[128];
    snprintf(path, sizeof(path), "games/lib/%s.lua", name);
    if (luaL_loadfile(L, path) != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 1);
#endif

    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);                             // cache[name] = module
    lua_remove(L, -2);                                     // drop cache table
    return 1;
}

/* =========================================================
 *   KERNEL REGISTRATION
 * ========================================================= */

void LightAir_LuaGame::registerKernel() {
    lua_State* L = _engine.L();

    lua_newtable(L);                                       // la

    struct Verb { const char* name; lua_CFunction fn; };
    static const Verb kVerbs[] = {
        { "now", l_now }, { "my_id", l_my_id }, { "my_team", l_my_team },
        { "team_of", l_team_of }, { "player_count", l_player_count },
        { "player_short", l_player_short }, { "team_short", l_team_short },
        { "totem_for_role", l_totem_for_role },
        { "trigger_down", l_trigger_down }, { "trigger_state", l_trigger_state },
        { "shine", l_shine }, { "shine_lit", l_shine_lit },
        { "shine_ms", l_shine_ms }, { "shine_config", l_shine_config },
        { "send", l_send }, { "broadcast", l_broadcast },
        { "broadcast_relay", l_broadcast_relay },
        { "ui", l_ui }, { "ui_enlight", l_ui_enlight },
        { "show", l_show }, { "clear_tray", l_clear_tray },
        { "background", l_background },
        { "lib", LightAir_LuaGame::l_lib },
    };
    for (const Verb& v : kVerbs) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, v.fn, 1);
        lua_setfield(L, -2, v.name);
    }

    // la.state — reads this instance's live state byte.
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, [](lua_State* LL) -> int {
        LightAir_LuaGame* g = self(LL);
        lua_pushinteger(LL, g->_state);
        return 1;
    }, 1);
    lua_setfield(L, -2, "state");

    // la.msg
    lua_newtable(L);
    for (const NamedU8& m : kMsgConsts) {
        lua_pushinteger(L, m.val);
        lua_setfield(L, -2, m.name);
    }
    lua_setfield(L, -2, "msg");

    // la.flag_event
    lua_newtable(L);
    lua_pushinteger(L, FlagEvent::TAKEN);   lua_setfield(L, -2, "TAKEN");
    lua_pushinteger(L, FlagEvent::DROPPED); lua_setfield(L, -2, "DROPPED");
    lua_pushinteger(L, FlagEvent::SCORED);  lua_setfield(L, -2, "SCORED");
    lua_setfield(L, -2, "flag_event");

    // la.colors.team / la.colors.player
    lua_newtable(L);                                       // colors
    lua_newtable(L);                                       // team
    for (uint8_t t = 0; t < TeamColors::kCount; t++) {
        lua_createtable(L, 3, 0);
        for (int c = 0; c < 3; c++) {
            lua_pushinteger(L, TeamColors::kColors[t][c]);
            lua_rawseti(L, -2, c + 1);
        }
        lua_rawseti(L, -2, t);                             // 0-based team keys
    }
    lua_setfield(L, -2, "team");
    lua_newtable(L);                                       // player
    for (uint8_t p = 0; p < PlayerDefs::MAX_PLAYER_ID; p++) {
        lua_createtable(L, 3, 0);
        for (int c = 0; c < 3; c++) {
            lua_pushinteger(L, PlayerColors::kColors[p][c]);
            lua_rawseti(L, -2, c + 1);
        }
        lua_rawseti(L, -2, p);
    }
    lua_setfield(L, -2, "player");
    lua_setfield(L, -2, "colors");

    // la.rhythm
    lua_newtable(L);
    for (uint8_t t = 0; t < TeamLedRhythm::kCount; t++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, TeamLedRhythm::kTable[t].periodMs);
        lua_setfield(L, -2, "period");
        lua_pushinteger(L, TeamLedRhythm::kTable[t].pulseCount);
        lua_setfield(L, -2, "pulses");
        lua_rawseti(L, -2, t);
    }
    lua_setfield(L, -2, "rhythm");

    lua_setglobal(L, "la");

    // ---- vars proxy (empty table, metamethods carry the instance) ----
    lua_newtable(L);                                       // proxy
    lua_createtable(L, 0, 2);                              // metatable
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, l_vars_index, 1);
    lua_setfield(L, -2, "__index");
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, l_vars_newindex, 1);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "vars");
    _varsProxyRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // ---- packet proxies ----
    if (luaL_newmetatable(L, "LA_PKT")) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, l_pkt_index, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t* ud = (uint8_t*)lua_newuserdatauv(L, 1, 0);
        *ud = i;
        luaL_setmetatable(L, "LA_PKT");
        _pktUdRef[i] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pushcfunction(L, l_pkt_byte);
    _pktByteFnRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // ---- la.lib cache ----
    lua_newtable(L);
    _libCacheRef = luaL_ref(L, LUA_REGISTRYINDEX);
}
