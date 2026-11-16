# Parked features — build them or remove them

Each entry below is code that exists, is tested and documented, and that **no
game uses**.  It was kept on purpose, as the start of a future improvement,
but it costs Lua RAM in every game (the projector library is loaded by all of
them) and it reads as a working feature to anyone who finds it.  So none of
these may stay parked forever: each one is either built out or removed, and
each entry says what either takes.

Last checked against the `projectors` branch (game unit, effects, gates,
classes).  When an entry is
resolved, delete it from this file in the same commit.

---

## 1. The RSSI plausibility gate

**What exists.** A profile may declare `rssi_min`; `proj.payload()` sends it
as MSG_LIT `payload[3]` (a positive magnitude), and `std.lit_target` refuses
a hit read weaker than it — but only in a game that declares a `far` reply,
so the shooter is told why.  Rationale: `docs/projector.md` §4.

**Why it is parked.** No profile declares `rssi_min` and no game declares a
`far` reply, so the byte is always 0 and the branch never runs.  §4 makes it
depend on a measurement nobody has taken yet: the RSSI spread between two
players at fixed distances.

**To build it.**
- Take the measurement in `docs/projector.md` §4 "The measurement worth
  taking" (and record it in `docs/bench-checklist.md`).
- If the spread allows it, set a loose `rssi_min` on the profiles that need
  it and give each lives game a `far` reply with its `on_reply` cue.

**To remove it.**
- `projector.lua`: `rssi_min` (its `LIM` entry, the baseline field, the
  fourth value of `P.payload`).  Keep MSG_LIT `payload[3]` as a reserved 0:
  the bytes after it have fixed places (`LitPayload` in `src/config.h`).
- `std.lua`: the `cfg.reply.far` branch of `std.lit_target` and its comment.
- Tests: the gate cases in the std block of `test/host/test_games.lua`
  ("a hit beyond the shooter's gate…") and the gate value in the "payload"
  block.
- Docs: `docs/projector.md` §4.

---

## 2. `range_m`, the range label

**What exists.** Every profile carries `range_m` (BASE 40 m, FAST 30 m, LONG
none, STRONG and SPLASH 40 m), clamped at load like every field.

**Why it is parked.** Nothing reads it.  `proj.result()` deliberately gates
nothing on distance (`docs/projector.md` §2: Enlight's distance estimate is
not yet trusted), and no screen shows it.

**To build it.**
- Show it, e.g. on the pickup line ("BONUS LONG ∞"), or
- gate on it once the distance estimate is trusted (§2 "Open questions").

**To remove it.**
- `projector.lua`: the `LIM` entry, the baseline field, the catalogue's
  fields and the comments that call it a label.
- Tests: `range_m` in the profile field lists and the "range is a label"
  block of `test/host/test_games.lua`.
- Docs: `docs/projector.md` §2 "`range_m` is a label" and the "range label"
  column of the catalogue table (§6).

---

## 3. Everything the stock games do not use yet

**What exists.**  The projector and `std` libraries now carry a set of
mechanics that are tested and documented (`docs/projector.md` §3, §12–§15)
and that **no flashed game declares**: every stock game still plays as it
did.

| Mechanic | Where | Tests (`test/host/test_games.lua`) |
|---|---|---|
| `game_unit` above 1, `min_unit` | `projector.lua` | "game_unit" |
| `kind`, run-time kinds and effects | `projector.lua` | "payload" |
| fire modes, `charge`, `lock`, `spinup`, `overheat_ms`, `uses`, `lifetime_ms`, `on_lit_refund`, `self_cost` | `projector.lua` | "fire modes", "charge", "the lock", "uses, lifetime, …" |
| the receiver-side effects other than `splash` (`restore`, `dim`, `mute`, `expose`, `siphon`, `beacon`, `ignore_immunity`, `silent`) and `MSG_TRACE` | `projector.lua`, `std.lit_target` | "effects at the receiver" |
| gates on the reload and respawn waits, the four `Gate…` UI events, the bar's marker | `std.gates`, `std.respawn_gates`, `LightAir_UICtrl`, `LightAir_DisplayCtrl` | "gates"; `test_luagame.cpp` §15, §15a |
| player classes, weightless areas | `std.classes`, the area service | "player classes" |

Two things here are **in use**, and are not parked: the `<` / `>` switch
keys with the eviction notice (every game with pickups), and the new LIT
layout itself.

**Why it is parked.**  These are the game designer's building blocks; the
games that use them have not been written.  Until one is, nothing here has
been played, and the four UI cues and the marker have not been seen or
heard on a device (`docs/bench-checklist.md`).

**What it costs meanwhile.**  Measured on a 64-bit host with the libraries
stripped of debug information as on the device: **+10.3 KB of Lua per
loaded game** against `main` (freeforall 13.9 → 24.2 KB, upkeep 36.2 →
46.4 KB on that model).  On the 32-bit device that is an estimated 6–7 KB,
not measured.  The display binding table grew by about 2 KB of static RAM
for the marker.  `docs/bench-checklist.md` §1–§2 (the free heap on a real
device) decides whether that is affordable; if it is not, the candidates
to drop first are the ones the designer ends up not using.

**To build it.**  Write the games: a fine one (`game_unit = 10`) is where
the lock, the charge and the fractional strengths have room, and a class
game is where kinds mean something.  Bench the gate cues and the marker.

**To remove a mechanic.**  Each is a field read in `prepare()` / `tick()` /
`result()` / `landed()` of `projector.lua`, or one function of `std.lua`
(`gates`, `respawn_gates`, `classes`), with its own test block named above
and its own section of `docs/projector.md`.  An effect's bit stays reserved
once it has shipped: never reuse it.
