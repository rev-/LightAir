# Parked features — build them or remove them

Each entry below is code that exists, is tested and documented, and that **no
game uses**.  It was kept on purpose, as the start of a future improvement,
but it costs Lua RAM in every game (the projector library is loaded by all of
them) and it reads as a working feature to anyone who finds it.  So none of
these may stay parked forever: each one is either built out or removed, and
each entry says what either takes.

Last checked against `fd8472a` (projector tidy-up).  When an entry is
resolved, delete it from this file in the same commit.

---

## 1. Projector inventory: switching and the eviction notice

**What exists.** `games/lib/projector.lua` keeps an inventory: the baseline
(slot 1), the role projectors a game gives (VIRUS, TRIAL: `bonus = false`),
and up to `max_owned` (default 3) projectors picked up from BONUS totems,
evicted oldest-first when a fourth arrives.  `proj.next(vars)` /
`proj.prev(vars)` cycle through them; `proj.consume_evicted()` returns the
name of the one just evicted, for the tray.

**Why it is parked.** No game binds a key to `next` / `prev`, and a pickup
always puts the new projector in hand — so a player holding two or three
picked-up projectors can only ever use the last one.  No game reads
`consume_evicted`, so the eviction is silent.

**To build it.**
- A switch key in every game, best as one `std` helper called from each
  game's `update`.  A+B is the firmware's tools menu and `<`+`>` is
  FestaSportSasso's restart chord; `<` or `>` alone are free.
- Show `proj.consume_evicted()` on the tray after a grant.
- Decide whether a switch costs time: today it costs the new projector's
  `ready_ms` (SPLASH 600 ms).
- Bench: the switch cue (`ProjectorChange`) and the icon change while
  playing.

**To remove it** (one picked-up projector at a time; a new pickup replaces
it; baseline and role projectors unchanged):
- `projector.lua`: `cycle`, `P.next`, `P.prev`, `evict_oldest`,
  `evicted_name`, `P.consume_evicted`, the `max_owned` option and its `LIM`
  entry, the slots' `acquired_at`; `P.give` replaces the held picked-up
  projector instead of appending.
- Tests: the "inventory: FIFO eviction" block in `test/host/test_games.lua`;
  check the pickups section still passes.
- Docs: `docs/projector.md` §10 ("`max_owned` still applies…").

---

## 2. The RSSI plausibility gate

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
  the area service's local flag sits at `payload[4]`
  (`src/game/LightAir_AreaEffect.cpp`, `src/config.h` MSG_LIT comment).
- `std.lua`: the `cfg.reply.far` branch of `std.lit_target` and its comment.
- Tests: the gate cases in the std block of `test/host/test_games.lua`
  ("a hit beyond the shooter's gate…") and the gate value in the "payload
  carries strength, id, role and the rssi gate" block.
- Docs: `docs/projector.md` §4.

---

## 3. `range_m`, the range label

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
