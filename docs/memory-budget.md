# RAM budget — where the internal SRAM goes

The projectors are `ESP32-S3-WROOM-1-N8`: 8 MB flash, **no PSRAM**. Every
allocation the firmware makes — the display bindings, the radio buffers, the
Lua interpreter, the task stacks — comes out of the same ~512 KB of internal
SRAM, most of which is already spoken for by the IDF, the WiFi/ESP-NOW stack
and the FreeRTOS heap before a single LightAir object exists.

This is the survey of what *we* spend, what sizes each item, and what can be
given back.

---

## Method, and how much to trust the numbers

Sizes below are `sizeof()` **measured on a host build** of the real headers.
That is exact for the parts that dominate — fixed `char`/`uint8_t` arrays are
the same width everywhere — and an over-estimate for pointer-heavy structs,
since a host pointer is 8 bytes against the device's 4. The "device" column
corrects for that by hand where it matters.

**Not measured here:** the actual `.bss`/`.data` totals and the free heap at
boot. Those need a target build (`arduino-cli`, `.map`) or the device itself.
Two log lines exist for that now:

```
Lua: allocator using internal RAM (N B free internal)
GameStore: loading /games/x.lua (psram … heap … largest block …)
```

Treat this document as the map, and those two lines as the ground truth.

---

## 1. Static objects — always resident

Constructed at global scope in `LightAir.ino`, so they cost their full size
from boot to power-off whether or not the code path that uses them runs.

| Object | host | device (est.) | Sized by |
|---|---:|---:|---|
| `LightAir_DisplayCtrl` | 27,016 | **~19,700** | `MAX_SETS = 32` × `MAX_BINDINGS = 8` × 76 B |
| `LightAir_LuaGame` ×2 | 16,080 | **~14,400** | `MAX_VARS`, `MAX_PROG`, `MAX_MSG_RULES`, `MAX_RULES` |
| `LightAir_Radio` | 8,032 | ~8,000 | `RADIO_MAX_PAYLOAD = 237` × (`MAX_PENDING = 10` + report 10) |
| `LightAir_RadioESPNow` | 4,184 | ~4,170 | `ESPNOW_RECV_QUEUE = 16` × 250 B |
| `LightAir_GameStore` | 4,400 | ~3,200 | `MAX_GAMES = 16` (manifest + placeholder per slot) |
| `LightAir_UICtrl` | 536 | ~450 | action table |
| `LightAir_GameRunner` | 472 | ~350 | |
| `Enlight` | 456 | ~430 | |
| totem-path globals (idle on a player) | 320 | ~320 | LED strip buffer |
| input (`InputCtrl` + keypad + 2 buttons) | 320 | ~280 | |
| `LightAir_GameManager` | 208 | ~130 | `MAX_GAMES` |
| **Total** | | **~51 KB** | |

Two objects are 2/3 of it, and both are sized far above anything the firmware
can actually use. See §3.

## 2. Transient peaks — on top of the above

| What | Size | When | Where |
|---|---:|---|---|
| Lua state for the selected ruleset | **~55 KB** | while a game is loaded | heap |
| (the same, before the streaming fix) | ~120 KB | — | heap |
| `GameOutput output;` | ~2,000 B | **every tick**, `GameRunner::update()` | loop-task stack |
| OLED frame buffer | 1–2 KB | after `display.begin()` | heap |
| `EnlightCalibRoutine` / `EnlightTestMode` | 1.2 / 1.4 KB | only while the tool is open | heap |
| Enlight DMA task stack | 4 KB | player path, from `Enlight::begin()` | task stack |
| WiFi AP + `WebServer` | tens of KB | only in Settings → Share games | IDF |

The loop task runs on Arduino's default 8 KB stack, and everything above the
runner shares it: the ~2 KB `GameOutput` each tick, and — during a game load —
Lua's recursive-descent parser. That is a second, independent ceiling from the
heap one, and worth remembering before adding anything large as a local.

The Lua figure is the *whole* cost of a ruleset: compiled functions for the
game file plus `std.lua` plus `projector.lua`, and the tables and closures they
build. It is reported per game in the load log.

---

## 3. Where we can spare some

Ranked by size, with what makes each safe or not.

### A. `DisplayDefaults::MAX_SETS` 32 → 9, `MAX_BINDINGS` 8 → 4 · **~16.8 KB**

The largest single object in the firmware, and both dimensions are far above
their real ceilings.

*Sets.* `GameRunner::begin` creates one binding set per state that has a
monitor row, plus one empty set to freeze the display after scoring. A Lua
ruleset may declare at most `LuaDefaults::MAX_STATES = 8` states, so **9 is the
hard ceiling**, not a guess. Measured need across the whole catalogue:

```
freeforall 4   teams 4   flag 4   kingofhill 4
outflow 4      upkeep 4  virus 4  festasportsasso 5
```

*Bindings per set.* The content area is `SCREEN_HEIGHT − TRAY_HEIGHT` = 34 px
at `CELL_HEIGHT` = 12, so **two rows of `CELL_COLS` = 2 cells fit on the glass
— four cells**. Every game in the catalogue uses exactly 4. Eight was never
displayable.

9 × 4 × 76 B = 2.9 KB, against 19.7 KB today.

> **Do not cut `MAX_SETS` without fixing this first:** `_setCount` is never
> reset, and `GameRunner::begin()` does not clear the sets it created last
> time. Today the sketch calls `begin()` once per boot and the end-game A+B
> reboots, so 32 slots hide the leak. At 9 a second `begin()` would run out.
> Add a `DisplayCtrl::resetBindingSets()` called from `GameRunner::begin`.

### B. Drop the second `LightAir_LuaGame` · **~7.2 KB**

`LightAir_GameStore.cpp` keeps two full instances: `s_loadedGame` and
`s_scanner`. The scanner only ever calls `peekManifest`, which builds and tears
down its own `lua_State` and touches none of the 7 KB of descriptor arrays —
`_slots`, `_progs`, `_configVars`, `_monitorVars`, `_rules`, the ref tables.

The boot scan runs before any game is realized, and `peekManifest` leaves the
instance unloaded, so `s_loadedGame` can do both jobs. One instance, one
trampoline slot, ~7.2 KB back.

### C. `ESPNOW_RECV_QUEUE` 16 → 8 · **~2 KB**

`Entry { uint8_t data[250]; int len; int8_t rssi; }` × 16. Halving it is two
kilobytes, but this is the buffer that absorbs bursts between `radio.poll()`
calls and the cost of getting it wrong is dropped packets under load. Measure
the real high-water mark before touching it.

### D. `RADIO_MAX_PENDING` 10 → 6 · **~2 KB**

Shrinks both `_pending[]` and `RadioReport::events[]`, each of which holds a
full 250-byte packet per slot. Same caveat: this is the depth of outstanding
request/reply pairs, and overflowing it loses replies.

### E. `MAX_GAMES` 50 → 16 · ~6.4 KB · **already taken**

Each menu slot costs a manifest plus a placeholder descriptor whether a file
fills it or not. Done in this branch.

### Rejected: shrinking `RADIO_MAX_PAYLOAD`

237 bytes per packet is what makes the radio structures large, and it looks
like the obvious cut — it is not. It is set by `TotemVMDefs::MAX_PROG = 225`:
a totem's whole behaviour program travels inside one 0xF1 activation reply.
Cutting the payload means cutting what a totem can be told to do. The score
payload has its own floor too, guarded by a `static_assert` in `config.h`
against `MAX_WINNER_VARS × MAX_PLAYER_ID`.

### Rejected: shrinking `LuaDefaults::MAX_VARS` / `MAX_MSG_RULES`

Measured headroom is thinner than it looks. Worst case in the catalogue:

| | used | limit |
|---|---:|---:|
| config + var slots | 15 | 24 |
| monitor rows | 10 | 16 |
| rules | 6 | 16 |
| states | 5 | 8 |

`festasportsasso` alone accounts for 15 of the 24 slots. There is perhaps 1–2 KB
here across two instances, and taking it would cap what a new ruleset can
declare. Not worth it while §A and §B are on the table.

---

## 4. Summary

| | now | after A + B |
|---|---:|---:|
| static objects | ~51 KB | **~27 KB** |
| peak during a game load | ~55 KB | ~55 KB |

A and B together return roughly **24 KB** of internal SRAM, neither of them
reducing any limit the firmware can actually reach: 9 binding sets is the Lua
state ceiling, 4 cells is the screen, and the scanner instance is 7 KB of
arrays that a manifest peek never reads.
