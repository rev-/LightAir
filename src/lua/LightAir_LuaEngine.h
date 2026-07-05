#pragma once
#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ----------------------------------------------------------------
// LightAir_LuaEngine — one sandboxed lua_State.
//
// - Custom allocator: prefers PSRAM (heap_caps SPIRAM) with an
//   internal-RAM fallback, so Lua never competes with radio/display
//   buffers for internal memory.
// - Sandbox: opens base / table / string / math only, then removes
//   dofile/loadfile/load/collectgarbage.  Game code can only act
//   through the `la` kernel registered by LightAir_LuaGame.
// - Protected calls: pcall() wraps lua_pcall with a traceback
//   message handler and an instruction-budget hook so a buggy or
//   runaway game file produces a logged Lua error instead of a
//   frozen device.
//
// One engine per loaded game file (each game keeps its own globals
// and library cache); memory lives in PSRAM so several loaded games
// are cheap.
// ----------------------------------------------------------------
class LightAir_LuaEngine {
public:
    // Create the state and open the sandboxed libraries.
    // Returns false on allocation failure.
    bool begin();

    // Close the state (safe to call twice).
    void end();

    lua_State* L() const { return _L; }

    // Protected call with traceback + instruction budget.
    // Stack contract: function and nargs arguments on top, exactly like
    // lua_pcall.  On success, nresults results replace them.  On failure
    // the error (with traceback) is copied into lastError(), logged, and
    // popped — the stack is back to the pre-push level minus the call.
    bool pcall(int nargs, int nresults);

    // One incremental GC step; call in the game loop's slack window.
    void gcStep();

    // Last pcall error message (truncated); empty string if none.
    const char* lastError() const { return _err; }

private:
    lua_State* _L = nullptr;
    char       _err[120] = {0};

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static int   traceback(lua_State* L);
    static void  budgetHook(lua_State* L, lua_Debug* ar);
};
