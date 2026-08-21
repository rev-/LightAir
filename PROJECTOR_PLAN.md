# Projector Profile — Design Proposal

> Design document only. No implementation is included; every code block below is a
> sketch meant to be argued with, not a patch.

## Goal

Introduce a **Projector**: the profile of the light-beam device a player carries.
It bundles the parameters that today are either hardcoded in `Enlight` member
initialisers or set imperatively by individual rulesets, and turns them into a
named, selectable, extensible object — so that a game can offer several
projectors, hand them out as totem rewards or quest unlocks, and let players
carry different ones at different moments of the same game.

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
| **Optical** | `cycles`, `cooldownMs`, `rangeM` | `Enlight` | pushed via setters |
| **Game** | `strength` (damage), `roleTag`, `energyCost` | ruleset logic + the radio packet | read by rules |
| **Identity** | `id`, `name` | menu, display, UI event | read by UI |

Four consequences follow, and they define the whole architecture:

1. **`Enlight` never learns what a Projector is.** It gains one new setter
   (`setRangeM`) and keeps `setRepetitions` / `setCooldown`. Something above it
   decomposes the profile into those three calls. `Enlight.h` does not include
   the projector header, and the optical layer stays independently testable —
   which matters because `EnlightCalibRoutine` and `EnlightTestMode` must be
   able to drive it with fixed, projector-free settings.

2. **The optics stay on the optical side of that line.** The projector says
   "15 metres"; `Enlight` owns the conversion from metres to a correlator
   threshold, because that conversion needs the calibration constants and the
   falloff exponent, which are physics, not game design (§2.2).

3. **Damage never travels as a table lookup.** It travels as a byte in the
   `MSG_LIT` payload (§7). No projector definition ever has to be distributed
   over the radio — which is what makes both ruleset-local custom projectors and
   per-player asymmetric loadouts work with no wire-format negotiation at all.

4. **Selection is a queued output, not a direct call.** Rulesets ask for a
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
    uint8_t     rangeM;         // approximate reach in metres; 0 = whatever the device can see

    // ---- game: read by rules, sent on the wire ----
    uint8_t     strength;       // lives/energy taken from the target on a confirmed hit
    uint8_t     roleTag;        // 0 = generic; the player's "role" for a per-game table (§7.3)
    uint8_t     energyCost;     // energy consumed per shot; 0 = free, 1 = today's behaviour

    uint8_t     reserved[3];    // future fields; keeps the struct at 16 bytes
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

### 2.2 Range in metres, via the inverse-cube law

The projector's reach is expressed as **`rangeM`, a linear distance in metres**.
`Enlight` converts it to a correlator threshold using the measured falloff.

**The model.** With signal falling as `1/x^EXP`, a reference measurement
`refFar` taken at a known distance `refDist` fixes the whole curve:

```
S(x)   = refFar * (refDist / x)^EXP
T(R)   = refFar * (refDist / R)^EXP        threshold for a projector of range R
```

`EXP` belongs in `EnlightDefaults` as a named float, **not hardcoded as 3**:

```cpp
constexpr float RANGE_FALLOFF_EXP = 3.0f;   // measured retroreflector falloff
```

The whole conversion collapses into one `powf` executed **once per projector
switch**, in the setter — so a non-integer exponent (2.8, 3.2) costs nothing at
runtime, and re-fitting the law against new field measurements is a one-constant
edit rather than a rewrite:

```cpp
void Enlight::setRangeM(uint8_t m) {          // 0 = no projector gate
    _rangeMul = m ? powf((float)_cal.refDistM / (float)m,
                         EnlightDefaults::RANGE_FALLOFF_EXP)
                  : 0.0f;
}
```

and `classify()` keeps exactly its current shape, with the threshold sourced from
the range instead of directly from calibration:

```cpp
const float tr = fmaxf(_cal.refFarR * _rangeMul, (float)_cal.thresh_far_r);
const float tg = fmaxf(_cal.refFarG * _rangeMul, (float)_cal.thresh_far_g);
const float tb = fmaxf(_cal.refFarB * _rangeMul, (float)_cal.thresh_far_b);
if (_rout < (long long)(tr * baseScale) &&
    _gout < (long long)(tg * baseScale) &&
    _bout < (long long)(tb * baseScale))
    return {EnlightStatus::LOW_POW, 0};
```

Three properties fall out of that `fmaxf`, and they are the reason the metric
form is safe rather than aspirational:

- **`rangeM = 0` is exactly today's behaviour.** `_rangeMul = 0` makes the first
  term vanish and the threshold reduces to the current bare `thresh_far_*`
  comparison. `BASE` uses it, so `BASE` remains a provable no-op on the optical
  side (§4).
- **The floor can only be raised, never lowered.** A projector asking for 200 m
  gets whatever the device's calibrated noise floor actually permits. No
  projector definition can claim to see through noise.
- **The real ceiling is a device property, and it is computable:**
  ```
  Rmax = refDist * (refFar / thresh_far)^(1/EXP)
  ```
  Which leads to the single most useful addition in this section — see §2.4.

### 2.3 What this costs: the reference measurement

Metric range needs a signal measured at a **known distance**. Today no such
number exists — `thresh_far_*` is a *maximum over a sweep* against a *diffusing*
white wall, so it pins no distance and describes the wrong optical target.

The good news is that the calibration routine already takes the measurement; it
just doesn't record the distance. **Step 1 already fires 50 shots at a clear
retroreflective target** and keeps the per-channel far power in
`_step1_r/g/b[]`, and **step 2 already baseline-subtracts exactly those arrays**
to compute the white-balance factors:

```cpp
// EnlightCalibRoutine.cpp, step2(), already present:
long long r = _step1_r[i] - (long long)REPS * (long long)cal.rcal;
```

So the entire new-calibration cost is:

1. Step 1's prompt gains a distance: `"Clear target at 5 m"`.
2. Step 2 saves the median of those already-computed values, divided by `REPS`,
   into three new NVS keys `ref_far_r/g/b` (matching how step 3 already
   normalises `thresh_far_*` by `REPS`).

No new calibration step, no new user interaction, ~15 lines. `refDist` itself is
best a compile-time constant in `config.h` (`ProjectorLimits::CAL_REF_DIST_M`)
printed in the prompt, since making it per-device would require entering a number
on a 6-key keypad for no real benefit.

Two honest caveats on what "metres" means here, worth putting in the header
comment rather than engineering away:

- **The gate runs before classification**, so it necessarily uses a reference
  target's reflectivity. A darker player colour returns less light and will be
  gated at a shorter true distance than a clear one. Per-colour accuracy would
  need a per-colour reference table and a post-classification gate — a sensible
  v2, overkill now.
- `1/x³` is a far-field empirical fit. At very short range the retroreflector is
  larger than the beam spot and the exponent flattens toward 2. Irrelevant here:
  the gate only ever operates at the far end of the curve.

### 2.4 Make the calibration self-validating

`Rmax` above is a number the operator can *check by walking*. Displaying it turns
the whole model from an assumption into a field-verifiable claim:

> **Add the computed `Rmax` to the calibration summary screen** (`step4()`, which
> already pages through NVS values). The operator calibrates, reads `Rmax: 38 m`,
> walks 38 m, and confirms the target stops registering. If it doesn't, the
> falloff exponent or the reference distance is wrong, and they find out at
> calibration time instead of mid-game.

This also surfaces a pre-existing tension worth measuring early: the README
claims ~40 m range, while `thresh_far_*` is the max return from a white wall as
close as contact. If `refFar / thresh_far` turns out not to support 40 m, the
existing classifier is already gating shorter than the hardware can see — a
useful thing to learn regardless of this feature.

### 2.5 A naming note

The repo's nonviolent-semantics guideline argues against a field literally named
`damage`. `strength` is used above: it reads correctly next to the `STRONG`
standard, and unlike `power` or `intensity` it doesn't collide with the optical
vocabulary (`LOW_POW`, correlator power). `drain` is another candidate and
matches Outflow's energy language. Trivial to rename — flagged because the choice
propagates into the wire-format comment and the ruleset API.

---

## 3. Limits and clamping

### 3.1 What actually moves where

A correction to the premise, because it changes the work: **`config.h` does not
currently hold any projector parameters.** The values that become the `BASE`
projector live elsewhere:

| Value | Today | Becomes |
|---|---|---|
| `_repetitions = 10` | `Enlight.h:164` member initialiser | `BASE.cycles` |
| `_cooldown = 0` | `Enlight.h:160` member initialiser | `BASE.cooldownMs` (now 10 ms — §4) |
| `setCooldown(20); setRepetitions(20)` | `GameOutflow.cpp:208-209` | an Outflow custom projector, or a standard |
| implicit `lives--` | every ruleset's `onLit*` | `BASE.strength = 1` |

Everything projector-shaped in `EnlightDefaults` — `MS_PER_REP`,
`AFE_STARTUP_MICROS`, `SAT_*`, `LED_FREQ_HZ` — is a *hardware* constant, not a
profile parameter, and stays where it is. `MS_PER_REP` remains the multiplier
that turns `cycles` into milliseconds; `RANGE_FALLOFF_EXP` joins it as a new
member of the same family.

Also worth retiring while in the area: `EnlightCalib::limpow` ("min rawsum for
classification") is still loaded and saved by `nvs_config.cpp` but **no longer
read by `classify()`** — it is dead. It was the ancestor of exactly this range
gate, so retire the key rather than leaving two half-implemented notions of a
signal floor in the tree.

### 3.2 New limits in `config.h`

```cpp
namespace ProjectorLimits {
    constexpr uint16_t MIN_CYCLES   = 1,  MAX_CYCLES   = 100;     // see §3.4
    constexpr uint16_t MIN_COOLDOWN = 0,  MAX_COOLDOWN = 10000;   // ms
    constexpr uint8_t  MIN_RANGE_M  = 1,  MAX_RANGE_M  = 100;     // 0 = device max, unclamped
    constexpr uint8_t  MIN_STRENGTH = 0,  MAX_STRENGTH = 10;
    constexpr uint8_t  MAX_ENERGY_COST = 10;
    constexpr uint8_t  MAX_CUSTOM      = 4;   // custom slots per ruleset
    constexpr uint8_t  NAME_LEN        = 8;   // fits the OLED cell width
    constexpr uint8_t  CAL_REF_DIST_M  = 5;   // distance for calibration step 1 (§2.3)
}
```

Note that `rangeM` gets **two** clamps and they do different jobs: the static one
above catches a typo in a ruleset table, and the dynamic `fmaxf` in `classify()`
(§2.2) catches a projector asking for more range than this particular device's
calibration can deliver. Neither subsumes the other.

### 3.3 One clamp function, both paths

```cpp
constexpr Projector projectorClamp(Projector p);   // clamps every field
```

Making it `constexpr` lets the **same** function clamp the standard table at
compile time and custom ruleset tables at registration time. One definition of
the bounds logic, no possibility of the two drifting. At registration, emit an
`ESP_LOGW` when a clamp actually bites, so a ruleset author who wrote
`cycles = 200` finds out rather than silently getting 40.

### 3.4 `MAX_CYCLES` is not a hardware limit — here is the evidence

**`MAX_CYCLES` is a name proposed by this document; it does not exist in the
tree.** The quantity it would bound is `Enlight::_repetitions` (default 10 at
`Enlight.h:164`), written only by `setRepetitions()` and read at `run()` into
`_repsRemaining` and by `cycleTime()`. It has exactly three callers today:
`GameOutflow.cpp:209` (20), `EnlightCalibRoutine.cpp:381` (fixed at `REPS = 5`),
and `EnlightTestMode.cpp:138` (interactive).

That last one already carries a bound — `EnlightTestMode.cpp:83`:

```cpp
const uint32_t MIN_REPS = 1, MAX_REPS = 100;
```

so a ceiling for this quantity has already been chosen once, locally, by the
tool that exercises it hardest. Whatever `ProjectorLimits::MAX_CYCLES` ends up
being, **it and `MAX_REPS` should be the same constant**, hoisted to `config.h` —
two independently-drifting ceilings on one hardware quantity is exactly the kind
of duplication this proposal exists to remove.

Traced through `Enlight`, with the numbers this hardware actually produces
(`LED_CLOCK_HZ` 16 MHz, `LED_FREQ_HZ` 1667 → `_periodClocks` 9600,
`_waveformBytes` 2400, `_goertzPeriod` 200, `_periodsPerCycle` 13,
`cycleMs` 7.8 — which is where `MS_PER_REP = 8` comes from):

| Candidate limit | Verdict |
|---|---|
| **DMA / memory** | **Not a limit.** All four buffers (2 × 31200 B LED + 2 × 15602 B ADC ≈ 94 KB) are allocated **once in `begin()`**, sized by `_periodsPerCycle`, which is itself capped by `ENLIGHT_SPI_MAX_DMA_LEN = 32767`. A repetition re-issues the *same* transaction. Memory does not scale with `cycles` at all. |
| **Accumulator overflow** | **Not a limit.** `_rout`/`_gout`/`_bout` are `long long`; worst case ≈ 2.2 × 10¹⁰ per channel per cycle, so 64 bits holds ~4 × 10⁸ cycles. `_arrayiter` is `uint32_t` at +2600/cycle → 1.6 × 10⁶ cycles. |
| **`triggerEnlight()` / `UIAction::durations[]`** | **The only hard cap in the firmware: 8191 cycles.** Both are `uint16_t` milliseconds, fed by `cycleTime() = cycles × MS_PER_REP`. Above 8191 the UI burst duration silently wraps. Far beyond anything playable, but it is the one real number. |
| **Already exercised at 100** | `EnlightTestMode` sweeps `reps` up to 100 (780 ms) interactively. If that has been run at its ceiling without trouble, it is direct hardware evidence — better than any of the reasoning below. |
| **Task churn** | One `xTaskCreatePinnedToCore` at `configMAX_PRIORITIES-1` on core 0 **per cycle**. Already the case at `cycles = 20` in Outflow; scales linearly but doesn't break. |
| **Power / thermal** | **Real, and yours to quantify.** `AFE_ON` is raised in `run()` and dropped only when `_repsRemaining` hits 0, and the LED PDM buffer is transmitted every cycle — so `cycles × 7.8 ms` is *continuous* emitter-on time. |
| **Feedback latency** | **Real.** No hit result exists until the whole run completes. 40 cycles = 312 ms between trigger and response; 20 (Outflow today) = 156 ms. |
| **Aiming stability** | **Real, and probably the binding one.** Coherent integration only helps while the target stays in the beam. Past roughly 200–300 ms, hand tremor decorrelates the return, so extra cycles stop buying SNR and start costing hit rate. |

**Conclusion: nothing in the firmware constrains `cycles` below ~8000.** The
ceiling is a playability and battery budget, so pick it from measurement, not
from code — and the instrument already exists. `EnlightTestMode` varies `reps`
live and prints the colour coordinates and raw sums for each shot, so a sweep
against a target at a marked distance answers *both* open questions at once:
where added cycles stop improving hit reliability (the real `MAX_CYCLES`), and
how the far sums fall with distance (the falloff exponent of §2.2).

`MAX_CYCLES = 100` is adopted above to match the existing `MAX_REPS`, on the
grounds that a ceiling already chosen and exercised by the test tool beats one
invented here. Note it is a *clamp*, not a recommendation: it only catches a
typo in a ruleset table. The playable range stays far below it — `LONG` at 30 is
240 ms, already near the hand-tremor limit, and 100 cycles is 780 ms of
continuous emission, which the battery budget will almost certainly rule out
long before the classifier does.

### 3.5 Optional: a balance check

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

| | cycles | burst | cooldown | period | rangeM | strength | character |
|---|---|---|---|---|---|---|---|
| `BASE`   | 10 | 80 ms  | **10 ms** | 90 ms   | 0 (device max) | 1 | the default profile |
| `FAST`   | 4  | 32 ms  | 60 ms     | 92 ms   | 8              | 1 | quick, short reach |
| `LONG`   | 30 | 240 ms | 400 ms    | 640 ms  | 0 (device max) | 1 | slow, maximum reach |
| `STRONG` | 15 | 120 ms | 900 ms    | 1020 ms | 15             | 3 | slow, heavy, mid reach |

These are starting points for playtesting, with one exception that is an
architectural commitment rather than a tuning value:

> **`BASE` is today's behaviour apart from one deliberate change.**
> `cycles = 10` is the current `Enlight` member initialiser, `strength = 1` is
> the implicit `lives--`, and `rangeM = 0` reduces the classifier comparison to
> exactly what it does now (§2.2). **`cooldownMs = 10` is the one accepted
> behavioural change**, applying to all six existing rulesets.

That 10 ms is safe to introduce without touching ruleset code — verified against
each one's shot loop:

- `run()` already returns `false` while `_active`, and every ruleset already
  guards its energy accounting on that return value
  (`if ((energy > 0) && (enlightPtr->run()))`), so no ruleset can spend energy on
  a refused shot.
- `poll()` gains a `COOLDOWN` result for one window, then a single `IDLE`. Every
  ruleset switches only on `PLAYER_HIT`, so both fall through harmlessly.
- `GameDefaults::LOOP_MS` is 10, so a 10 ms cooldown costs at most one extra loop
  tick: the shot period moves 80 ms → ~90 ms.

`FAST`'s short reach is not an arbitrary nerf — with 4 cycles the integration
gain is genuinely lower, so gating it explicitly makes the profile honest about
what it can detect instead of letting it produce unreliable long-range hits.
`LONG` at `rangeM = 0` is the only one that reaches as far as the device
physically can, and its 30 cycles are what make that reach *reliable*: coherent
integration accumulates signal ∝ N against noise ∝ √N.

---

## 5. Custom projectors in a ruleset

`LightAir_Game` would need five new fields to carry all of this, meaning five new
lines in all six positional ruleset initialisers. **One pointer to a sub-struct
instead**, with `nullptr` meaning "standards only, everyone starts on `BASE`" —
which is the requested "if no projectors are defined in a ruleset, just use the
default definition":

```cpp
struct ProjectorSet {
    const Projector* custom;       // extra profiles; nullptr = none
    uint8_t          customCount;  // <= ProjectorLimits::MAX_CUSTOM
    uint16_t         catalogMask;  // bit i = ProjectorId i exists in this game
    uint8_t          startId;      // profile every player begins with
    uint8_t          fallbackId;   // switched to when the active one becomes unavailable

    // Per-cycle availability predicate — see §6.4.  nullptr = always available.
    bool (*isAvailable)(uint8_t projectorId);
};

// in LightAir_Game, one new field:
const ProjectorSet* projectors;    // nullptr = standards-only default
```

Usage in a ruleset stays at the density the codebase is written for:

```cpp
static const Projector customProjectors[] = {
    //  id                 name      cycles cooldown range str role energy
    { ProjectorId::CUSTOM1, "SNIPER",   40,     900,    0,   2,  0,   3 },
    { ProjectorId::CUSTOM2, "SPRAY",     3,      40,    4,   1,  0,   1 },
};

static const ProjectorSet projectorSet = {
    customProjectors, 2,
    (1u<<ProjectorId::BASE) | (1u<<ProjectorId::FAST) |
    (1u<<ProjectorId::STRONG) | (1u<<ProjectorId::CUSTOM1),
    ProjectorId::BASE, ProjectorId::BASE,
    projAvailable,
};
```

`catalogMask` does triple duty: it is the list any picker offers, the set a totem
or quest may grant, and the validity check on any runtime `select()`. At runtime
`ProjectorCtrl` keeps a live `unlockedMask`, initialised to `1 << startId`;
unlocks set bits, and a `select()` of a locked or non-catalogued id is rejected
and logged rather than silently applied.

---

## 6. Selecting and switching — per-player and per-moment

Asymmetric loadouts are a goal: players are expected to hold **different
projectors at different times within the same game**, chosen at respawn, granted
by totems, or earned by quest progress. That single requirement settles several
open questions at once.

### 6.1 The wire format already supports it, for free

Because damage travels as a value in the packet rather than as a projector id
resolved against a shared table (§7), two players holding different projectors
need no negotiation, no config-blob field and no shared state. The design decision
taken in §1 for a different reason turns out to be exactly what asymmetry
requires. **Nothing in the radio layer changes.**

### 6.2 The queue

```cpp
struct ProjectorOutput {           // third member of GameOutput
    void select(uint8_t id);       // switch now (if unlocked and available)
    void unlock(uint8_t id);       // add to unlockedMask, don't switch
    void grant(uint8_t id);        // unlock + select
    void next();                   // cycle forward through unlocked+available ids
    void prev();                   // cycle backward
};
```

`LightAir_GameRunner::flushOutput()` applies it after all logic has run:

1. Validate against `catalogMask`, `unlockedMask` and `isAvailable()`.
2. If `enlight.isActive()`, hold the switch pending and retry next cycle —
   never reconfigure a measurement in flight.
3. Push `cycles`, `cooldownMs`, `rangeM` into `Enlight`.
4. Copy `name` into the bound display buffer.
5. Queue `UIEvent::ProjectorChange`.

That list is the entire integration surface. Everything below reduces to
producing one of those calls.

### 6.3 There is no new menu

The in-game picker the OUT_GAME respawn choice needs is **not** a menu screen. A
blocking modal like `LightAir_GameSetupMenu` would be actively wrong here: it
would stall the 10 ms loop and the radio RX while an out-of-game player still
needs to receive replies, roster traffic and the end-game signal.

Everything required is already in the loop. `StateBehavior` receives
`InputReport.keyEvents`, and `MonitorVar::Str` can show the active name in the
OUT_GAME binding set:

```cpp
static void doOutGame(const InputReport& inp, const RadioReport&,
                      LightAir_DisplayCtrl&, GameOutput& out) {
    tickGameTime();
    for (uint8_t i = 0; i < inp.keyEventCount; i++) {
        if (inp.keyEvents[i].state != KeyState::RELEASED) continue;
        if (inp.keyEvents[i].key == '>') out.proj.next();
        if (inp.keyEvents[i].key == '<') out.proj.prev();
    }
}
```

Six lines of ruleset code, zero framework code, and the player respawns with
whatever is on screen. A richer picker (stats, icons, a dedicated overlay owned
by the runner and enabled per state) is a clean upgrade later if it earns its
keep — but it should not be built before something needs more than a name.

### 6.4 Availability: how `STRONG` disappears at one life

"`STRONG` is discarded when lives reach 1" is a *conditional availability* rule,
and it generalises to role restrictions, energy thresholds and quest state. Three
ways to express it, and the middle one is wrong in an instructive way:

- A `minLives` field on `Projector` — but `lives` is a ruleset variable the
  framework knows nothing about, and Outflow has energy instead. Rejected.
- An open-coded check in each behavior — works, but every ruleset re-implements
  "detect, switch away, notify the player", and will forget the notification.
- **One function pointer on `ProjectorSet`.** The *policy* stays in the ruleset
  where the variables live; the *enforcement* (re-check, auto-fallback, UI event,
  tray message, skipping unavailable entries in `next()`/`prev()`) lives in one
  place.

```cpp
// in the ruleset — the whole feature:
static bool projAvailable(uint8_t id) {
    return !(id == ProjectorId::STRONG && lives <= 1);
}
```

`GameRunner` evaluates it once per cycle in the OUTPUT phase — one call — and if
the active projector just became unavailable, switches to `fallbackId` and fires
`UIEvent::ProjectorChange` plus a tray message. Cheap, and it makes the rule
visible in exactly one line of the ruleset.

### 6.5 Asymmetric starts

`ProjectorSet::startId` is the common default. A ruleset wanting per-player or
per-role starts needs to select during `onBegin`, which currently receives
`(LightAir_DisplayCtrl&, LightAir_Radio&, LightAir_UICtrl*, const LightAir_GameRunner&)`
— no `GameOutput`, and a `const` runner.

Selection at begin-time is safe (Enlight is idle, no loop is running), so the
queue is unnecessary there. **Recommendation: expose a direct
`LightAir_GameRunner::projector()` accessor** and drop the `const` on that
parameter, mirroring how the runner already exposes `totemIdForRole()` to
`onBegin`. Six one-word signature edits, and it avoids inventing a second
queueing path for a case that doesn't need one.

### 6.6 What the pre-game `ConfigVar` is *not* for

The config blob broadcasts one value to every player, so it cannot express
asymmetric loadouts and should not be the projector selector. It remains
available as an ordinary per-game knob for the future "tune some projector
behaviour" case (a range multiplier, a strength scale) — one broadcast integer,
applied identically everywhere, which is what it is good at.

The one generic improvement still worth making, independent of projectors: an
optional `labels` field on `ConfigVar` so S4a can render `"SNIPER"` instead of
`3` for any enumerated setting.

### 6.7 From a totem

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

### 6.8 From game events (quests)

No framework support needed. A `StateRule` condition already sees whatever the
ruleset tracks, and its `onTransition` gets a `GameOutput`:

```cpp
static bool earnedStrong(const InputReport&, const RadioReport&) { return points >= 10; }
static void giveStrong(LightAir_DisplayCtrl& d, GameOutput& out) {
    out.proj.grant(ProjectorId::STRONG);
    d.showMessage("Quest complete", 2000);
}
```

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
MSG_LIT payload[1] = projectorId  (feedback: "shone by a STRONG projector")
MSG_LIT payload[2] = roleTag      (the sender's role — §7.3)
```

Sending the value rather than an id is what keeps ruleset-local custom projectors
and per-player asymmetry working with no shared state. Sending the id *as well*
costs one byte and buys receiver-side feedback and post-game telemetry.

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

### 7.3 `roleTag` *is* the role

For a rock–paper–scissors game, no separate role concept is needed: **switching
projector at respawn is choosing your role**, and `roleTag` is what makes the
matchup resolvable. The sender cannot know the receiver's role; the receiver
knows both — its own, and the sender's from `payload[2]`:

```cpp
// entirely inside a ruleset; no framework support
static const uint8_t rps[ROLE_COUNT][ROLE_COUNT] = { ... };   // [attacker][defender]
// on receive:  dmg = rps[pkt.payload[2]][myRoleTag];
```

The outcome then flows back to the shooter through the existing reply
sub-types — `REPLY_TAKEN` / `REPLY_SHONE` / a new `REPLY_COUNTERED` — so the
whole matchup mechanic needs **zero framework changes** beyond carrying the byte.
Combined with §6.4's availability predicate (which can restrict which role a
player may take), the projector becomes the complete carrier of the role concept.

**Recommendation: reserve `payload[2]` and populate it from day one, but ship no
table.** The byte costs nothing and the table's shape should be decided by the
first game that needs one.

---

## 8. UI and display linkage

Deliberately the smallest possible surface, reusing what exists:

- **Notification.** Add `ProjectorChange` to `LightAir_UICtrl::UIEvent` and a row
  to `_actionTable`. The enum is already sized by `UIEvent::Count`, so this is an
  additive change. Rulesets wanting a per-projector signature sound still have
  `Custom1..4`.
- **Name on the OLED.** `MonitorVar::Str("Proj", projectorName, ...)` bound to a
  buffer that `ProjectorCtrl` rewrites on every switch. Zero new display code,
  and it doubles as the picker readout in §6.3. One wart: `MonitorVar` tables are
  `static const`, so the pointer must be a compile-time constant — the buffer has
  to be a global (`extern char projectorName[ProjectorLimits::NAME_LEN];`) rather
  than a `ProjectorCtrl` member. That matches how `enlightPtr` is already
  exposed; the alternative (the runner registering the binding itself) trades the
  global for asymmetry with every other monitor var. Suggest living with it.
- **Cooldown bar.** `DisplayCtrl::bindCooldownVariable(..., cooldownTimeMs, ...)`
  already exists and is unused by the rulesets; the active projector's
  `cooldownMs` is exactly its argument, and it becomes visible feedback now that
  `BASE` has a non-zero cooldown.
- **Burst indication, already correct.** Rulesets call
  `out.ui.triggerEnlight(enlightPtr->cycleTime())`, and `cycleTime()` is
  `_repetitions * MS_PER_REP` — so the UI burst length tracks the projector's
  `cycles` automatically, with no ruleset edit at all.

---

## 9. Changes to `Enlight` and calibration

`Enlight`, complete list:

1. `void setRangeM(uint8_t m)` + `float _rangeMul = 0.0f;` — one `powf` per
   switch (§2.2).
2. In `classify()`, source the three thresholds from `refFar * _rangeMul` floored
   at `thresh_far_*`. Same logic shape, three `fmaxf` calls.
3. Defensive `if (_active) return;` in the three setters, behind the runner's own
   "don't switch mid-run" rule.
4. `EnlightDefaults::RANGE_FALLOFF_EXP`.

Calibration (§2.3), complete list:

5. `EnlightCalib` gains `refFarR/G/B`; `nvs_config` gains three keys; `limpow`
   retires.
6. `step1()`'s prompt states the reference distance.
7. `step2()` saves the median of its already-computed baseline-subtracted values.
8. `step4()` displays the derived `Rmax` (§2.4).

**Ownership invariant to write down and keep:** Enlight's optical settings are
owned by `ProjectorCtrl` while a game is running, and by the tool
(`EnlightCalibRoutine`, `EnlightTestMode`) outside a game. Calibration must keep
setting `REPS` explicitly and must never run through a projector — its output is
what the range model is measured against, so calibrating through a range gate
would be circular.

---

## 10. Files

| File | Change |
|---|---|
| `src/config.h` | add `ProjectorLimits` |
| `src/game/LightAir_Projector.h` | **new** — `Projector`, `ProjectorId`, `ProjectorSet`, `projectorClamp()`, standard table |
| `src/game/LightAir_ProjectorCtrl.h/.cpp` | **new** — registry, `unlockedMask`, availability re-check, Enlight push, name buffer |
| `src/game/LightAir_GameOutput.h` | add `ProjectorOutput proj` |
| `src/game/LightAir_Game.h` | add `const ProjectorSet* projectors`; drop `const` on `onBegin`'s runner |
| `src/game/LightAir_GameRunner.h/.cpp` | register the set in `begin()`, apply queue + availability in `flushOutput()`, `projector()` accessor |
| `src/game/LightAir_GameVar.h` | optional `labels` on `ConfigVar` (§6.6) |
| `src/game/LightAir_GameSetupMenu.cpp` | render `labels` when present |
| `src/enlight/Enlight.h/.cpp` | `setRangeM`, range-derived thresholds in `classify()`, `RANGE_FALLOFF_EXP` |
| `src/nvs_config.h/.cpp` | `refFarR/G/B` keys; retire `limpow` |
| `src/tools/EnlightTestMode.cpp` | replace the local `MAX_REPS` with `ProjectorLimits::MAX_CYCLES` |
| `src/tools/EnlightCalibRoutine.cpp` | step 1 prompt, step 2 saves the reference, step 4 shows `Rmax` |
| `src/ui/player/LightAir_UICtrl.h/.cpp` | `UIEvent::ProjectorChange` + table row |
| `src/LightAir.h` | include the two new headers |
| `src/rulesets/*.cpp` (×6) | one `projectors` field; `dmg(pkt)` in the lit conditions; `onBegin` signature |

The standard table goes in `LightAir_Projector.h` rather than `config.h` —
`config.h` is already 508 lines, and only the limits were asked to live there.
`TeamLedRhythm` sets the precedent for either choice.

---

## 11. Open questions

1. **The falloff exponent and reference distance.** `EXP = 3` and
   `CAL_REF_DIST_M = 5` are the two numbers the whole metric model rests on.
   Both are one-constant edits, and §2.4's `Rmax` readout is the cheapest way to
   validate them in the field. Worth measuring signal at two or three marked
   distances once, before committing.
2. **Does the existing calibration actually support 40 m?** `Rmax` computed from
   `refFar / thresh_far` will say. If it comes out well under the README's 40 m,
   the current classifier is already gating shorter than the hardware can see —
   independent of this feature, but this is what would reveal it.
3. **`MAX_CYCLES`** — answered in §3.4: the name is new, nothing in the firmware
   binds below ~8000, and a ceiling of 100 already exists as `MAX_REPS` in
   `EnlightTestMode`. Adopt that number, hoist it to `config.h` so the tool and
   the projector clamp share it, and set the *playable* range from a battery
   measurement of the AFE duty cycle.
4. **Naming of `strength`** (§2.5).
5. **Per-colour range accuracy.** The gate necessarily uses a reference target's
   reflectivity, so darker players are gated closer (§2.3). A per-colour
   reference table with a post-classification gate would fix it; probably v2.
6. **Persistence across games.** An unlocked-projector bitmask in NVS would turn
   totem rewards into meta-progression. It changes the NVS layout and has real
   fairness implications between players with different play histories — suggest
   explicitly leaving it out of v1.
