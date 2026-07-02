# Lua games — design

Move every game ruleset (FreeForAll, Teams, Flag, KingOfHill, Outflow, Upkeep)
and the totem behaviours they use out of the firmware and into one `.lua` file
per game, stored on flash and exchangeable as plain files over HTTP.

`games/freeforall.lua` is the reference implementation of this format — a full
port of `src/rulesets/GameFreeForAll.cpp` plus the BONUS/MALUS totem roles.

---

## 1. Where the boundary sits

Everything the current `LightAir_GameRunner` already does *around* the ruleset
stays in C++ and is untouched by this migration:

| Stays in C++ (firmware)                          | Moves to Lua (game file)              |
|--------------------------------------------------|---------------------------------------|
| 10 ms loop, READ→LOGIC→OUTPUT phasing            | State list, initial/scoring state      |
| Input polling (buttons, keypad)                  | Transition rules (`rules`)             |
| Radio transport, pending/replies/dedup/flood     | Per-state tick bodies (`update`)       |
| Enlight optics (run/poll/calibration)            | Incoming-message handlers (`on_message`) |
| LCD driver, binding sets, tray messages          | Reply/timeout handlers (`on_reply`)    |
| Buzzer/vibration/RGB effects (UIEvent playback)  | Config var *declarations*              |
| Setup menu, discovery, teams/totem assignment    | Monitor var *declarations* (LCD layout)|
| Config blob wire format + broadcast              | Winner var declarations                |
| Warmup countdown                                 | Totem role behaviour (`totems`)        |
| Score collection, fusion, winner election        | All game-private state (Lua locals)    |
| Totem activation protocol (0xF0/0xF1/0xF2)       |                                        |
| End-game A+B reboot                              |                                        |

The key observation that makes this cheap: `LightAir_Game` is **already a pure
data table** of function pointers and var descriptors. The Lua engine does not
replace the runner — it *synthesizes* a `LightAir_Game` descriptor at load
time whose callbacks are trampolines into Lua. `GameRunner`, `GameSetupMenu`,
score collection, the config blob and the radio protocol all keep working
unchanged, and native C++ games can coexist with Lua games during migration.

---

## 2. The variable blackboard (monitorVars / configVars answer)

The LCD binds to `int*` and the setup menu edits `int*`. Instead of copying
values across the boundary every tick, **the variables live in C++ and Lua
gets a proxy**:

- `LightAir_LuaGame` owns a fixed array `int32_t _slots[LuaDefaults::MAX_VARS]`.
- Every entry of the Lua `config = {...}` and `vars = {...}` tables claims one
  slot at load time.
- The synthesized `ConfigVar[]`, `MonitorVar[]` and `WinnerVar[]` point into
  `_slots` — so `DisplayCtrl`, the S4a config menu, the config blob
  serializer and score collection are literally reusing today's code paths
  with zero per-tick marshalling. The LCD reads the int directly, 30 fps,
  no Lua involved.
- Lua reads/writes through a global proxy table `vars` whose `__index` /
  `__newindex` metamethods are C functions doing a name→slot lookup (the
  name→index map is built once at load; lookups are one hash probe, no
  allocation). `vars.lives = vars.lives - 1` costs ~2 µs.

Because config vars are ordinary slots, the flow the user asked for falls out
naturally:

1. Boot: game store scans `/games/*.lua`, extracts `{name, type_id, path}`
   manifests for the game list.
2. DM selects a game → the file is loaded, slots are allocated, `ConfigVar[]`
   is synthesized from the `config` table (name/min/max/step/default).
3. The existing S4a menu edits the slots; the existing config blob
   (`game_serialize_config`) broadcasts them; non-DM devices apply the blob
   into their own slots for the same file.
4. Warmup countdown runs (C++, unchanged), then the runner calls the Lua
   `on_begin(vars)` — which reads the settled config values through the proxy.

Declarative bonus verb: a var may declare `countdown_in = {states...}`; the
firmware then decrements it once per second (drift-free) while in those
states. This deletes the hand-rolled `tickGameTime()` from every ruleset and
also feeds the totem watchdog (`time_left_var`).

---

## 3. Data across the boundary each 10 ms tick

Principle: **push events with scalar arguments, pull state through verbs,
never build Lua tables in the per-tick path.** Marshalling whole
`InputReport`/`RadioReport` structures into tables every tick would churn the
GC for data the game mostly ignores.

Per tick, in the runner's existing order:

| Phase | Crossing | Mechanism |
|-------|----------|-----------|
| Radio in (requests) | `on_message[state][msgType](vars, pkt)` | one pcall per event; `pkt` is a **reused** userdata proxy over the live `RadioPacket` (fields `sender`, `team`, `role`, `rssi`, `msg`, `len`, `pkt:byte(i)`); valid only during the call; zero allocation |
| Radio in (replies/timeouts) | `on_reply[origMsg][subType](vars, reply, orig)` | same proxy mechanism; timeouts dispatch under sub-type key `"timeout"` |
| Transitions | `rules[i].when(vars)` for rules matching the current state | condition trampolines; first match wins, runner switches display set exactly as today |
| Tick body | `update[state](vars)` | one pcall; inputs are *pulled* via verbs (`la.trigger_down(1)`, `la.shine_hit()`) so quiet ticks cost almost nothing |
| Hardware in | `la.shine_hit()`, `la.trigger_down(n)`, `la.now()` | C functions reading the current `InputReport` / polling Enlight; no copies |
| Radio out | `la.broadcast(msg, ...)`, `la.send(target, msg, ...)`, return value of `on_message` = reply sub-type | verbs append to the existing `GameOutput` buffers, flushed in phase 3 as today |
| UI out | `la.ui("Down")`, `la.ui_enlight(ms)`, `la.show(text, ms)` | verbs → `UIOutput` / tray; event names resolved to enum values once at load |
| LCD vars | — | nothing crosses: slots (§2) |

Cost estimate on the ESP32-S3 @ 240 MHz: a handful of short Lua calls per
tick (4–6 rule conditions + 1 update + rare radio events) ≈ 50–200 µs, i.e.
1–2 % of the 10 ms budget. The C++ rulesets burn most of the same budget in
`enlight`/radio work anyway, which is unchanged.

GC policy: after the `update` trampoline returns, the binding runs
`lua_gc(L, LUA_GCSTEP, 0)` — collection happens in the loop's slack window
(the runner currently spin-waits the remainder of `LOOP_MS`, so there is
guaranteed headroom). Games written to the "no tables per tick" style produce
almost no garbage; `lit_at[sender] = now` style bookkeeping is fine.

Robustness: every crossing is a `lua_pcall` with a `lua_sethook` instruction
budget (~200 k instructions). An error or runaway loop shows the message on
the tray, logs it, and forces the game into `scoring_state` instead of
freezing the device mid-match.

---

## 4. The game file format

A game file returns a single table (see `games/freeforall.lua` for a complete
example):

```lua
return {
  api           = 1,             -- binding version
  type_id       = 0x0001,        -- unique; also names the file for totems
  name          = "Free for All",

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,
  score_msg     = la.msg.SCORE_COLLECT,

  config  = { { id, name, min, max, step, default }, ... },
  vars    = { { id, default, countdown_in = {...}? }, ... },
  monitor = { { var, icon, col, row, states = {...} }, ... },
  winners = { { var, dir = "max"|"min" }, ... },

  totem_slots   = { { role = "BONUS", min = 0, max = 16, config_var = "..."? }, ... },
  teams         = 0,             -- 0 = teamless, 2..8 = team count
  time_left_var = "time_left",   -- optional, feeds totem watchdog

  on_begin   = function(vars) ... end,
  on_message = { [state] = { [msgType]  = function(vars, pkt) ... return subtype end } },
  on_reply   = { [origMsg] = { [subtype] = function(vars, reply, orig) ... end,
                               timeout   = function(vars, orig) ... end } },
  rules      = { { from, to, when = fn(vars), action = fn(vars) }, ... },
  update     = { [state] = function(vars) ... end },

  totems     = { [ROLE] = { on_activate, on_message, update, on_roster? }, ... },

  on_score_announce = function(scores) ... end,   -- optional (team games)
  on_end            = function(vars) ... end,     -- optional
}
```

Each section maps 1:1 onto an existing `LightAir_Game` field, which is what
keeps the C++ diff small:

| Lua section | Synthesized C++ |
|---|---|
| `config` | `ConfigVar[]` → slots |
| `vars` + `monitor` | `MonitorVar[]` → slots (stateMask from `states`) |
| `winners` | `WinnerVar[]` → slots |
| `on_message` | `DirectRadioRule[]` (fromState, msgType, trampoline; returned integer becomes `replySubType` — the condition/receive split of the C++ table collapses into one handler that both decides and acts) |
| `on_reply` | `ReplyRadioRule[]` (default state mask = all states except `scoring_state`) |
| `rules` | `StateRule[]` trampolines |
| `update` | `StateBehavior[]` trampolines |
| `totem_slots` | `LightAir_TotemRequirement[]` (`config_var` wires a slot into `configSecs` for the 0xF1 payload) |
| `teams` | `teamCount` + a firmware-owned `teamMap` |
| `time_left_var` | `gameTimeLeft` pointer into the slot |

### The `la` verb library

Registered by the firmware before the chunk runs. Deliberately small; grows
only when a pattern repeats across ≥2 games.

**Constants** — `la.msg.*` (RadioMsg registry), `la.role.*` (TotemRoleId),
`la.team_colors`, icon names and UI event names are validated at load.

**Identity / queries** — `la.my_id()`, `la.my_team()`, `la.team_of(id)`,
`la.player_short(id)`, `la.totem_for_role(role, idx)`, `la.state()`,
`la.now()` (millis).

**Inputs (pull)** — `la.trigger_down(n)`, `la.trigger_state(n)`,
`la.shine()` (start an Enlight burst if allowed → bool),
`la.shine_hit()` (confirmed optical hit → player id or nil),
`la.shine_ms()` (burst duration for UI sync).

**Outputs (queue, flushed in phase 3)** — `la.broadcast(msg, byte...)`,
`la.send(target, msg, byte...)`, `la.ui(event)`, `la.ui_enlight(ms)`,
`la.show(text, ms)`, `la.background(name)` / `la.background()` (Flag's
carry-alert pattern), and on totems `la.totem_ui(event, r, g, b, ...)`.

Candidates surfaced by the existing rulesets for later verbs: an RSSI
proximity gate (`pkt.rssi >= threshold` is fine in Lua, but a named
`la.near(pkt, meters)` centralizes calibration), and a per-sender rate/immunity
helper if the `lit_at` idiom proves annoying.

---

## 5. Totem behaviour in the same file

Totems are generic hardware; the role logic (today `src/totem-rulesets/*.cpp`)
moves into the game file's `totems` table, keyed by role name. The same
`.lua` file is installed on player and totem devices — a totem simply never
touches the player sections.

Runtime on the totem:

1. Idle totem beacons 0xF0 as today; the host replies 0xF1 with roleId,
   session token, time-left and optional config byte. The reply's packet
   header already carries the game's `typeId` — `LightAir_TotemActivation`
   gains a `typeId` field (one-line change in `TotemDriver`).
2. `TotemDriver` first asks the Lua game store for a file with that `type_id`;
   if found, a single generic `LightAir_LuaTotemRunner` (implements
   `LightAir_TotemRunner`) loads the file and binds `totems[roleName]`.
   If not found, the native `TotemRoleManager` path is the fallback — so
   migration can be role-by-role.
3. The runner passes each handler a persistent scratch table `t` (pre-filled
   with `t.role`, `t.team`, `t.config_secs`), calls `on_activate` / `on_message`
   / `update` on the driver's existing tick, and flushes the same
   `LightAir_TotemOutput`. `reset()` drops the scratch table and closes the
   chunk's state.

Roles that several games share (BASE, CP) are duplicated per game file at
first. That is a feature for the stated goal — a game file is fully
self-contained and exchangeable — and a `require`-able shared library on the
filesystem can come later if duplication hurts.

---

## 6. Storage and exchange

- **Filesystem**: LittleFS on the existing `default_8MB` partition scheme's
  SPIFFS partition. Games live in `/games/*.lua` (a few KB each; hundreds fit).
- **Game store** (`LightAir_GameStore`): mounts FS, scans `/games`, loads each
  file once at boot *only far enough* to read `api`, `type_id`, `name`
  (then closes the state — one `lua_State` lives at a time), caches
  `{name, typeId, path, crc16}` manifests, maps `typeId → path` for totems.
- **HTTP exchange** (`GameFileServer`): a new Settings → "Games" menu entry
  starts a SoftAP (`LightAir-<id>`) + `WebServer.h` (already declared in
  `library.properties`) with four routes: list page, upload (POST), download
  (`GET /games/<file>` — this is how players share games phone-to-device),
  delete. On exit the AP stops and WiFi returns to ESP-NOW-only. Keeping file
  management strictly pre-game avoids ESP-NOW/AP channel coexistence issues.
- **Consistency guard**: the config blob gains the game file's CRC16 after the
  session token byte. A joiner whose file differs (edited copy, older
  version) shows "Game file mismatch" instead of silently diverging
  mid-match. Native games write 0x0000.

---

## 7. C++ change list

New (≈ the entire diff; the runner core is untouched):

| File | Contents |
|---|---|
| `src/libs/lua-5.5.0/` | vendored Lua core (Makefile already carries `-Isrc/libs/lua-5.5.0/src -DLUA_32BITS`); drop `lua.c`/`onelua.c` standalone frontends |
| `src/lua/LightAir_LuaEngine.{h,cpp}` | owns `lua_State`; custom `lua_Alloc` preferring PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, internal-RAM fallback); opens base/table/string/math only (no io/os/package); pcall + instruction-budget hook; `gcStep()` |
| `src/lua/LightAir_LuaGame.{h,cpp}` | loads/validates a game file; owns the slot array; synthesizes the `LightAir_Game` descriptor (§4 mapping) with trampolines; registers the `la` verbs and the `vars` proxy; per-second countdown service |
| `src/lua/LightAir_LuaTotem.{h,cpp}` | generic `LightAir_TotemRunner` running a file's `totems[role]` section |
| `src/lua/LightAir_GameStore.{h,cpp}` | LittleFS mount, manifest scan, `typeId → path` |
| `src/tools/GameFileServer.{h,cpp}` | SoftAP + WebServer upload/download/delete |

Since exactly one Lua game is active at a time, the trampolines are a fixed
templated table (`LuaTramp<0..N>::cond/action/...`) dispatching through a
file-scope binding singleton plus the rule index — no changes to the
plain-function-pointer signatures in `StateRule`/`DirectRadioRule`/etc.

Modified:

| File | Change |
|---|---|
| `LightAir_GameManager` | registry entries become `{name, typeId, native* or path}`; add lazy `load(idx)` — Lua descriptors are synthesized on selection, not at boot |
| `LightAir_GameSetupMenu` | game list from manager entries (manifest names); call `load()` on selection before S4; append CRC16 to the config blob and verify on apply; new Settings entry launching `GameFileServer` |
| `LightAir_TotemDriver` / `LightAir_TotemRunner.h` | add `typeId` to `LightAir_TotemActivation`; runner lookup order: Lua store → native role manager |
| `sketches/LightAir/LightAir.ino` | mount FS, construct store/engine, hand them to menu (player path) and driver (totem path) |
| `src/config.h` | `namespace LuaDefaults { MAX_VARS, MAX_RULES, MAX_MSG_RULES, GAMES_DIR, INSTR_BUDGET, ... }` |
| `Makefile` | extend `SRCS` wildcard so `src/libs/**` participates in dependency tracking (arduino-cli compiles `src/**` regardless) |

Explicitly unchanged: `LightAir_GameRunner.{h,cpp}` (loop, score collection,
totem beacon replies, display bindings), `LightAir_Radio`, `DisplayCtrl`,
`UICtrl`, input stack, Enlight, the wire protocol. A Lua FreeForAll device
interoperates with a native FreeForAll device in the same match.

Footprint: Lua core ≈ 150–200 KB flash (8 MB available); a loaded game state
≈ 30–80 KB RAM, PSRAM-backed (8 MB available), hot objects cached in internal
RAM by the allocator fallback ordering if latency ever shows up in profiling.

---

## 8. Migration plan

1. **Engine + FFA** — vendor Lua, build engine/binding/store, ship
   `freeforall.lua` alongside the native games (registry supports both);
   validate 10 ms budget and radio interop native↔Lua on hardware.
2. **Totems** — `typeId` in activation, `LightAir_LuaTotemRunner`,
   BONUS/MALUS from the FFA file; native totem roles as fallback.
3. **Port the rest** — Teams, Flag (exercises teams, `on_score_announce`,
   backgrounds, flag events), KingOfHill, Outflow, Upkeep; grow verbs only as
   patterns repeat.
4. **Exchange** — `GameFileServer`, CRC guard in the config blob.
5. **Cleanup** — delete `src/rulesets/` and `src/totem-rulesets/` natives,
   drop `AllGames.cpp`/`AllTotems.cpp` registration.
