# How Lua works inside LightAir — the embedding guide

This is the companion to `docs/lua-games-design.md`.  That document says
*what* crosses the C++/Lua boundary; this one explains *how* the boundary
physically works — the `lua_State`, the virtual stack (with step-by-step
stack diagrams for our own binding code), the registry, metatables, errors
and the garbage collector, plus the configuration choices we make for the
ESP32-S3.

Everything here targets the vendored Lua 5.5 (`src/libs/lua-5.5.0`) built
with `-DLUA_32BITS`.

---

## 1. The moving parts

**`lua_State`** is the whole interpreter: globals, loaded code, the garbage
collector, and *the stack*.  It is an opaque pointer; every API call takes it
as the first argument.  We create exactly one per device:

```cpp
lua_State* L = lua_newstate(la_alloc, nullptr);   // custom allocator (§7)
```

**A chunk** is a compiled piece of Lua source.  `luaL_loadfile()` compiles
`/games/flag.lua` into an anonymous function; *running* that function (the
file's top-level code) executes the `local` declarations and returns the
game table.  Nothing in a game file runs "by itself" — the firmware calls
into it.

**A C function** (`lua_CFunction`) is any `int f(lua_State* L)` registered
with Lua.  It receives its arguments on the stack and returns how many
results it left on the stack.  Every `la.*` verb is one of these.

**The registry** is a Lua table only C can see (`LUA_REGISTRYINDEX`).  We
use it to keep references to Lua functions (rule conditions, handlers) so we
can call them later without them being collected.

---

## 2. The virtual stack — the one concept that matters

C and Lua never share raw pointers to values.  All exchange happens through
a per-state stack of Lua values.  Think of it as an array you can only
address by index:

- **Positive indices** count from the bottom: `1` is the first value pushed.
- **Negative indices** count from the top: `-1` is the top, `-2` below it.
- `lua_gettop(L)` returns the current height (also = the index of the top).

Three families of operations:

| Family | Examples | Effect on stack |
|---|---|---|
| **push** (C → stack) | `lua_pushinteger`, `lua_pushstring`, `lua_pushcfunction`, `lua_newtable` | height +1 |
| **query** (stack → C) | `lua_tointeger(L, i)`, `lua_isnil(L, i)`, `luaL_checkinteger(L, i)` | height unchanged |
| **manipulate** | `lua_pop(L, n)`, `lua_settop`, `lua_rotate`, `lua_remove` | height changes |

Two composite families do a query *and* a push or pop:

- `lua_getfield(L, t, "name")` — reads `t.name` (where `t` is a stack index
  of a table) and **pushes** the result (+1).
- `lua_setfield(L, t, "name")` — **pops** the top value (−1) and stores it
  into `t.name`.
- `lua_call/lua_pcall(L, nargs, nresults, msgh)` — pops the function and
  `nargs` arguments, runs, then pushes `nresults` results.

**The golden rule of embedding:** every code path must account for exactly
what it pushed and popped.  A function that leaks one stack slot per tick
overflows the stack in seconds at 100 Hz.  The idiom used throughout our
binding:

```cpp
int base = lua_gettop(L);
// ... push things, call things, read results ...
lua_settop(L, base);              // restore no matter which branch ran
```

The default stack has room for `LUA_MINSTACK` (20) slots per C-function
invocation; anything that pushes in a loop must call
`lua_checkstack(L, n)` first.  Our binding never pushes unbounded sequences,
so a single `lua_checkstack(L, 40)` at `begin()` is a formality.

---

## 3. Stack population, step by step, in our own binding

The examples below are the actual shapes of `LightAir_LuaGame`.  Stack
snapshots are drawn top-down: the top of the stack (`-1`) is the first line.

### 3a. Registering the `la` kernel

```cpp
static const luaL_Reg kVerbs[] = {
    { "now",        l_now        },
    { "my_id",      l_my_id      },
    { "shine",      l_shine      },
    { "shine_lit",  l_shine_lit  },
    { "send",       l_send       },
    { "broadcast",  l_broadcast  },
    // ...
    { nullptr, nullptr }
};

void registerKernel(lua_State* L) {
    luaL_newlib(L, kVerbs);            // push a new table populated
                                       // with the C functions
    //  -1: la table
    lua_newtable(L);                   //  -1: msg table, -2: la
    lua_pushinteger(L, RadioMsg::MSG_LIT);
    //  -1: 0x10,  -2: msg,  -3: la
    lua_setfield(L, -2, "LIT");        // msg.LIT = 0x10   (pops the int)
    //  -1: msg,   -2: la
    ...                                // all other RadioMsg constants
    lua_setfield(L, -2, "msg");        // la.msg = msg     (pops msg)
    //  -1: la
    lua_setglobal(L, "la");            // _G.la = la       (pops la)
    //  stack empty again
}
```

Every `set*` **pops** the value it stores; that is why the table being
filled drifts back to `-1` after each field.  Forgetting this ("why is my
table at `-3` now?") is the classic first embedding bug.

### 3b. Loading a game file and harvesting its table

```cpp
int base = lua_gettop(L);                        // == 0
if (luaL_loadfile(L, "/games/flag.lua") != LUA_OK) {
    //  -1: error message (string)
    log(lua_tostring(L, -1));
    lua_settop(L, base);
    return false;
}
//  -1: the compiled chunk (a function)
if (lua_pcall(L, 0, 1, 0) != LUA_OK) {           // run it: 0 args, 1 result
    //  -1: error message
    ...
}
//  -1: the game table (the file's `return { ... }`)
```

Now the loader walks the table.  Scalars are read and popped immediately:

```cpp
lua_getfield(L, -1, "type_id");   //  -1: 0x0003,  -2: game table
_typeId = (uint16_t)luaL_checkinteger(L, -1);
lua_pop(L, 1);                    //  -1: game table
```

Sub-tables are iterated with the `lua_next` protocol, which has a precise
stack contract — push `nil` as the "previous key", and each iteration
replaces key/value:

```cpp
lua_getfield(L, -1, "config");    //  -1: config array, -2: game
lua_pushnil(L);                   //  -1: nil (seed key), -2: config, -3: game
while (lua_next(L, -2) != 0) {
    //  -1: value (one config entry table)
    //  -2: key   (array index)
    //  -3: config array
    lua_getfield(L, -1, "min");   //  -1: min, -2: entry, ...
    int mn = luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    ...
    lua_pop(L, 1);                // pop value; KEEP the key —
}                                 // lua_next uses it to find the next pair
lua_pop(L, 1);                    // pop the config array
```

Functions we will need every tick are not kept on the stack — they go into
the registry and we remember an integer handle:

```cpp
lua_getfield(L, -1, "update");    //  -1: update table, -2: game
lua_geti(L, -1, S_IN_GAME);       //  -1: function, -2: update, -3: game
_updateRef[S_IN_GAME] = luaL_ref(L, LUA_REGISTRYINDEX);   // pops the function
lua_pop(L, 1);                    // pop the update table
```

`luaL_ref` pops the value, stores it in the registry, and returns a key.
The value is now (a) reachable by C in O(1) and (b) protected from the GC.
`luaL_unref` releases it when the game is unloaded.

Finally the game table itself gets one ref, and the stack is back to `base`.

### 3c. One tick: dispatching `on_message` — full trace

The runner sees `MSG_LIT` arrive in state `IN_GAME` and the synthesized
`DirectRadioRule` trampoline fires.  `g` below is the binding singleton;
`g->pkt` is the *reused* packet-proxy userdata created once at load.

```cpp
int base = lua_gettop(L);                              // stack: (empty)

lua_rawgeti(L, LUA_REGISTRYINDEX, g->msgtabRef);       // 1: msg-handler table
                                                       //    for IN_GAME
lua_rawgeti(L, -1, pkt.msgType);                       // 2: handler function
                                                       // 1: handler table
lua_rawgeti(L, LUA_REGISTRYINDEX, g->varsProxyRef);    // 3: vars proxy
                                                       // 2: handler, 1: table
lua_rawgeti(L, LUA_REGISTRYINDEX, g->pktProxyRef);     // 4: pkt proxy
*g->pktSlot = &incomingPacket;      // point the proxy at the live packet

if (lua_pcall(L, 2, 1, g->tracebackIdx) != LUA_OK) {   // pops fn + 2 args,
    handleError(L);                                    // pushes 1 result
} else {
    // stack: 2: result (reply sub-type or nil), 1: handler table
    replySubType = lua_isnil(L, -1) ? 0 : (uint8_t)lua_tointeger(L, -1);
}
*g->pktSlot = nullptr;              // proxy must not outlive the packet
lua_settop(L, base);                // pop result + handler table
```

Total stack traffic: 4 pushes, one call, everything reclaimed by a single
`lua_settop`.  **No Lua value was allocated**: the handler table, proxy
userdata and function all pre-exist; the integers involved are stack
values, not heap objects.

Inside Lua, when the handler runs `pkt.sender`, the proxy's metatable
`__index` C function executes:

```cpp
int l_pkt_index(lua_State* L) {
    // Lua called pkt.sender, so the stack IS the argument list:
    //  1: the userdata (pkt)
    //  2: the key ("sender")
    const RadioPacket* p = *checkPktSlot(L, 1);
    if (!p) return luaL_error(L, "pkt used outside its handler");
    const char* k = lua_tostring(L, 2);
    switch (k[0]) {
        case 's': lua_pushinteger(L, p->senderId); return 1;
        case 'r': lua_pushinteger(L, rssiOf(p));   return 1;
        // ... 'team', 'msg', 'len', method 'byte' ...
    }
    lua_pushnil(L); return 1;
}
```

Return value = "how many results I left on top".  Lua moves that result to
where the expression needed it and unwinds the rest.  A C function never
sees, and cannot corrupt, the stack frames of other calls: each
`lua_CFunction` invocation gets its own window onto the stack where *its*
argument #1 is index 1.

### 3d. The `vars` proxy — `__newindex` with upvalues

The proxy is an empty table whose metatable carries two C closures.  A
*closure* is a C function bundled with **upvalues** — values it can reach in
O(1) without touching the stack contract:

```cpp
lua_newtable(L);                          // 1: proxy
lua_newtable(L);                          // 2: metatable, 1: proxy

lua_pushlightuserdata(L, this);           // 3: binding ptr
lua_pushcclosure(L, l_vars_index, 1);     // 3: closure (captured the ptr)
lua_setfield(L, -2, "__index");           // 2: metatable

lua_pushlightuserdata(L, this);
lua_pushcclosure(L, l_vars_newindex, 1);
lua_setfield(L, -2, "__newindex");

lua_setmetatable(L, -2);                  // 1: proxy (metatable popped)
_varsProxyRef = luaL_ref(L, LUA_REGISTRYINDEX);   // stack empty
```

```cpp
int l_vars_newindex(lua_State* L) {
    // stack: 1: proxy table, 2: key, 3: value   (vars.lives = 2)
    auto* self = (LightAir_LuaGame*)lua_touserdata(L, lua_upvalueindex(1));
    int slot = self->slotOf(lua_tostring(L, 2));      // hash lookup
    if (slot < 0) return luaL_error(L, "unknown var '%s'", lua_tostring(L, 2));
    if (self->isText(slot))
        self->writeText(slot, luaL_checkstring(L, 3));
    else
        self->_slots[slot] = (int32_t)luaL_checkinteger(L, 3);
    return 0;                                          // nothing pushed
}
```

Because the proxy table stays empty, *every* read and write takes the
metamethod path, and the ints land directly in the C array the LCD is bound
to.  `lua_upvalueindex(1)` is a pseudo-index — it addresses the closure's
first upvalue, outside the normal stack, which is how one C function serves
many binding instances without globals.

### 3e. Errors, tracebacks, runaway code

Raw `lua_call` propagates errors as C `longjmp` — fatal in firmware.  We
only ever use `lua_pcall`, with a message handler that is pushed once at
`begin()` and referenced by stack index (`g->tracebackIdx`):

```cpp
int l_traceback(lua_State* L) {
    //  1: the original error message
    luaL_traceback(L, L, lua_tostring(L, 1), 1);   // 2: message + stack trace
    return 1;
}
```

On error, `lua_pcall` leaves exactly one value (the handler's result) on
top; we count it per call-site (`faultStats()`), log it with the traceback,
show a throttled tray notice, and **continue the match from the previous
condition** — pcall guarantees the Lua and C++ state stay consistent, so a
broken handler costs one missed event, never the device or the match.  Only
a failed `on_begin` is fatal (the game refuses to start).

Runaway loops are caught with a count hook installed before every pcall:

```cpp
lua_sethook(L, l_budget_hook, LUA_MASKCOUNT, 200000);
// l_budget_hook -> luaL_error(L, "instruction budget exceeded")
```

The hook fires inside the VM after 200k instructions and raises an error,
which the same pcall path catches.  10 ms of straight-line Lua on a 240 MHz
S3 is *millions* of instructions, so the budget only trips genuinely stuck
code.

---

## 4. What allocates and what doesn't (GC discipline)

The garbage collector only runs when something allocates.  Per-tick cost is
therefore a *design property* of the binding:

| Never allocates | Allocates (fine at load / on events; avoid per tick) |
|---|---|
| pushing/reading integers, booleans, floats | `{}` table constructors |
| calling a Lua function (frame reuse) | closures (`function() ... end`) |
| reading `vars.x` / writing `vars.x = n` | string concatenation `..`, `string.format` |
| `pkt.sender`, `pkt:byte(i)` (reused proxy) | new userdata |
| short strings already interned (event names like `"Taken"` are interned at load) | first use of a *new* string |
| table reads/writes to existing keys | adding a key to a table (may rehash) |

The game files follow this: closures and config tables are built at load
time; tick bodies only do arithmetic, comparisons and verb calls; strings
are only built in *event* handlers (a lit, an infection — a few per second
at most).

The collector runs incrementally: after the per-state `update` trampoline
returns, the binding calls

```cpp
lua_gc(L, LUA_GCSTEP, 0);
```

inside the loop's slack window (the runner busy-waits the remainder of
`LOOP_MS` anyway, so collection is free).  Lua 5.5's generational mode
(`LUA_GCGEN`) is worth enabling once profiled: event-handler garbage is
short-lived, exactly what the young generation collects cheaply.

---

## 5. `LUA_32BITS` and numbers

Built with `-DLUA_32BITS` (already in the Makefile), Lua integers are
`int32_t` and floats are C `float`:

- A Lua integer is *exactly* our slot type — `vars.lives` round-trips with
  no conversion or precision concern.
- `la.now()` returns `millis()` as an integer; it wraps together with the
  C++ clock (49 days), and all our comparisons use differences, which are
  wrap-safe.
- Bitwise operators (`& | ~ << >>`, used by the CP totem port) operate on
  those 32-bit integers natively.
- Division `/` always yields a float; the game files use `//` (floor
  division) when an integer is wanted, e.g. `start_energy // 5` in Virus.

---

## 6. The sandbox

`luaL_openlibs()` would expose `io`, `os.execute`, `package` — none of which
may exist on a game device.  We open exactly four libraries:

```cpp
luaL_requiref(L, "_G",     luaopen_base,   1);   // pcall, pairs, type, ...
luaL_requiref(L, "table",  luaopen_table,  1);
luaL_requiref(L, "string", luaopen_string, 1);
luaL_requiref(L, "math",   luaopen_math,   1);
lua_settop(L, 0);
```

then remove `dofile`, `loadfile` and `load` from the base library (file
access goes through `la.lib`, which only reaches `/games/lib/`).  A game
file can compute anything, but it can only *act* through the `la` kernel —
which is the whole security model, and why the kernel must stay small.

---

## 7. Memory placement on the ESP32-S3

`lua_newstate` takes an allocator, and ours prefers PSRAM so Lua never
competes with radio/display buffers for internal RAM:

```cpp
void* la_alloc(void*, void* ptr, size_t, size_t nsize) {
    if (nsize == 0) { heap_caps_free(ptr); return nullptr; }
    void* p = heap_caps_realloc(ptr, nsize,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);  // fallback
}
```

Budget observed with comparable setups: the Lua core costs ~150–200 KB of
flash; a loaded game (state + chunk + closures + our proxies) is ~30–80 KB
of PSRAM.  PSRAM access is slower than internal SRAM (cache-mediated), but
the tick path touches a few hundred bytes of hot objects that live in cache;
if profiling ever disagrees, the allocator can route small blocks to
internal RAM first instead.

---

## 8. Pitfalls checklist (read before touching the binding)

1. **Balance the stack** — wrap every entry point in
   `int base = lua_gettop(L); ... lua_settop(L, base);`.
2. **`set*` pops, `get*` pushes** — after `lua_setfield` the table is one
   slot nearer the top than you think.
3. **Negative indices move** — after a push, what was `-1` is now `-2`.
   Convert to absolute (`lua_absindex`) before loops that push.
4. **`lua_next` needs the key left on the stack** — pop only the value.
5. **`lua_tostring` results die with their value** — copy the C string out
   before popping; never cache it across a `lua_pcall` (GC may move on).
6. **Never `lua_call` in firmware** — always `lua_pcall` with the traceback
   handler; a Lua error otherwise `longjmp`s through C++ frames and skips
   destructors.
7. **Proxies must not escape their scope** — `pkt` is valid only during the
   handler call; the C side nulls the backing pointer afterwards and the
   `__index` guard turns later use into a Lua error instead of a wild read.
8. **Registry refs leak like malloc** — every `luaL_ref` needs a
   `luaL_unref` on game unload, or reloading games slowly eats RAM.
9. **Don't allocate in the tick path** — see the table in §4; new tables,
   closures and strings belong in load-time or event code.
10. **One `lua_State`, one core** — the runner and the binding run on the
    same task; no `la` verb may be called from an ISR or another FreeRTOS
    task.
