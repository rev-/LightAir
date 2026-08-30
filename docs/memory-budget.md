# RAM budget — where the internal SRAM goes

## The 8 MB is flash. The RAM is 512 KB.

Worth stating plainly, because it is the whole shape of the problem.
`ESP32-S3-WROOM-1-N8` means **8 MB of flash** and, on this part, **no PSRAM**.
RAM is the 512 KB of SRAM on the die itself, and it is not expandable — the
flash holds the program and the LittleFS games partition and contributes
nothing to the budget below.

So the numbers are:

| | size | holds |
|---|---:|---|
| flash | 8 MB | 3.3 MB app slot ×2, ~1.5 MB LittleFS. Roomy. |
| **SRAM** | **512 KB** | IDF + WiFi + FreeRTOS heap + everything here |

Of that 512 KB, a large share is spoken for before any LightAir object exists:
the IDF, the WiFi/ESP-NOW stack, lwIP, the task stacks. What is left is the
heap that the display bindings, the radio buffers and the Lua interpreter all
draw on.

This is the survey of what we spend, what sizes each item, and what can be
given back — **§1–§4 our own C++, §5 the platform** (WiFi, Bluetooth, lwIP),
which is where the larger numbers are.

---

## Method, and how much to trust the numbers

Sizes below are `sizeof()` **measured on a host build** of the real headers.
That is exact for the parts that dominate — fixed `char`/`uint8_t` arrays are
the same width everywhere — and an over-estimate for pointer-heavy structs,
since a host pointer is 8 bytes against the device's 4. The "device" column
corrects for that by hand where it matters.

**Not measured here:** the actual `.bss`/`.data` totals and the free heap at
boot. Those need a target build (`arduino-cli`, `.map`) or the device itself.

The device now reports them. `LightAir.ino` weighs the heap at every boot
stage, so the platform costs in §5 stop being estimates on the next flash:

```
mem boot           free ...  largest ...  min ...
mem enlight        ...
mem display        ...
mem radio+wifi     ...          <-- the big one
mem games scanned  ...
mem game loaded    ...
```

`min` is the low-water mark since boot — how close the device actually came
to the edge, which no static analysis can tell you. Two more lines come from
the Lua layer:

```
Lua: allocator using internal RAM (N B free internal)
GameStore: loading /games/x.lua (psram … heap … largest block …)
```

Treat this document as the map and those lines as the ground truth. **Nothing
below is worth acting on before reading them** — the whole memory story so far
is inference from one game refusing to load, and if `mem radio+wifi` comes back
at 200 KB free, §5 is not needed at all.

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

## 5. The platform — WiFi, Bluetooth, lwIP

This is where the larger numbers live, and where the intuition "import only the
functions we need" runs into how Arduino builds.

### First, the thing that does not work

**`arduino-cli` cannot trim WiFi or Bluetooth at the config level.** The ESP32
core ships as *prebuilt static libraries* with a frozen `sdkconfig`. The linker
already discards unreferenced code — `-ffunction-sections -fdata-sections
-Wl,--gc-sections` are core defaults — but that only reclaims **flash**, which
we have 8 MB of and do not need. The RAM those libraries reserve is fixed by
`CONFIG_ESP32_WIFI_*`, `CONFIG_LWIP_*` and friends at the time the core was
compiled, and no compiler flag we can pass reaches them.

Changing those needs a different build system: PlatformIO with a custom
`sdkconfig`, or ESP-IDF with Arduino as a component. That is a real option, but
it is a migration, not a setting.

What *is* reachable from inside Arduino is the part WiFi takes at **runtime**,
and that turns out to be most of it.

### B1 — Bluetooth: verify, don't assume · up to ~40 KB

Confirmed by grep: **the firmware references no Bluetooth at all** — no BLE, no
`BluetoothSerial`, no `esp_bt_*`. That matters because the BT controller
reserves a large block of DRAM, and it is only returned to the heap by an
explicit `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`.

Arduino's `initArduino()` is supposed to make that call for us when nothing
references BT (it tests a weak `btInUse()` symbol). It very probably already
happens here. **Check `mem boot` before adding anything** — if it comes back
noticeably higher than the rest of the boot accounts for, the release already
ran and there is nothing to win. If not, one call in `setup()` is worth tens of
kilobytes for one line of code.

### B2 — WiFi buffers sized for TCP throughput we never do · est. 25–45 KB

`LightAir_RadioESPNow::begin()` brings WiFi up through Arduino:

```cpp
WiFi.mode(WIFI_STA);
WiFi.disconnect();
...
esp_now_init();
```

`WiFi.mode()` calls `esp_wifi_init()` with `WIFI_INIT_CONFIG_DEFAULT()`, which
is tuned for streaming TCP over an access point: ten static RX buffers of ~1.6 KB
each, thirty-two dynamic RX, thirty-two TX, and A-MPDU aggregation on in both
directions with its reorder windows.

**We run ESP-NOW.** Frames are at most 250 bytes, unacknowledged, unaggregated,
a handful per 10 ms tick. Essentially none of that provisioning is used.

`esp_wifi_init()` takes the config as an argument, so this *is* changeable at
runtime — but not through `WiFi.mode()`, which passes the defaults and offers no
hook. It means initialising WiFi at the IDF level in `RadioESPNow::begin()`
(`esp_wifi_init(&trimmed)` → `esp_wifi_set_mode` → `esp_wifi_start`) instead of
going through the Arduino class: fewer static/dynamic RX and TX buffers, AMPDU
off in both directions, `WIFI_STORAGE_RAM` so association state stops touching
NVS.

*Risk:* Settings → Share games runs a SoftAP and an HTTP server over the same
WiFi init, and smaller buffers make a file transfer slower. Slower, not broken —
and see B3, which changes that picture anyway.

### B3 — Gameplay does not need lwIP at all · est. 10–25 KB

ESP-NOW is a link-layer protocol. It needs no IP stack, no DHCP server, no
sockets — but `WiFi.mode(WIFI_STA)` brings up `esp_netif` and lwIP regardless,
with their PCB tables and pbuf pools.

The one thing that genuinely needs TCP/IP is **Share games**, and the code
already keeps it apart: `GameFileServer` is entered from the Settings menu, and
leaving it **reboots the device** (`"Share games" / "Rebooting..."`). The HTTP
server and a running match never coexist.

So the split is already there in the product, and only the boot has to follow
it: come up ESP-NOW-only with no netif, and let Share games be a boot mode that
brings up `esp_netif` + AP + `WebServer` — where the full-size buffers of B2 are
also fine, because nothing else is competing for RAM in that mode.

This is the most structural of the three and the one that makes the other two
easy. It is also the most work.

### B4 — Small change, take it anyway

`esp_wifi_set_ps(WIFI_PS_NONE)` for ESP-NOW: a few KB and better latency, since
a sleeping radio delays every beacon-aligned frame.

---

## 6. Summary

Ranked by return, across both halves:

| | est. saving | effort | risk |
|---|---:|---|---|
| B3 no lwIP on the gameplay path | 10–25 KB | high | medium — reshapes the Share-games flow |
| B2 trim `wifi_init_config_t` | 25–45 KB | medium | low–medium — slower file transfer |
| A `MAX_SETS` 32→9, `MAX_BINDINGS` 8→4 | 16.8 KB | low | low — needs the `resetBindingSets` fix |
| B1 release BT memory | 0 or ~40 KB | trivial | none — probably already done |
| C drop the scanner `LuaGame` | 7.2 KB | low | low |
| D radio queue depths | ~4 KB | low | medium — dropped packets under load |
| — shrink `RADIO_MAX_PAYLOAD` | — | — | **rejected**: set by `MAX_PROG = 225` |
| — shrink `MAX_VARS` / `MAX_MSG_RULES` | 1–2 KB | low | caps what a ruleset may declare |

Our own C++ (A + C) gives ~24 KB for very little work and lowers no limit the
firmware can actually reach: 9 binding sets is the Lua state ceiling, 4 cells is
the screen, and the scanner instance is 7 KB of arrays a manifest peek never
reads. **Do those first** — they are cheap and provably safe.

The platform (B1–B3) is two to three times bigger but costs real work and real
risk, and none of it is justified yet. Read `mem radio+wifi` and `mem game
loaded` off a boot log first: they say in one line whether there is a shortage
at all, and B2/B3 are only worth building if there is.
