#pragma once
#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "../libs/lua-5.5.0/src/lua.h"
#include "../libs/lua-5.5.0/src/lauxlib.h"
#include "../libs/lua-5.5.0/src/lualib.h"
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

    lua_State* L() const { return _lua; }

    // Protected call with traceback + instruction budget.
    // Stack contract: function and nargs arguments on top, exactly like
    // lua_pcall.  On success, nresults results replace them.  On failure
    // the error (with traceback) is copied into lastError(), logged, and
    // popped — the stack is back to the pre-push level minus the call.
    bool pcall(int nargs, int nresults);

    // One incremental GC step; call in the game loop's slack window.
    void gcStep();

    // Full collection.  Loading a ruleset parses it and both its
    // libraries, which leaves far more garbage than the incremental
    // collector will have reached — and does it at the one moment
    // nothing is timing-critical, before the match starts.
    void gcFullCollect();

    // Lua's own heap, in KB.  Logged after a load: the ruleset files are
    // user-editable, so "how much of the device did this one cost" is a
    // question the serial log has to be able to answer.
    unsigned heapKB() const;

    // Last pcall error message (truncated); empty string if none.
    const char* lastError() const { return _err; }

private:
    // Not `_L`: newlib's <ctype.h> defines _L (and _U/_N/_S/_P/_C/_X/_B)
    // as character-class bit macros, and the Arduino core drags ctype.h in
    // via Arduino.h -> WCharacter.h.  Any translation unit that includes
    // Arduino.h before this header would see `lua_State* 02 = nullptr;`.
    lua_State* _lua = nullptr;
    char       _err[120] = {0};

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static int   traceback(lua_State* L);
    static void  budgetHook(lua_State* L, lua_Debug* ar);
};
