# Projector Profile — Design Proposal

> Design document only. No implementation is included; every code block below is a
> sketch meant to be argued with, not a patch.

## Goal

Introduce a **Projector**: the profile of the light-beam device a player carries.
It bundles the parameters that today are either hardcoded in `Enlight` member
initialisers or set imperatively by individual rulesets, and turns them into a
named, selectable, extensible object — so that a game can offer several
projectors, hand them out as totem rewards or quest unlocks, and let the player
switch between them.

Standard projectors (`BASE`, `FAST`, `LONG`, `STRONG`) are globally defined and
usable by any ruleset with no declaration, exactly as `LightAir_UICtrl`'s
standard `_actionTable` entries are. A ruleset may additionally declare a small
number of custom projectors, as `defineCustomAction()` allows for UI events.

---

## 1. Layering — the single most important decision

The parameters listed in the request do not belong to one consumer. They split
cleanly into three groups, and keeping that split visible is what stops the
Projector from becoming a god-object wired into everything:

| Group | Fields | Consumer | Lives in |
|---|---|---|---|
| **Optical** | `cycles`, `cooldownMs`, `minSignalPct` | `Enlight` | pushed via existing setters |
| **Game** | `strength` (damage), `roleTag`, `energyCost` | ruleset logic + the radio packet | read by rules |
| **Identity** | `id`, `name` | menu, display, UI event | read by UI |

Three consequences follow, and they define the whole architecture:

1. **`Enlight` never learns what a Projector is.** It gains one new setter
   (`setMinSignalPct`) and keeps `setRepetitions` / `setCooldown`. Something
   above it decomposes the profile into those three calls. `Enlight.h` does not
   include the projector header, and the optical layer stays independently
   testable — which matters because `EnlightCalibRoutine` and `EnlightTestMode`
   must be able to drive it with fixed, projector-free settings.

2. **Damage never travels as a table lookup.** It travels as a byte in the
   `MSG_LIT` payload (§7). No projector definition ever has to be distributed
   over the radio, which is what makes ruleset-local custom projectors work at
   all without touching the config-blob format.

3. **Selection is a queued output, not a direct call.** Rulesets ask for a
   switch through `GameOutput`, symmetric with `out.radio` and `out.ui`, and
   `LightAir_GameRunner` applies it in the OUTPUT phase (§6). Besides matching
   the existing three-phase-loop discipline, this fixes a real hazard: changing
   `_repetitions` while a run is in flight would corrupt the measurement in
   progress.

---

## 2. The structure

```cpp
// src/game/LightAir_Projector.h
struct Projector {
    uint8_t     id;             // ProjectorId value; index into the registry
    const char* name;           // <= ProjectorLimits::NAME_LEN-1 chars, shown on the OLED

    // ---- optical: pushed into Enlight ----
    uint16_t    cycles;         // = Enlight::setRepetitions(); burst = cycles * MS_PER_REP
    uint16_t    cooldownMs;     // = Enlight::setCooldown(); dead time after each measurement
    uint16_t    minSignalPct;   // range gate, % of this device's calibrated far threshold

    // ---- game: read by rules, sent on the wire ----
    uint8_t     strength;       // lives/energy taken from the target on a confirmed hit
    uint8_t     roleTag;        // 0 = generic; index into a per-game damage table (§7.3)
    uint8_t     energyCost;     // energy consumed per shot; 0 = free, 1 = today's behaviour

    uint8_t     reserved[2];    // future fields; keeps the struct at 16 bytes
};
```

### 2.1 Fire rate and cooldown are already the same knob

`Enlight::poll()` keeps `_active == true` for the whole cooldown window, and
`run()` returns `false` while `_active`. **Cooldown is therefore already the
fire-rate limiter** — a separate `fireRateMs` field would be a second name for
the same behaviour and the two would inevitably disagree. The shot-to-shot
period is:

```
period = cycles * EnlightDefaults::MS_PER_REP + cooldownMs
```

which gives a pleasant emergent property: `LONG`, by asking for many cycles, is
*automatically* slower without anyone tuning a rate for it.

The one thing this collapse loses is a cooldown that differs between a hit and a
miss (common in tag games: a successful shot costs you more). If that is wanted,
add `hitCooldownMs` later and have `poll()` pick between the two based on the
result status — a three-line change. **Recommendation: ship one cooldown, add
the second only when a ruleset actually asks for it.**

### 2.2 The range gate must be a ratio, not an absolute

"Distance" is implemented, as the request says, as a minimum signal level — but
the number must not be absolute. `classify()` already compares the accumulated
far correlator sums against `_cal.thresh_far_{r,g,b}`, which are *per-device*
calibration values (the peak return from a white diffusing wall). Two guns
calibrated on different days have different absolute numbers for the same
physical range. A projector table is shared source code compiled into every
device, so an absolute threshold would mean a different range on every unit.

Expressing the gate as a **percentage of the device's own calibrated far
threshold** makes the table portable and calibration-independent:

```cpp
// in classify(), replacing the bare _cal.thresh_far_* comparison
const float gate = baseScale * (_minSignalPct / 100.0f);
if (_rout < (long long)(_cal.thresh_far_r * gate) &&
    _gout < (long long)(_cal.thresh_far_g * gate) &&
    _bout < (long long)(_cal.thresh_far_b * gate))
    return {EnlightStatus::LOW_POW, 0};
```

`100` means "no gating beyond calibration" and is the floor: a projector can
raise the threshold (shorter reach) but never lower it below the calibrated
noise floor, so no projector can be defined that claims to see through noise.
Note the inversion when reading the table — **higher `minSignalPct` = shorter
reach**.

Three notes on the physics, which the field name should not overclaim:

- Return power also depends on target colour, albedo and incidence angle, so the
  gate is an approximate range limit, not a metric one. Call the field
  `minSignalPct`, and document "approximate max range" in the comment — don't
  call it `rangeMeters`.
- The emitted power is fixed by the PDM amplitude, so **no projector can reach
  further than `BASE` by raising power.** Extra reach comes only from coherent
  integration: more cycles accumulate signal ∝ N against noise ∝ √N, so `LONG`
  genuinely detects reliably at ranges where `FAST` is intermittent, even though
  both compare against the same normalised per-cycle threshold.
- The adaptive low-power PDM path already renormalises the accumulators to
  full-power equivalents (`_cycleNormScale`), so the percentage gate is
  consistent with it. It engages on saturation, i.e. very close targets — the
  opposite end from where the gate acts. No interaction.

A gated-out shot returns `LOW_POW`, which rulesets can already distinguish from
`NO_HIT`. "Out of range" feedback comes for free; no new `EnlightStatus` needed.

### 2.3 A naming note

The repo's nonviolent-semantics guideline argues against a field literally named
`damage`. `strength` is used above: it reads correctly next to the `STRONG`
standard, and unlike `power` or `intensity` it doesn't collide with the optical
vocabulary (`LOW_POW`, correlator power). `drain` is another candidate and
matches Outflow's energy language. Trivial to rename — flagged because the
choice propagates into the wire format comment and the ruleset API.

---

## 3. Limits and clamping

### 3.1 What actually moves where

A correction to the premise, because it changes the work: **`config.h` does not
currently hold any projector parameters.** The values that become the `BASE`
projector live elsewhere:

| Value | Today | Becomes |
|---|---|---|
| `_repetitions = 10` | `Enlight.h:164` member initialiser | `BASE.cycles` |
| `_cooldown = 0` | `Enlight.h:160` member initialiser | `BASE.cooldownMs` |
| `setCooldown(20); setRepetitions(20)` | `GameOutflow.cpp:208-209` | an Outflow custom projector, or a standard |
| implicit `lives--` | every ruleset's `onLit*` | `BASE.strength = 1` |

Everything projector-shaped in `EnlightDefaults` — `MS_PER_REP`,
`AFE_STARTUP_MICROS`, `SAT_*`, `LED_FREQ_HZ` — is a *hardware* constant, not a
profile parameter, and stays exactly where it is. `MS_PER_REP` in particular
remains the multiplier that turns `cycles` into milliseconds.

Also worth retiring while in the area: `EnlightCalib::limpow` ("min rawsum for
classification") is still loaded and saved by `nvs_config.cpp` but **no longer
read by `classify()`** — it is dead. It was the ancestor of exactly this range
gate, so either repurpose the NVS key or drop it rather than leaving two
half-implemented notions of a signal floor.

### 3.2 New limits in `config.h`

```cpp
namespace ProjectorLimits {
    constexpr uint16_t MIN_CYCLES     = 1,   MAX_CYCLES     = 60;
    constexpr uint16_t MIN_COOLDOWN   = 0,   MAX_COOLDOWN   = 10000;   // ms
    constexpr uint16_t MIN_SIGNAL_PCT = 100, MAX_SIGNAL_PCT = 5000;    // 100 = calibration floor
    constexpr uint8_t  MIN_STRENGTH   = 0,   MAX_STRENGTH   = 10;
    constexpr uint8_t  MAX_ENERGY_COST = 10;
    constexpr uint8_t  MAX_CUSTOM     = 4;   // custom slots per ruleset
    constexpr uint8_t  NAME_LEN       = 8;   // fits the OLED cell width
}
```

`MAX_CYCLES` is the one that needs a hardware answer rather than a guess: the
AFE is powered for the entire run (`AFE_ON` is raised in `run()` and dropped
when `_repsRemaining` hits 0), so `cycles` is directly LED-on time and therefore
battery drain and AFE heating. 60 cycles ≈ 480 ms of continuous emission — pick
the real ceiling from the thermal/current budget, not from playability.

### 3.3 One clamp function, both paths

```cpp
constexpr Projector projectorClamp(Projector p);   // clamps every field
```

Making it `constexpr` lets the **same** function clamp the standard table at
compile time and custom ruleset tables at registration time. One definition of
the bounds logic, no possibility of the two drifting. At registration, emit an
`ESP_LOGW` when a clamp actually bites, so a ruleset author who wrote
`cycles = 200` finds out rather than silently getting 60.

### 3.4 Optional: a balance check

A cheap guard against an accidental god-projector, since custom projectors are
open to contributors:

```
strength * 1000 / (cycles * MS_PER_REP + cooldownMs)  <=  ProjectorLimits::MAX_DPS
```

Warn-only (log and accept), not a hard clamp — a ruleset may legitimately want an
unbalanced projector for a boss role or a one-shot quest reward. Listed as a
possible addition, not a requirement.

---

## 4. The standard table

Modelled directly on `LightAir_UICtrl::_actionTable`: a `constexpr` array indexed
by a plain `uint8_t` id namespace, so the same value works as an array index, a
menu selection, a radio payload byte and a totem reward id with no enum-class
conversions anywhere.

```cpp
namespace ProjectorId {
    constexpr uint8_t BASE      = 0;
    constexpr uint8_t FAST      = 1;
    constexpr uint8_t LONG      = 2;
    constexpr uint8_t STRONG    = 3;
    constexpr uint8_t STD_COUNT = 4;
    constexpr uint8_t CUSTOM1   = 4;   // .. CUSTOM4 = 7
    constexpr uint8_t COUNT     = STD_COUNT + ProjectorLimits::MAX_CUSTOM;  // 8
}
```

| | cycles | burst | cooldown | period | minSignalPct | strength | character |
|---|---|---|---|---|---|---|---|
| `BASE`   | 10 | 80 ms  | 0 ms   | 80 ms  | 100 | 1 | today's behaviour, exactly |
| `FAST`   | 4  | 32 ms  | 60 ms  | 92 ms  | 400 | 1 | quick, short reach |
| `LONG`   | 30 | 240 ms | 400 ms | 640 ms | 100 | 1 | slow, maximum reach |
| `STRONG` | 15 | 120 ms | 900 ms | 1020 ms| 150 | 3 | slow, heavy |

These numbers are a starting point for playtesting, with one exception that is
an architectural commitment rather than a tuning value:

> **`BASE` is defined as exactly today's behaviour.** `cycles = 10` and
> `cooldownMs = 0` are the current `Enlight` member initialisers; `strength = 1`
> is the implicit `lives--`; `minSignalPct = 100` is the unmodified calibration
> comparison. Selecting `BASE` is therefore a provable no-op, every existing
> ruleset keeps its current feel with no edit, and the whole feature can land
> without a behavioural regression anywhere.

`FAST`'s short reach is not an arbitrary nerf — with 4 cycles the integration
gain is genuinely lower, so gating it explicitly makes the profile honest about
what it can detect instead of letting it produce unreliable long-range hits.

---

## 5. Custom projectors in a ruleset

The pre-game menu must be able to enumerate a game's projectors *before*
`onBegin` runs, so a `defineCustomProjector()` call in `onBegin` (the direct
analogue of `defineCustomAction`) is too late. It has to be declarative data on
`LightAir_Game`, like `configVars` and `totemRequirements`.

Adding four fields to `LightAir_Game` would mean four new lines in all six
positional ruleset initialisers. **One pointer to a sub-struct instead**, with
`nullptr` meaning "standards only, start on `BASE`" — which is the requested
"if no projectors are defined in a ruleset, just use the default definition":

```cpp
struct ProjectorSet {
    const Projector* custom;       // extra profiles; nullptr = none
    uint8_t          customCount;  // <= ProjectorLimits::MAX_CUSTOM
    uint16_t         catalogMask;  // bit i = ProjectorId i exists in this game
    uint8_t          startId;      // profile every player begins with
};

// in LightAir_Game, one new field:
const ProjectorSet* projectors;    // nullptr = { nullptr, 0, 1u<<BASE, BASE }
```

Usage in a ruleset stays at the density the codebase is written for:

```cpp
static const Projector customProjectors[] = {
    //  id                 name      cycles cooldown  minSig  str role energy
    { ProjectorId::CUSTOM1, "SNIPER",   40,     900,     100,   2,  0,   3 },
    { ProjectorId::CUSTOM2, "SPRAY",     3,      40,     900,   1,  0,   1 },
};

static const ProjectorSet projectorSet = {
    customProjectors, 2,
    (1u<<ProjectorId::BASE) | (1u<<ProjectorId::FAST) |
    (1u<<ProjectorId::CUSTOM1) | (1u<<ProjectorId::CUSTOM2),
    ProjectorId::BASE,
};
```

`catalogMask` does triple duty: it is the list the pre-game menu offers, the set
a totem or quest may grant, and the validity check on any runtime `select()`.
At runtime `ProjectorCtrl` keeps a live `unlockedMask`, initialised to
`1 << startId`; unlocks set bits, and a `select()` of a locked or non-catalogued
id is rejected and logged rather than silently applied.

---

## 6. Selecting and switching

### 6.1 The queue

```cpp
struct ProjectorOutput {           // third member of GameOutput
    void select(uint8_t id);       // switch now (if unlocked)
    void unlock(uint8_t id);       // add to unlockedMask, don't switch
    void grant(uint8_t id);        // unlock + select
    void next();                   // cycle through unlockedMask — for a keypad binding
};
```

`LightAir_GameRunner::flushOutput()` applies it after all logic has run:

1. Validate against `catalogMask` / `unlockedMask`.
2. If `enlight.isActive()`, hold the switch pending and retry next cycle —
   never reconfigure a measurement in flight.
3. Push `cycles`, `cooldownMs`, `minSignalPct` into `Enlight`.
4. Copy `name` into the bound display buffer.
5. Queue `UIEvent::ProjectorChange`.

That list is the entire integration surface. Everything else — the menu, the
totems, the quests — reduces to producing one of those four calls.

### 6.2 Pre-game menu, for free

The cheapest possible answer, and the one worth taking: **the pre-game choice is
an ordinary `ConfigVar`.** It already renders in S4a, is already serialised into
the config blob, and is already broadcast to every player, so every device starts
the game with the same projector and no new screen, no new blob field and no new
message type is needed.

The only thing missing is that S4a would show a bare integer. That is worth
fixing generically rather than with a projector special case — one optional
field on `ConfigVar`, useful to any game with an enumerated setting:

```cpp
struct ConfigVar {
    const char* name;
    int*        value;
    int         min, max, step;
    const char* const* labels;   // NEW, optional: labels[value-min] instead of the number
};
```

Then a ruleset writes `{ "Projector", &startProj, 0, 3, 1, projectorLabels }` and
gets a named picker with no menu code at all. A dedicated S4d screen should only
be built if per-player different starting projectors turn out to be wanted —
which is a bigger question (see §11).

### 6.3 From a totem

`TotemRoleId::BONUS` exists, beacons, and **no ruleset currently consumes it** —
this is its first real use. The reply path is already wired end to end, so
granting a projector needs no new message type:

```cpp
static void onBonusClaimed(const RadioPacket& pkt, LightAir_DisplayCtrl& d, GameOutput& out) {
    out.proj.grant(ProjectorId::STRONG);
    d.showMessage("STRONG projector!", 2000);
}
```

If different bonus totems should grant different projectors, put the id in the
beacon's `payload[1]` — `payload[0]` is already "0 when ready", so the extension
is backward compatible with existing totem firmware.

### 6.4 From game events (quests)

No framework support needed at all. A `StateRule` condition already sees
whatever the ruleset tracks, and its `onTransition` gets a `GameOutput`:

```cpp
static bool earnedStrong(const InputReport&, const RadioReport&) { return points >= 10; }
static void giveStrong(LightAir_DisplayCtrl& d, GameOutput& out) {
    out.proj.grant(ProjectorId::STRONG);
    d.showMessage("Quest complete", 2000);
}
```

### 6.5 In-game manual switching

`StateBehavior` already receives `InputReport.keyEvents`, so a keypad binding is
two lines in the behavior — `if (key == '>' && state == RELEASED) out.proj.next();`
No new plumbing.

---

## 7. Damage on the wire

### 7.1 The problem

Damage is applied by the **receiver** (`lives--` in `onLitTaken`), but the
projector belongs to the **sender**. Something has to cross the radio.

### 7.2 Payload, not table lookup

`MSG_LIT` is currently sent with no payload (`out.radio.sendTo(r.id, MSG_LIT)`),
and `DirectRadioRule::condition` / `onReceive` both already receive the full
`RadioPacket`. So:

```
MSG_LIT payload[0] = strength     (0 = "use the receiver's default", i.e. 1)
MSG_LIT payload[1] = projectorId  (display/telemetry: "shone by a STRONG projector")
MSG_LIT payload[2] = roleTag      (reserved for the per-game table, §7.3)
```

Sending the value rather than an id is what keeps ruleset-local custom
projectors working without extending the config blob, and it is what the role
table needs anyway. Sending the id *as well* costs one byte and buys
receiver-side feedback and post-game telemetry.

**Backward compatibility is the migration path:** a receiver must treat
`payloadLen == 0` as `strength = 1`. Old and new firmware then interoperate, and
rulesets can be converted one at a time.

The one real per-ruleset edit this forces is the elimination threshold. Today:

```cpp
static bool litAndTaken(const RadioPacket& pkt) { return lives >  1 && notImmune(pkt); }
static bool litAndShone(const RadioPacket& pkt) { return lives <= 1 && notImmune(pkt); }
```

which must become `lives > dmg(pkt)` / `lives <= dmg(pkt)` with a shared helper
`static inline int dmg(const RadioPacket& p) { return p.payloadLen ? p.payload[0] : 1; }`.
Six rulesets, two lines each — mechanical, but it must be done everywhere or
`STRONG` silently behaves like `BASE`.

### 7.3 The role field, deliberately not implemented yet

The requested "role for which a damage table can be defined for each game" works
because the sender cannot know the receiver's role, but the receiver knows it:

```cpp
// entirely inside a ruleset; no framework support
static const uint8_t dmgTable[PROJ_ROLE_COUNT][PLAYER_ROLE_COUNT] = { ... };
// on receive: dmg = dmgTable[pkt.payload[2]][myRole];  else pkt.payload[0]
```

**Recommendation: reserve `payload[2]` now, ship nothing else.** The byte costs
nothing, and the table's shape should be decided by the first game that actually
needs one rather than guessed at in the framework.

---

## 8. UI and display linkage

Deliberately the smallest possible surface, reusing what exists:

- **Notification.** Add `ProjectorChange` to `LightAir_UICtrl::UIEvent` and a row
  to `_actionTable`. The enum is already sized by `UIEvent::Count`, so this is an
  additive change. (`RoleChange` could be reused to save a slot; a distinct event
  is clearer and the table has room.) Rulesets wanting a per-projector signature
  sound still have `Custom1..4`.
- **Name on the OLED.** `MonitorVar::Str("Proj", projectorName, 1u<<IN_GAME, ICON_ROLE, c, r)`
  bound to a buffer that `ProjectorCtrl` rewrites on every switch. Zero new
  display code. One wart: `MonitorVar` tables are `static const`, so the pointer
  must be a compile-time constant — the buffer therefore has to be a global
  (`extern char projectorName[ProjectorLimits::NAME_LEN];`) rather than a
  `ProjectorCtrl` member. That matches how `enlightPtr` is already exposed, but
  it is a global, and the alternative (having the runner register the binding
  itself) trades it for asymmetry with every other monitor var. Suggest living
  with the global.
- **Cooldown bar.** `DisplayCtrl::bindCooldownVariable(..., cooldownTimeMs, ...)`
  already exists and is unused by the rulesets; the active projector's
  `cooldownMs` is exactly its argument.
- **Burst indication, already correct.** Rulesets call
  `out.ui.triggerEnlight(enlightPtr->cycleTime())`, and `cycleTime()` is
  `_repetitions * MS_PER_REP` — so the UI burst length tracks the projector's
  `cycles` automatically, with no ruleset edit at all.

---

## 9. Changes to `Enlight`

The complete list, kept small on purpose:

1. `void setMinSignalPct(uint16_t pct)` + `uint16_t _minSignalPct = 100;`
2. In `classify()`, scale the existing `thresh_far_*` comparison by
   `_minSignalPct / 100` (§2.2). One multiplication; the logic shape is unchanged.
3. Defensive `if (_active) return;` at the top of the three setters, belt-and-braces
   behind the runner's own "don't switch mid-run" rule.

Everything else — `setRepetitions`, `setCooldown`, `cycleTime()`,
`EnlightStatus::COOLDOWN` — already exists and needs no change.

**Ownership invariant to write down and keep:** Enlight's optical settings are
owned by `ProjectorCtrl` while a game is running, and by the tool
(`EnlightCalibRoutine`, `EnlightTestMode`) outside a game. Calibration must keep
setting `REPS` explicitly and must never see a projector — its output is what the
`minSignalPct` ratio is measured against, so a calibration run performed through
a projector would be circular.

---

## 10. Files

| File | Change |
|---|---|
| `src/config.h` | add `ProjectorLimits` |
| `src/game/LightAir_Projector.h` | **new** — `Projector`, `ProjectorId`, `ProjectorSet`, `projectorClamp()`, standard table |
| `src/game/LightAir_ProjectorCtrl.h/.cpp` | **new** — registry, `unlockedMask`, active selection, Enlight push, name buffer |
| `src/game/LightAir_GameOutput.h` | add `ProjectorOutput proj` |
| `src/game/LightAir_Game.h` | add `const ProjectorSet* projectors` |
| `src/game/LightAir_GameRunner.cpp` | register the set in `begin()`, apply the queue in `flushOutput()` |
| `src/game/LightAir_GameVar.h` | optional `labels` on `ConfigVar` (§6.2) |
| `src/game/LightAir_GameSetupMenu.cpp` | render `labels` when present |
| `src/enlight/Enlight.h/.cpp` | `setMinSignalPct` + gated `classify()` |
| `src/ui/player/LightAir_UICtrl.h/.cpp` | `UIEvent::ProjectorChange` + table row |
| `src/LightAir.h` | include the two new headers |
| `src/rulesets/*.cpp` (×6) | one `projectors` field; `dmg(pkt)` in the lit conditions |

The standard table is put in `LightAir_Projector.h` rather than `config.h` —
`config.h` is already 508 lines, and only the limits were asked to live there.
`TeamLedRhythm` sets the precedent for either choice.

---

## 11. Open questions

1. **`MAX_CYCLES`** — must come from the AFE current/thermal budget, since
   `cycles` is literally continuous LED-on time. A hardware answer, not a
   software guess.
2. **`BASE` cooldown.** Keeping it at `0` guarantees no behavioural change but
   preserves today's slightly odd free-running feel, where the shot rate is
   limited only by burst duration. If a nonzero `BASE` cooldown is wanted, it is
   a deliberate gameplay change to all six existing games and should be a
   separate, playtested commit.
3. **Naming of `strength`** (§2.3).
4. **Per-player starting projectors.** The `ConfigVar` route gives everyone the
   same one. Asymmetric loadouts (per-player, or per-team, or per-role) would
   need a real menu screen and a blob extension — worth deciding now whether
   that is a goal, because it is the one requirement that the cheap path cannot
   later absorb.
5. **Persistence across games.** An unlocked-projector bitmask in NVS would make
   totem rewards into meta-progression. It changes the NVS layout and has real
   fairness implications between players with different play histories —
   suggest explicitly leaving it out of v1.
6. **`EnlightCalib::limpow`** — dead field (§3.1). Retire or repurpose as part of
   this work rather than leaving two notions of a signal floor in the tree.
