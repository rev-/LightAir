#include "LightAir_LuaEngine.h"
#include <string.h>
#include <stdlib.h>

#ifdef ESP32
#include <esp_heap_caps.h>
#include <ArduinoLog.h>
#define LUA_LOG_ERR(msg) Log.errorln("Lua: %s", msg)
#else
#include <stdio.h>
#define LUA_LOG_ERR(msg) fprintf(stderr, "Lua: %s\n", (msg))
#endif

#include "../config.h"

/* =========================================================
 *   ALLOCATOR — PSRAM first, internal RAM fallback
 * ========================================================= */

void* LightAir_LuaEngine::alloc(void*, void* ptr, size_t, size_t nsize) {
    if (nsize == 0) {
#ifdef ESP32
        heap_caps_free(ptr);
#else
        free(ptr);
#endif
        return nullptr;
    }
#ifdef ESP32
    void* p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
    return p;
#else
    return realloc(ptr, nsize);
#endif
}

/* =========================================================
 *   LIFECYCLE
 * ========================================================= */

bool LightAir_LuaEngine::begin() {
    if (_L) return true;
    // Lua 5.5 takes a hash seed; derive one from the clock + this pointer.
    unsigned seed = (unsigned)((uintptr_t)this ^ (uintptr_t)&seed);
#ifdef ESP32
    seed ^= (unsigned)millis();
#endif
    _L = lua_newstate(alloc, this, seed);
    if (!_L) return false;

    // Sandboxed library set: base, table, string, math.  No io/os/package.
    luaL_requiref(_L, LUA_GNAME,       luaopen_base,   1);
    luaL_requiref(_L, LUA_TABLIBNAME,  luaopen_table,  1);
    luaL_requiref(_L, LUA_STRLIBNAME,  luaopen_string, 1);
    luaL_requiref(_L, LUA_MATHLIBNAME, luaopen_math,   1);
    lua_settop(_L, 0);

    // Remove filesystem/code-loading escapes from the base library,
    // plus collectgarbage — GC pacing belongs to the firmware (gcStep).
    static const char* kRemoved[] = { "dofile", "loadfile", "load", "collectgarbage" };
    for (const char* name : kRemoved) {
        lua_pushnil(_L);
        lua_setglobal(_L, name);
    }

    // Incremental GC; stepped explicitly from the loop's slack window.
    lua_gc(_L, LUA_GCINC);
    return true;
}

void LightAir_LuaEngine::end() {
    if (_L) {
        lua_close(_L);
        _L = nullptr;
    }
}

/* =========================================================
 *   PROTECTED CALL — traceback + instruction budget
 * ========================================================= */

int LightAir_LuaEngine::traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
    return 1;
}

void LightAir_LuaEngine::budgetHook(lua_State* L, lua_Debug*) {
    luaL_error(L, "instruction budget exceeded");
}

bool LightAir_LuaEngine::pcall(int nargs, int nresults) {
    // Stack: ... func arg1..argN   (top = argN)
    int funcIdx = lua_gettop(_L) - nargs;   // absolute index of the function
    lua_pushcfunction(_L, traceback);
    lua_insert(_L, funcIdx);                // traceback sits below the function

    lua_sethook(_L, budgetHook, LUA_MASKCOUNT, (int)LuaDefaults::INSTR_BUDGET);
    int rc = lua_pcall(_L, nargs, nresults, funcIdx);
    lua_sethook(_L, nullptr, 0, 0);

    lua_remove(_L, funcIdx);                // drop the traceback handler
    if (rc == LUA_OK) {
        _err[0] = 0;
        return true;
    }
    const char* msg = lua_tostring(_L, -1);
    if (!msg) msg = "(unknown Lua error)";
    strncpy(_err, msg, sizeof(_err) - 1);
    _err[sizeof(_err) - 1] = 0;
    LUA_LOG_ERR(msg);
    lua_pop(_L, 1);                         // pop the error value
    return false;
}

void LightAir_LuaEngine::gcStep() {
    // LUA_GCSTEP takes a size_t vararg in Lua 5.5; 0 = one basic step.
    if (_L) lua_gc(_L, LUA_GCSTEP, (size_t)0);
}
