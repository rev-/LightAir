# Lua games — design

Move every game ruleset (FreeForAll, Teams, Flag, KingOfHill, Outflow, Upkeep)
and the totem behaviours they use out of the firmware and into one `.lua` file
per game, stored on flash and exchangeable as plain files over HTTP.

All six rulesets are ported under `games/`, plus two that only exist as Lua
(Virus and FestaSportSasso):

| File | Notes |
|---|---|
| `games/freeforall.lua` | the readable reference — every idiom spelled out except the shine, which goes through the projector like everything else |
| `games/teams.lua` | teams, friendly fire, point reports, BASE respawn |
| `games/flag.lua` | flag events, carry background alert, team announce |
| `games/kingofhill.lua` | per-player CP slots, teamless BASE |
| `games/outflow.lua` | energy-only, passive drain, a projector that never recharges |
| `games/upkeep.lua` | CP ownership, text monitor var ("myPts/enemyPts") |
| `games/virus.lua` | new game: infection tag; uses a custom message id |
| `games/festasportsasso.lua` | new game: a King of Hill that never ends — 500 s turns inside one endless match, restarted by an admin A+B chord |
| `games/lib/std.lua` | pure-Lua standard library (see §"API layering") |
| `games/lib/projector.lua` | the projector object: profile, energy, recharge, range, splash, inventory (see `docs/projector.md`) |

Vocabulary rule: the API and the game files use the project's non-violent
terms — *shine* (project light), *lit* (be illuminated), *shone*
(eliminated).  No "hit/shoot/kill" anywhere in verbs, events or comments.

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
unchanged — the runner consumes the descriptor without knowing Lua exists.

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

1. Boot: game store scans `/games/stock/*.lua` then `/games/custom/*.lua`,
   extracts `{name, type_id, path}` manifests for the game list.
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
| Tick body | `update[state](vars)` | one pcall; inputs are *pulled* via verbs (`la.trigger_down(1)`, `la.shine_lit()`) so quiet ticks cost almost nothing |
| Hardware in | `la.shine_lit()`, `la.trigger_down(n)`, `la.now()` | C functions reading the current `InputReport` / polling Enlight; no copies |
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
budget (~200 k instructions). The policy is **log, notify, continue**: an
error or runaway loop makes the failed callback a no-op for that event, is
logged with a traceback, shows a throttled "Lua error!" tray notice, and the
match proceeds from the previous condition — the pcall guarantees both Lua
and C++ state stay consistent (effects a handler applied *before* failing
stand, so write handlers with the important mutation last). Faults are
counted per call-site in `LightAir_LuaGame::faultStats()` and the lifetime
total is persisted to NVS at match end, so a stricter policy (ending the
game, a "return to boxes" prompt, per-site circuit breakers) can be layered
on later in one place (`maybeEscalate()`). The single fatal case is a
failed `on_begin`: a game that cannot establish its starting condition
refuses to play.

---

## 4. The game file format

A game file returns a single table (see `games/freeforall.lua` for a complete
example):

```lua
return {
  api           = 1,             -- binding version
  type_id       = 0x0001,        -- unique game identifier
  name          = "Free for All",

  initial_state = S.IN_GAME,
  scoring_state = S.GAME_END,     -- optional; omit for a game with no end
  score_msg     = la.msg.SCORE_COLLECT,

  config  = { { id, name, min, max, step, default }, ... },
  vars    = { { id, default, countdown_in = {...}?, text = true?, len = N? }, ... },
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

  totems     = { [ROLE] = <TotemVM program table (pure data)>, ... },

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
| `on_message` | `DirectRadioRule[]` (fromState, msgType, trampoline; returned integer becomes `replySubType` — the condition/receive split of the C++ table collapses into one handler that both decides and acts).  **Returning nothing sends nothing**: see "Answering is the signal" below |
| `on_reply` | `ReplyRadioRule[]` (default state mask = all states except `scoring_state`) |
| `rules` | `StateRule[]` trampolines |
| `update` | `StateBehavior[]` trampolines |
| `totem_slots` | `LightAir_TotemRequirement[]` (`config_var` wires a slot into `configSecs` for the 0xF1 payload) |
| `teams` | `teamCount` + a firmware-owned `teamMap` |
| `time_left_var` | `gameTimeLeft` pointer into the slot |

The spec details the games rely on:

- **Text vars** — a `vars` entry with `text = true, len = N` claims a char
  slot instead of an int slot; the LCD binds it via the existing
  `bindStringVariable`, and `vars.role = "VIRUS"` copies into the buffer.
  Used by Upkeep ("myPts/enemyPts") and Virus (the role display).
- **Bars are not a monitor row.**  `DisplayCtrl::bindBarVariable` turns a
  slot into a filling gauge while its variable sits at a trigger value
  (energy during recharge, the respawn wait), but no descriptor row reaches
  it: that binding belongs to the projector object, which owns the reload
  timing, and is wired up with it.
- **`on_score_announce(scores)`** — replaces the C++ `ScoreTable` callback for
  team games.  `scores` is built once when all slots arrive (allocation is
  fine outside the tick path): an array of `{ id, team, vals = {v1, v2} }`
  in winner-var order.  `games/lib/std.lua` provides the two-team
  aggregation used by Teams, Flag and Upkeep.
- **Answering is the signal** — a handler's return value *is* the reply: an
  integer answers with that sub-type, returning nothing answers nothing at
  all, and a message no handler matches goes unanswered.  This matters
  because a totem's beacon is a broadcast every player in range hears.  A
  BASE, BONUS or MALUS hands itself to whoever answers, so an answer has to
  mean "this ruleset acted on your beacon" — when every player auto-answered
  every beacon, an uninterested player's empty reply stood in for the
  deliberate one, and the pickup went to whoever was merely nearest the
  radio.  Handlers that only observe a beacon (tracking a CP owner, ignoring
  an out-of-range base) therefore return nothing, and the ones that act
  return a sub-type.  A totem that wants *every* answer, like CP counting who
  is standing on it, gets them: `LightAir_Radio` keeps a broadcast's reply
  window open for its whole timeout instead of closing it on the first reply.
- **Custom message ids** — a game may declare its own even msgType (Virus
  uses `0x16` for infection announcements).  `typeId` + session token already
  isolate games on the wire; the only rule is to stay out of the 0xA0
  infrastructure and 0xF0 totem-protocol blocks.
- **A match with no end** — `scoring_state` and `time_left_var` are both
  optional, and leaving them out is what makes an always-on ruleset like
  FestaSportSasso possible.  No `scoring_state`: the runner never sees its
  entry condition, so it never collects scores, never floods `MSG_END_GAME`,
  never announces a winner and never arms its own end-screen A+B reboot —
  which is what leaves that chord free for the ruleset's own use.  No
  `time_left_var`: the 0xF1 activation reply reports `0xFFFF` instead of a
  countdown, so a totem activated at any moment never arms its self-revert
  watchdog.  A `countdown_in` var still ticks normally, so such a game can
  run any number of internal timed rounds inside the one endless match.

### The `la` verb kernel

Registered by the firmware before the chunk runs. Deliberately small; §"API
layering" below is the policy for what may be added here.

The input verbs read the current READ-phase `InputReport`, so they belong in
rule conditions and `update` bodies — the two crossings the runner hands it
to.  Between them they cover the whole report: the registered buttons by
index, and the keypad both by key (`key_down` / `key_state`) and by
enumeration (`key_at`), so a ruleset can act on keys it never named and no
layout knowledge lives on either side of the boundary.  A key the report
does not list is up: it lists only what is not OFF this poll, and it lists
every such key, which is what makes a chord like
`la.key_down("A") and la.key_down("B")` (FestaSportSasso's turn restart)
work at all.

**Constant tables (data, not calls; pushed once at load)** — `la.msg.*`
(RadioMsg registry), `la.flag_event.*`, `la.colors.team[0..7]`,
`la.colors.player[0..16]` (each `{r,g,b}`), `la.rhythm[0..7]`
(`{period, pulses}`).  Icon names and UI event names are strings validated
at load time.

**Identity / queries** — `la.my_id()`, `la.my_team()`, `la.team_of(id)`,
`la.player_count()` (roster size), `la.player_short(id)`,
`la.team_short(team)` (the team's label, "O"/"X"/… from the one table in
`config.h` — a game file never spells the names out for itself),
`la.totem_for_role(role, idx)`, `la.state()`, `la.now()` (millis).

**Inputs (pull)** — `la.trigger_down(n)`, `la.trigger_state(n)`,
`la.key_down(key [, keypad])`, `la.key_state(key [, keypad])` (`"off"` /
`"pressed"` / `"held"` / `"released"` / `"released_held"` — the same ladder
as a trigger; the two released states appear in exactly one poll, so a rule
condition catches a key-up edge with no bookkeeping),
`la.key_at(i)` → `key, state, keypad` (1-based over the keys the report
holds this poll, nil past the last one),
`la.shine()` (start an Enlight burst if allowed → bool.  Returns false
while a burst is in flight or cooling down, which is why every caller
spends energy *inside* the `and la.shine()` short-circuit and never
beside it),
`la.shine_lit()` (confirmed lit target → player id or nil),
`la.shine_result()` → `status, id, metres, r, ang` (the whole
measurement: `status` is `"player"` / `"no_hit"` / `"low_pow"` / `"near"` /
`"cooldown"` / `"running"` / `"idle"`, `metres` is the estimated distance
— 0 when the device has no reference calibration — and `r`/`ang` are the
white-balanced colour coordinates.  Supersedes `shine_lit` for anything
applying a policy to the measurement.  Both poll, and the poll is
read-and-clear, so a game uses one or the other, never both in a tick),
`la.shine_ms()` (burst duration for UI sync).

Enlight reports a distance and gates on nothing but its own calibrated
validity floor: range is game policy and lives in
`games/lib/projector.lua`, which is what lets a profile gate on distance,
correct for target colour, or grade an effect by range without a firmware
change for each.

**Outputs (queue, flushed in phase 3)** — `la.broadcast(msg, byte...)`,
`la.broadcast_relay(msg, byte...)` (mesh flood, resend=2),
`la.send(target, msg, byte...)`, `la.ui(event)`, `la.ui_enlight(ms)`,
`la.show(text, ms)`, `la.clear_tray()`,
`la.shine_config{reps, cooldown_ms}` (the active projector's optics.
Queued, not applied on the spot: reconfiguring Enlight mid-measurement
corrupts it, and game logic runs at an arbitrary point in the LOGIC
phase.  Outside a tick — `on_begin` — there is nothing in flight and no
queue, so it applies straight away),
`la.background(spec)` / `la.background()` (set/clear a continuous
sound+vibration+RGB alert; `spec` is a steps table, see `games/flag.lua`),
`la.shine_action(spec)` / `la.shine_action()` (give the Enlight event this
projector's own feedback, or restore the standard one; same steps table as
`la.background`.  Overriding the slot rather than adding an event is
deliberate — the runtime burst-duration override is keyed on the event
value, so a per-projector id would discard the real burst length).

The rule for which of these are queued: **defer what only leaves the
system, call directly what feeds back into this tick.**  `la.shine()`
returns the bool that decides whether energy is spent, so queuing it would
split one decision across two ticks; `la.background` and
`la.shine_action` install a definition rather than emitting an event.
No verbs run on totems: totem behaviour is TotemVM data (§5), and totem
animations are referenced by name inside those programs.

**Loader** — `la.lib(name)` runs `/games/lib/<name>.lua` once per state and
caches the result (there is no `require`/`package`).

During a **manifest peek** it returns an inert stand-in instead: a table
answering any index or call with itself.  A peek reads three literal fields
(`api`, `type_id`, `name`), but the chunk stating them is a whole game file,
and a game file pulls its libraries in at file scope — so loading them for
real would compile tens of kilobytes of Lua per file, into a state torn down
immediately afterwards, for every file in `/games`.  On a device that is
enough to exhaust the interpreter partway through the scan and drop games
from the menu.  File-scope library use therefore no-ops harmlessly while
peeking, and **manifest fields must be literals**.

The `vars` proxy is passed to every handler as the first argument *and*
installed as a global, so load-time closures (e.g. a friendly-fire predicate)
can reference it.  `pkt:byte(i)` is 1-based over the payload
(`pkt:byte(1)` = `payload[0]`); `pkt.len`, `pkt.sender`, `pkt.team`,
`pkt.role`, `pkt.rssi`, `pkt.msg` are fields.  `pkt.rssi` is the
receive-side signal strength in dBm — measured locally, never carried on the
wire — and is the only distance information a ruleset has, so every
proximity gate (BASE respawn, flag pickup, CP presence) is a comparison
against it.  Choosing the range is the **ruleset's** job, not the library's:
the same BASE role is a 2 m respawn pad in Teams and the capture point in
Flag, so `std.base_respawn` and `std.pickup_claim` require an explicit
`rssi` and error without one rather than defaulting to a number.  It is valid in `on_message` handlers and in `on_reply` for the
`reply` packet; the `orig` packet is our own send and reports 0.

## API layering — verbs vs. easy games vs. future-proofness

Making games easy to write by adding a C++ verb for every pattern would
bloat the kernel, freeze design decisions into firmware releases, and make
the API hard to learn.  The resolution is three layers with a hard rule for
what goes where:

1. **C++ kernel verbs** (the `la` table, above).  A verb is admitted only if
   it (a) touches hardware or firmware-private state (optics, display tray,
   radio queues, roster), (b) enforces a protocol invariant (reply framing,
   flood resend), or (c) cannot be made fast enough in Lua.  Everything on
   this list is a *capability*, not a policy: `la.shine()` starts a burst,
   it does not decide when shining is allowed.  Target size: ~25 verbs — one
   page, learnable in an afternoon, and stable because policies never live
   here.

2. **The Lua standard library** (`games/lib/std.lua`, and
   `games/lib/projector.lua` beside it) — recurring *game patterns* built
   only out of kernel verbs: the immunity window, the standard
   lit-handler ladder, BASE respawn gating, team score aggregation, and
   the five standard totem roles.  The projector is the largest of them
   and has its own file: the profile in hand, the energy a beam costs,
   how it comes back, how far it reaches, what a hit weighs on the wire,
   splash, and the inventory.  It ships as a file next to the games, so it can grow, be fixed,
   or be forked without reflashing firmware — and a game that wants
   different semantics simply doesn't call it.  This is where "easy to
   define new games" comes from: `games/teams.lua` is ~½ the logic of its
   C++ original because the idioms are one-liners, and
   `games/freeforall.lua` stays the readable reference by spelling out
   everything else.  The one library every ruleset takes is the projector:
   it is the only route to Enlight, because a ruleset firing or polling on
   its own would bypass the shine economy and race the projector for a
   read-and-clear poll.

3. **The game file** — only the rules that make this game *this game*.

Future-proofness falls out of the same rule.  A future game that needs a new
*pattern* (Virus needed infection tracking) writes it in Lua — no firmware
release.  Only a new *capability* (a new sensor, a new radio primitive)
needs a kernel verb, and adding one is backward-compatible: the `api` field
versions the contract, and files can feature-test with
`if la.background then ... end`.

The traffic has run the other way too.  `std.shiner` — the
trigger/energy/recharge idiom — grew into `games/lib/projector.lua` and
was deleted: the projector's baseline profile reproduces it exactly, so
the growth cost its callers two lines each.  That is the layering working
as intended.  A pattern that later proves both universal *and* hot enough
to matter could still be promoted to C++ behind the signature the library
established, but nothing has needed it: the projector runs once per 10 ms
tick against a loop that spin-waits out its own slack.

Historical candidates deliberately *kept out* of the kernel: RSSI proximity
gating (one comparison), immunity windows (a table and a clock), CP window
arithmetic (integer bit ops).  All are `std` functions instead.

---

## 5. Totem behaviour: TotemVM programs over the handshake

Totems hold **no game files** — games are shared projector-to-projector
while totems may be off, out of range, or bought yesterday.  And a totem
that wakes up *mid-game* cannot afford a long transfer on a busy channel.

The game file's `totems` table therefore contains **pure data**, not
functions: each role is a declarative state-machine program for a fixed
interpreter in the totem firmware ("TotemVM").  The projector validates the
table at game load and serializes it into the **single 0xF1 activation
reply** — activating a totem costs one packet, cold or warm, for standard
and custom roles alike.  Measured sizes: BASE 41 B, BONUS/MALUS 53 B, FLAG
91 B, CP 191 B against a 225 B budget.

`games/lib/std.lua` provides factories (`std.totems.base(0)`, `.cp()`, …)
returning these tables, so most games write one-liners;
`games/freeforall.lua` spells two programs out in full as the tutorial.
Doctrine that keeps the VM small: totems beacon, referee presence and
render — decisions live player-side, where the Lua files are.

Full model, semantics, wire encoding, versioning and failure modes:
**`docs/totem-behavior-handshake.md`**.

---

## 6. Storage and exchange

- **Filesystem**: LittleFS on the existing `default_8MB` partition scheme's
  SPIFFS partition. Games live under `/games/stock/*.lua` (firmware-owned)
  and `/games/custom/*.lua` (player-owned); libraries live in `/games/lib/`.
  A few KB each; hundreds fit. Paths are LittleFS-relative, so every read
  goes through the `LittleFS` object — never `luaL_loadfile`/`fopen`, which
  resolve against the VFS base path the Arduino core mounts LittleFS under
  and cannot see `/games/...` at all.
- **Game store** (`LightAir_GameStore`): mounts FS, seeds the embedded stock
  games into `/games/stock` and the libraries into `/games/lib`, scans
  `/games/stock` then `/games/custom`,
  and reads each file at boot *only far enough* to extract `api`, `type_id`,
  `name` (`peekManifest`, on a scratch instance — no descriptor, no
  trampoline slot).  It registers one lightweight manifest + placeholder
  descriptor per file (`GameDefaults::MAX_GAMES = 16`) plus a load hook;
  the full game — Lua state, variable slots, totem programs — is realized
  on ONE shared `LightAir_LuaGame` instance only when the menu actually
  selects it (`GameManager::load()`), and reloading a different file on the
  same instance is how switching games works.  So a menu full of games costs
  a table of ~80-byte manifests, not a table of interpreters.  The table is
  sized for the board that has to hold it: the projectors have no PSRAM, so
  every unused slot is internal RAM taken from the one Lua state sitting
  beside it.  Totems need no files at
  all: their behaviour travels as TotemVM programs in the activation reply
  (§5).
  **The directory *is* the ownership boundary.** `/games/stock` and
  `/games/lib` have no HTTP write path at all (see below), so nothing can
  ever be "edited" there in a way seeding needs to detect or protect —
  every embedded file is simply rewritten on every boot, unconditionally,
  no hashing. A ruleset a player wants to change lives in `/games/custom`
  instead, which seeding never touches; editing (or uploading) a file there
  is all it takes to change the game, and a firmware update can't reach it
  even by accident. A stock game can't be shadowed by a same-named custom
  one either: the scan visits `/games/stock` first, and the first file to
  claim a `type_id` wins. Editing `games/*.lua` **in the repo** is
  different — those reach the device through `LightAir_GamesBundle.h`,
  which `make` regenerates from the wildcard, so build with `make` rather
  than invoking `arduino-cli` directly.

  Both the ruleset and its libraries are **streamed** into the parser, one
  256-byte block at a time, never read whole.  A load compiles three files
  — the game plus `std.lua` plus `projector.lua`, ~65 KB of source — and a
  whole-file buffer is garbage only once the parser is finished with it, so
  all three used to sit in RAM together on top of the prototypes they were
  becoming.  On a board with PSRAM nobody notices; the projectors are an N4
  part with none, and there that peak is what decides whether a game loads
  at all.  Chunks are also loaded **text-only**: nothing here ships
  precompiled Lua, and undumping bytecode is a way out of a sandbox that
  has already had `load`/`loadfile`/`dofile` removed.
  A realize that fails carries its **reason** back with it —
  `LightAir_LuaGame::loadError()` → the hook's `errOut` →
  `GameManager::lastLoadError()` → the setup menu's failure screen — because
  the people who hit it are the ones editing `.lua` files and uploading them
  over WiFi, and the serial log they cannot see is the wrong place for the
  only copy of the message.  The reason is the Lua error's first line with
  the directory stripped ("`festasportsasso.lua:52: library 'projector' not
  found`"), which also names the two failures a bare "Game failed to load"
  once left to be guessed at from the bench: a missing library and an
  exhausted heap.  A successful load then logs the ruleset's cost —
  `loaded 'X' (typeId 0x8, 57 KB of Lua)` — after a full collection, so
  serial answers "how much of the device does this game take" directly.
- **HTTP exchange** (`GameFileServer`): the Settings → "Share games" menu
  entry (next to Calibration and ID/DM) starts a SoftAP
  (`LightAir-<PLAYERSHORT>`, password `lightair`) + `WebServer.h` on
  `http://192.168.4.1/`, serving one page that both **sends** games (every
  `/games/custom/*.lua` listed with a download link, served as a `.lua`
  attachment) and **receives** them (multipart upload into `/games/custom`;
  name-sanitized, `.lua`-only, 64 KB cap), plus delete.  `/games/stock` and
  `/games/lib` are never listed, downloadable, uploadable to, or
  deletable through this server — there is no `d`/directory parameter any
  more, every route is hardcoded to `/games/custom`, so stock rulesets and
  libraries are structurally unreachable from here, not just protected by
  convention.  The LCD shows SSID/password/URL and the joined-station
  count; exiting reboots the device — SoftAP displaced the ESP-NOW radio
  and the game list may have changed, and a reboot restores the radio and
  rescans `/games/stock` + `/games/custom` in one stroke.  Keeping file
  management strictly pre-game avoids ESP-NOW/AP channel coexistence
  issues.
- **Consistency guard (designed, NOT yet implemented)**: the config blob
  would gain the game file's CRC16 after the session token byte, so a
  joiner whose file differs (edited copy, older version) shows "Game file
  mismatch" instead of silently diverging mid-match.  As of today
  `game_serialize_config` / `game_apply_config`
  (`LightAir_GameSetupMenu.{h,cpp}`) carry no CRC — matching typeId is
  the only check — so keep game files in sync via the share server.

---

## 7. C++ change list

Status: implemented (Lua 5.5.1 vendored under `src/libs/lua-5.5.0/`; the
engine, binding, TotemVM and store below exist and are host-tested — every
game file loads through the real binding, and a scripted FFA session runs
begin/messages/replies/rules/updates on host).  `GameFileServer` (HTTP
exchange) and lazy manifest loading are implemented too.  Still open: the
S4c vmVersion check, the config-blob CRC16 consistency guard (§6), and
on-target validation, which needs real hardware.

New (≈ the entire diff; the runner core is untouched):

| File | Contents |
|---|---|
| `src/libs/lua-5.5.0/` | vendored Lua core (Makefile already carries `-Isrc/libs/lua-5.5.0/src -DLUA_32BITS`); drop `lua.c`/`onelua.c` standalone frontends |
| `src/lua/LightAir_LuaEngine.{h,cpp}` | owns `lua_State`; custom `lua_Alloc` preferring PSRAM (`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, internal-RAM fallback); opens base/table/string/math only (no io/os/package); pcall + instruction-budget hook; `gcStep()` |
| `src/lua/LightAir_LuaGame.{h,cpp}` | loads/validates a game file; owns the slot array; synthesizes the `LightAir_Game` descriptor (§4 mapping) with trampolines; per-second countdown service |
| `src/lua/LightAir_LuaKernel.cpp` | the `la` verb table and the `vars`/`pkt` proxies (same class, split along its seams; shared glue in `LightAir_LuaGameInternal.h`) |
| `src/lua/LightAir_TotemEncoder.cpp` | serializes a game's `totems` tables into TotemVM program bytes at load time |
| `src/totem/LightAir_TotemVM.{h,cpp}` | fixed state-machine interpreter (`LightAir_TotemRunner`) executing programs received in the 0xF1 reply; see `docs/totem-behavior-handshake.md` |
| `src/lua/LightAir_GameStore.{h,cpp}` | LittleFS mount + stock-game seeding, manifest scan, lazy realize hook (one shared loaded instance) |
| `src/tools/GameFileServer.{h,cpp}` | Settings → Share games: SoftAP + WebServer download/upload/delete of `.lua` files |

Since exactly one Lua game is active at a time, the trampolines are a fixed
templated table (`LuaTramp<0..N>::cond/action/...`) dispatching through a
file-scope binding singleton plus the rule index — no changes to the
plain-function-pointer signatures in `StateRule`/`DirectRadioRule`/etc.

Modified:

| File | Change |
|---|---|
| `LightAir_GameManager` | registry entries become `{name, typeId, native* or path}`; add lazy `load(idx)` — Lua descriptors are synthesized on selection, not at boot |
| `LightAir_GameSetupMenu` | game list from manager entries (manifest names); call `load()` on selection before S4; new Settings entry launching `GameFileServer` (the CRC16 blob guard is designed but not implemented — see §6) |
| `LightAir_GameRunner` (beacon intercept only) | 0xF1 reply gains `[vmVersion][progLen][program]` for Lua-defined roles; program bytes come from the Lua binding's serializer |
| `LightAir_TotemDriver` / `LightAir_TotemUICtrl` | 0xF1 activation is VM-form only (the role manager and native runners are gone); `Control` effect gains the slot-based arg form |
| 0xF0 beacon / `LightAir_GameSetupMenu` S4c | beacon advertises `[fw api, vmVersion]`; totem-assignment screen checks compatibility at setup time |
| `LightAir.ino` (repo root — the sketch the Makefile and CI build) | mount FS, construct store/engine, hand them to menu (player path) and driver (totem path) |
| `src/config.h` | `namespace LuaDefaults { MAX_VARS, MAX_RULES, MAX_MSG_RULES, STOCK_DIR, CUSTOM_DIR, LIB_DIR, INSTR_BUDGET, ... }` |
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
2. **Totems** — `LightAir_TotemVM` interpreter + program serializer +
   extended 0xF1 (`docs/totem-behavior-handshake.md`); BONUS/MALUS from the
   FFA programs first; native totem roles as fallback.
3. **Port the rest** — Teams, Flag (exercises teams, `on_score_announce`,
   backgrounds, flag events), KingOfHill, Outflow, Upkeep; grow verbs only as
   patterns repeat.
4. **Exchange** — `GameFileServer`, CRC guard in the config blob.
5. **Cleanup** (done) — `src/rulesets/`, `src/totem-rulesets/` and the
   totem role-manager infrastructure are deleted; games exist only as
   .lua files and totem behaviour only as TotemVM programs.
