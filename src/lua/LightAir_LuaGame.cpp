#include "LightAir_LuaGame.h"
#include <Arduino.h>
#include <ArduinoLog.h>
#include <string.h>

#include "../game/LightAir_GameRunner.h"
#include "../radio/LightAir_Radio.h"
#include "../ui/player/display/LightAir_DisplayCtrl.h"
#include "../ui/player/LightAir_UICtrl.h"
#include "../ui/totem/LightAir_TotemUIOutput.h"
#include "../enlight/Enlight.h"
#include "../totem/TotemRoleIds.h"

#ifdef ESP32
#include <FS.h>
#include <LittleFS.h>
#include <nvs.h>
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

// Aligned with TotemUIEvent.
static const char* const kAnimNames[] = {
    "Respawn", "FlagTaken", "FlagReturn", "Bonus", "Malus", "Roster",
    "Idle", "BaseIdle", "CPIdle", "FlagIdle", "BonusIdle", "MalusIdle",
    "FlagMissing", "Control", "ControlContest",
    "Custom1", "Custom2", "Custom3", "Custom4",
};
static const uint8_t kAnimCount = sizeof(kAnimNames) / sizeof(*kAnimNames);

struct NamedU8 { const char* name; uint8_t val; };

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

static int lookupName(const NamedU8* tab, uint8_t n, const char* name) {
    for (uint8_t i = 0; i < n; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].val;
    return -1;
}
#define LOOKUP(tab, name) lookupName(tab, sizeof(tab) / sizeof(*tab), name)

/* =========================================================
 *   PROCESS-WIDE CONTEXT
 *   (one game is active at a time; set by the trampolines)
 * ========================================================= */

static LightAir_LuaGame* s_instances[LuaDefaults::MAX_LUA_GAMES] = {};
static uint8_t           s_instanceCount = 0;
static LightAir_LuaGame* s_active = nullptr;

static GameOutput*                s_out    = nullptr;   // valid inside callbacks
static LightAir_DisplayCtrl*      s_disp   = nullptr;
static LightAir_UICtrl*           s_ui     = nullptr;
static LightAir_Radio*            s_radio  = nullptr;
static const LightAir_GameRunner* s_runner = nullptr;
static const InputReport*         s_inputs = nullptr;

// Packet proxies: 0 = incoming request, 1 = reply, 2 = original.
static const RadioPacket* s_pkts[3]    = {};
static int8_t             s_pktRssi[3] = {};

uint8_t LightAir_LuaGame::instanceCount()            { return s_instanceCount; }
LightAir_LuaGame* LightAir_LuaGame::instance(uint8_t i) {
    return (i < s_instanceCount) ? s_instances[i] : nullptr;
}

/* =========================================================
 *   PACKET PROXY  (reusable userdata; zero alloc per event)
 * ========================================================= */

int LightAir_LuaGame::l_pkt_byte(lua_State* L) {
    uint8_t which = *(uint8_t*)luaL_checkudata(L, 1, "LA_PKT");
    lua_Integer i = luaL_checkinteger(L, 2);        // 1-based
    const RadioPacket* p = s_pkts[which];
    if (!p || i < 1 || i > p->payloadLen) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, p->payload[i - 1]);
    return 1;
}

int LightAir_LuaGame::l_pkt_index(lua_State* L) {
    LightAir_LuaGame* g = self(L);
    uint8_t which = *(uint8_t*)luaL_checkudata(L, 1, "LA_PKT");
    const char* k = luaL_checkstring(L, 2);
    const RadioPacket* p = s_pkts[which];
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
    else if (strcmp(k, "rssi")   == 0) lua_pushinteger(L, s_pktRssi[which]);
    else lua_pushnil(L);
    return 1;
}

void LightAir_LuaGame::pushPkt(uint8_t which, const RadioPacket* pkt, int8_t rssi) {
    s_pkts[which]    = pkt;
    s_pktRssi[which] = rssi;
    lua_rawgeti(_engine.L(), LUA_REGISTRYINDEX, _pktUdRef[which]);
}

/* =========================================================
 *   VARS PROXY  (blackboard slots)
 * ========================================================= */

LightAir_LuaGame* LightAir_LuaGame::self(lua_State* L) {
    return (LightAir_LuaGame*)lua_touserdata(L, lua_upvalueindex(1));
}

int LightAir_LuaGame::findSlot(const char* id) const {
    for (uint8_t i = 0; i < _slotCount; i++)
        if (strcmp(_slots[i].id, id) == 0) return i;
    return -1;
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

void LightAir_LuaGame::pushVarsProxy() {
    lua_rawgeti(_engine.L(), LUA_REGISTRYINDEX, _varsProxyRef);
}

/* =========================================================
 *   la VERBS
 * ========================================================= */

static int l_now(lua_State* L)      { lua_pushinteger(L, (lua_Integer)(int32_t)millis()); return 1; }
static int l_my_id(lua_State* L)    { lua_pushinteger(L, s_radio ? s_radio->playerId() : 0); return 1; }
static int l_my_team(lua_State* L)  {
    lua_pushinteger(L, (s_runner && s_radio) ? s_runner->teamOf(s_radio->playerId()) : 0xFF);
    return 1;
}
static int l_team_of(lua_State* L)  {
    lua_pushinteger(L, s_runner ? s_runner->teamOf((uint8_t)luaL_checkinteger(L, 1)) : 0xFF);
    return 1;
}
static int l_player_count(lua_State* L) {
    lua_pushinteger(L, s_runner ? s_runner->rosterCount() : 0);
    return 1;
}
static int l_player_short(lua_State* L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id < 0 || id >= PlayerDefs::MAX_PLAYER_ID) id = 0;
    lua_pushstring(L, PlayerDefs::playerShort[id]);
    return 1;
}
static int l_totem_for_role(lua_State* L) {
    int roleId;
    if (lua_type(L, 1) == LUA_TSTRING) {
        roleId = LOOKUP(kRoles, lua_tostring(L, 1));
        if (roleId < 0) return luaL_error(L, "unknown totem role '%s'", lua_tostring(L, 1));
    } else {
        roleId = (int)luaL_checkinteger(L, 1);
    }
    uint8_t idx = (uint8_t)luaL_optinteger(L, 2, 0);
    lua_pushinteger(L, s_runner ? s_runner->totemIdForRole((uint8_t)roleId, idx) : 0);
    return 1;
}
// ---- inputs ----
static const InputReport::ButtonEntry* findButton(uint8_t id) {
    if (!s_inputs) return nullptr;
    for (uint8_t i = 0; i < s_inputs->buttonCount; i++)
        if (s_inputs->buttons[i].id == id) return &s_inputs->buttons[i];
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
    if (s_out)        s_out->radio.sendTo(target, msg, pl, n);
    else if (s_radio) s_radio->sendTo(target, msg, pl, n);
    return 0;
}
static int doBroadcast(lua_State* L, uint8_t resend) {
    uint8_t msg = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t pl[16];
    uint8_t n = collectPayload(L, 2, pl, sizeof(pl));
    if (s_out)        s_out->radio.broadcast(msg, pl, n, resend);
    else if (s_radio) s_radio->broadcast(msg, pl, n, resend);
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
            if (s_out)     s_out->ui.trigger(ev);
            else if (s_ui) s_ui->trigger(ev);
            return 0;
        }
    }
    return luaL_error(L, "unknown UI event '%s'", name);
}
static int l_ui_enlight(lua_State* L) {
    uint16_t ms = (uint16_t)luaL_checkinteger(L, 1);
    if (s_out)     s_out->ui.triggerEnlight(ms);
    else if (s_ui) s_ui->triggerEnlight(ms);
    return 0;
}
static int l_show(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    uint32_t ms = (uint32_t)luaL_optinteger(L, 2, 0);
    if (s_disp) s_disp->showMessage(text, ms);
    return 0;
}
static int l_clear_tray(lua_State* L) {
    (void)L;
    if (s_disp) s_disp->clearTray();
    return 0;
}
static int l_background(lua_State* L) {
    if (!s_ui) return 0;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        s_ui->clearBackground();
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
    s_ui->setBackground(a);
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
        { "player_short", l_player_short }, { "totem_for_role", l_totem_for_role },
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

/* =========================================================
 *   TRAMPOLINES
 * ========================================================= */

struct LuaGameTramps {
    template <uint8_t N>
    static void begin(LightAir_DisplayCtrl& d, LightAir_Radio& r,
                      LightAir_UICtrl* ui, const LightAir_GameRunner& gr) {
        if (N < s_instanceCount && s_instances[N]) {
            s_active = s_instances[N];
            s_active->doBegin(d, r, ui, gr);
        }
    }
    template <uint8_t I>
    static bool ruleWhen(const InputReport& inp, const RadioReport&) {
        s_inputs = &inp;
        s_out    = nullptr;                    // conditions: direct-output fallback
        return s_active ? s_active->doRuleWhen(I) : false;
    }
    template <uint8_t I>
    static void ruleAct(LightAir_DisplayCtrl& d, GameOutput& out) {
        s_disp = &d;
        s_out  = &out;
        if (s_active) s_active->doRuleAct(I);
    }
    static void behavior(const InputReport& inp, const RadioReport&,
                         LightAir_DisplayCtrl& d, GameOutput& out) {
        s_inputs = &inp;
        s_disp   = &d;
        s_out    = &out;
        if (s_active) s_active->doBehavior();
    }
    static void message(const RadioPacket& pkt, LightAir_DisplayCtrl& d,
                        GameOutput& out) {
        s_disp = &d;
        s_out  = &out;
        if (s_active) s_active->doMessage(pkt, out);
    }
    static void reply(const RadioPacket& rep, const RadioPacket& orig,
                      LightAir_DisplayCtrl& d, GameOutput& out) {
        s_disp = &d;
        s_out  = &out;
        if (s_active) s_active->doReply(rep, orig);
    }
    static void timeout(const RadioPacket&, const RadioPacket& orig,
                        LightAir_DisplayCtrl& d, GameOutput& out) {
        s_disp = &d;
        s_out  = &out;
        if (s_active) s_active->doTimeout(orig);
    }
    static void score(const ScoreTable& t, LightAir_DisplayCtrl& d) {
        s_disp = &d;
        s_out  = nullptr;
        if (s_active) s_active->doScoreAnnounce(t);
    }
    static void end(LightAir_DisplayCtrl& d) {
        s_disp = &d;
        s_out  = nullptr;
        if (s_active) s_active->doEnd();
    }
};

typedef void (*BeginFn)(LightAir_DisplayCtrl&, LightAir_Radio&,
                        LightAir_UICtrl*, const LightAir_GameRunner&);
typedef bool (*WhenFn)(const InputReport&, const RadioReport&);
typedef void (*ActFn)(LightAir_DisplayCtrl&, GameOutput&);

#define B(n) &LuaGameTramps::begin<n>
static const BeginFn kBeginTramps[LuaDefaults::MAX_LUA_GAMES] = {
    B(0), B(1), B(2), B(3), B(4),  B(5),  B(6), B(7),
    B(8), B(9), B(10), B(11)
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
    return s_active ? s_active->patchedProgram(roleId) : nullptr;
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
    if (s_disp && (now - _lastFaultNoticeAt >= kFaultNoticeCooldownMs ||
                   _lastFaultNoticeAt == 0)) {
        _lastFaultNoticeAt = now;
        s_disp->showMessage("Lua error!", 5000);
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
    s_disp   = &d;
    s_radio  = &r;
    s_ui     = ui;
    s_runner = &gr;
    s_out    = nullptr;
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

void LightAir_LuaGame::doMessage(const RadioPacket& pkt, GameOutput& out) {
    uint8_t sub = 0;
    if (_state < LuaDefaults::MAX_STATES && _msgTabRef[_state] != LUA_NOREF) {
        lua_State* L = _engine.L();
        lua_rawgeti(L, LUA_REGISTRYINDEX, _msgTabRef[_state]);
        lua_rawgeti(L, -1, pkt.msgType);
        lua_remove(L, -2);                                 // drop the table
        if (lua_isfunction(L, -1)) {
            pushVarsProxy();
            pushPkt(0, &pkt, 0);
            if (!_engine.pcall(2, 1)) {
                luaFault(FaultSite::Message);
            } else {
                if (lua_isinteger(L, -1)) sub = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
            s_pkts[0] = nullptr;
        } else {
            lua_pop(L, 1);                                 // non-function entry
        }
    }
    // The synthesized rule uses DYNAMIC_REPLY, so the runner skips its
    // auto-reply; sending it here preserves the wire contract.
    out.radio.reply(pkt, sub);
}

void LightAir_LuaGame::doReply(const RadioPacket& reply, const RadioPacket& orig) {
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
    pushPkt(1, &reply, 0);
    pushPkt(2, &orig, 0);
    if (!_engine.pcall(3, 0)) luaFault(FaultSite::Reply);
    s_pkts[1] = s_pkts[2] = nullptr;
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
    s_pkts[2] = nullptr;
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

// Wire constants shared with the reference encoder and the TotemVM
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

static int cmpCode(lua_State* L, const char* op) {
    static const NamedU8 kCmp[] = {
        { "==", 0 }, { "~=", 1 }, { "<", 2 }, { ">=", 3 }, { "<=", 4 }, { ">", 5 },
    };
    int c = LOOKUP(kCmp, op);
    if (c < 0) luaL_error(L, "totem: bad comparator '%s'", op);
    return c;
}

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
// dsScale: encode a {"cfg"} placeholder as seconds→deciseconds patch site.
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

/* ---------------------------------------------------------
 *   Table-walking helpers
 * --------------------------------------------------------- */

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
            if (_slots[slot].isText)
                _monitorVars[monitorCount] = MonitorVar::Str(
                    _slots[slot].id, _slots[slot].text, mask, (IconType)icon, col, row);
            else
                _monitorVars[monitorCount] = MonitorVar::Int(
                    _slots[slot].id, &_slots[slot].val, mask, (IconType)icon, col, row);
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
            int roleId = LOOKUP(kRoles, roleName);
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
            int roleId = LOOKUP(kRoles, roleName);
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

bool LightAir_LuaGame::load(const char* path) {
    unload();

    if (s_instanceCount >= LuaDefaults::MAX_LUA_GAMES) {
        Log.errorln("LuaGame: instance pool full");
        return false;
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
    if (luaL_loadfile(L, path) != LUA_OK) {
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

    // Claim the registry slot (trampoline identity) before walking the
    // table — loadFromTable wires kBeginTramps[_slotIdx].
    _slotIdx = s_instanceCount;

    // Walk + validate in protected mode.
    lua_pushcfunction(L, loaderBody);
    lua_pushlightuserdata(L, this);
    lua_pushvalue(L, -3);                                  // the game table
    if (!_engine.pcall(2, 0)) {
        Log.errorln("LuaGame: %s rejected: %s", path, _engine.lastError());
        lua_settop(L, 0);
        _engine.end();
        _slotIdx = 0xFF;
        return false;
    }

    _gameRef = luaL_ref(L, LUA_REGISTRYINDEX);             // keep the table alive

    s_instances[s_instanceCount++] = this;
    _loaded = true;
    Log.infoln("LuaGame: loaded '%s' (typeId 0x%x) from %s", _name, _game.typeId, path);
    return true;
}

void LightAir_LuaGame::unload() {
    // NOTE: instances are load-once in this firmware (the pool never
    // shrinks); unload() only exists to clean up a failed load().
    _loaded = false;
    _engine.end();
    _slotCount = 0;
    _countdownCount = 0;
    _progCount = 0;
    _stateMax = 0;
    memset(&_game, 0, sizeof(_game));
}
