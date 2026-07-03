#pragma once
#include "../game/LightAir_Game.h"
#include "LightAir_LuaEngine.h"

// ----------------------------------------------------------------
// LightAir_LuaGame — loads one .lua game file and synthesizes a
// LightAir_Game descriptor from it, so GameRunner / GameSetupMenu /
// score collection run a Lua game through exactly the same code
// paths as a native C++ ruleset.
//
// Design (docs/lua-games-design.md):
//   - Variable blackboard: config/vars entries claim int (or text)
//     slots owned by this object; the synthesized ConfigVar[] /
//     MonitorVar[] / WinnerVar[] point straight into them, so the
//     LCD and the setup menu reuse today's code with zero per-tick
//     marshalling.  Lua reads/writes them through the global `vars`
//     proxy (__index/__newindex C closures).
//   - Callbacks are trampolines: descriptor tables carry plain
//     function pointers that dispatch into this instance's Lua
//     handlers (per-rule identity via small templated trampoline
//     tables; everything else re-derives identity from the current
//     state / packet).
//   - Totem roles are serialized at load time into TotemVM programs
//     (docs/totem-behavior-handshake.md); {"cfg"} placeholders are
//     patched with the live config value when GameRunner asks for
//     the program while replying to a totem beacon.
//
// Exactly one Lua game is *active* at a time (the one GameRunner
// begin()s); several may be loaded so the setup menu can list them.
// ----------------------------------------------------------------
class LightAir_LuaGame {
public:
    // Load and validate a game file.  On success the descriptor()
    // can be registered with LightAir_GameManager.  On failure the
    // instance is unusable (error logged) — call load() again or
    // discard.
    bool load(const char* path);

    const LightAir_Game& descriptor() const { return _game; }
    const char*          name()       const { return _name; }
    uint16_t             typeId()     const { return _game.typeId; }
    bool                 loaded()     const { return _loaded; }

    // ---- Static registry (bounded pool; instances register on load) ----
    static uint8_t         instanceCount();
    static LightAir_LuaGame* instance(uint8_t i);

private:
    // ---- One blackboard slot: an int or a text buffer ----
    struct VarSlot {
        char id[LuaDefaults::MAX_VAR_ID];
        bool isText;
        int  val;
        char text[LuaDefaults::MAX_TEXT_LEN];
    };

    // ---- One serialized totem program + its {"cfg"} patch sites ----
    struct Prog {
        TotemProgramEntry entry;
        uint8_t bytes[TotemVMDefs::MAX_PROG];
        int8_t  cfgSlot;          // config slot index for {"cfg"}; -1 = none
        uint8_t cfgDefault;       // seconds fallback (program's cfg_default)
        uint8_t patchOff[4];      // offsets of u16 ds immediates to patch
        uint8_t patchCount;
    };

    // ================= instance data =================
    LightAir_LuaEngine _engine;
    bool               _loaded = false;
    uint8_t            _slotIdx = 0xFF;      // index in the static registry
    char               _name[LuaDefaults::MAX_GAME_NAME] = {0};

    LightAir_Game _game = {};
    uint8_t       _state = 0;                // storage for _game.currentState

    // Blackboard
    VarSlot _slots[LuaDefaults::MAX_VARS];
    uint8_t _slotCount = 0;

    // Synthesized descriptor tables
    ConfigVar  _configVars[LuaDefaults::MAX_VARS];
    char       _cfgNames[LuaDefaults::MAX_VARS][LuaDefaults::MAX_CFG_NAME];
    MonitorVar _monitorVars[LuaDefaults::MAX_MONITOR];
    WinnerVar  _winnerVars[GameDefaults::MAX_WINNER_VARS];
    StateRule  _rules[LuaDefaults::MAX_RULES];
    StateBehavior _behaviors[LuaDefaults::MAX_STATES];
    DirectRadioRule _directRules[LuaDefaults::MAX_MSG_RULES];
    ReplyRadioRule  _replyRules[2];
    LightAir_TotemRequirement _totReqs[TotemDefs::MAX_TOTEM_ROLES];
    uint8_t _teamMap[PlayerDefs::MAX_PLAYER_ID];

    // Countdown vars (declarative per-second decrement)
    struct Countdown { uint8_t slot; uint32_t stateMask; };
    Countdown _countdowns[LuaDefaults::MAX_COUNTDOWNS];
    uint8_t   _countdownCount = 0;
    uint32_t  _lastSecTick = 0;

    // Serialized totem programs
    Prog    _progs[TotemDefs::MAX_TOTEM_ROLES];
    uint8_t _progCount = 0;

    // Lua registry references
    int _gameRef   = LUA_NOREF;
    int _beginRef  = LUA_NOREF;
    int _scoreRef  = LUA_NOREF;
    int _endRef    = LUA_NOREF;
    int _msgTabRef[LuaDefaults::MAX_STATES];
    int _updateRef[LuaDefaults::MAX_STATES];
    int _replyTabRef = LUA_NOREF;
    int _ruleWhenRef[LuaDefaults::MAX_RULES];
    int _ruleActRef[LuaDefaults::MAX_RULES];
    int _varsProxyRef = LUA_NOREF;
    int _pktUdRef[3];                        // reusable packet proxies
    int _pktByteFnRef = LUA_NOREF;           // shared pkt:byte method
    int _libCacheRef  = LUA_NOREF;           // la.lib() results

    uint8_t _stateMax = 0;                   // highest state index seen

    // ================= internals =================
    void unload();
    void registerKernel();                   // build the `la` table + proxies

    // Loader body (runs protected; throws lua errors on validation issues)
    static int  loaderBody(lua_State* L);
    void        loadFromTable(lua_State* L, int tbl);
    int         findSlot(const char* id) const;              // -1 = not found
    int         addSlot(lua_State* L, const char* id, bool text);
    uint8_t     encodeProgram(lua_State* L, int tbl, Prog& p); // returns len
    void        noteState(lua_State* L, int s);

    // Runtime dispatch (called from the trampolines)
    void doBegin(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*,
                 const LightAir_GameRunner&);
    bool doRuleWhen(uint8_t idx);
    void doRuleAct(uint8_t idx);
    void doBehavior();
    void doMessage(const RadioPacket& pkt, GameOutput& out);
    void doReply(const RadioPacket& reply, const RadioPacket& orig);
    void doTimeout(const RadioPacket& orig);
    void doScoreAnnounce(const ScoreTable& t);
    void doEnd();
    void tickCountdowns();
    void luaFault(const char* where);        // error containment
    bool pushHandler2(int tabRef, int key1, int key2); // tab[key1][key2] → stack
    void pushVarsProxy();
    void pushPkt(uint8_t which, const RadioPacket* pkt, int8_t rssi);
    const TotemProgramEntry* patchedProgram(uint8_t roleId);

    // ---- la verb implementations (upvalue = instance) ----
    static LightAir_LuaGame* self(lua_State* L);
    static int l_vars_index(lua_State* L);
    static int l_vars_newindex(lua_State* L);
    static int l_pkt_index(lua_State* L);
    static int l_pkt_byte(lua_State* L);
    static int l_lib(lua_State* L);
    // (plain-context verbs are file-local in the .cpp)

    // ---- static trampoline plumbing ----
    static const TotemProgramEntry* progTramp(uint8_t roleId);

    friend struct LuaGameTramps;
};
