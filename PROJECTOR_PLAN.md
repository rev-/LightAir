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
| **Game** | `strength` (damage), `roleTag`, the shot economy | ruleset logic + the radio packet | read by rules / `ProjectorCtrl` |
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
    uint8_t     id;              // ProjectorId value; index into the registry
    const char* name;            // <= ProjectorLimits::NAME_LEN-1 chars, shown on the OLED

    // ---- optical: pushed into Enlight ----
    uint16_t    cycles;          // = Enlight::setRepetitions(); burst = cycles * MS_PER_REP
    uint16_t    cooldownMs;      // = Enlight::setCooldown(); dead time after each measurement
    uint8_t     rangeM;          // approximate reach in metres; 0 = whatever the device can see

    // ---- game: the shot economy (§2.5) ----
    Recharge    recharge;        // refill behaviour: REFILL / RAMP / NONE / CONSUMED
    uint8_t     energyCost;      // energy consumed per shot
    uint8_t     maxEnergy;       // pool size when full / number of charges
    uint16_t    rechargeDelayMs; // idle time before recharging starts
    uint16_t    rechargeMs;      // empty -> full duration (RAMP only)

    // ---- game: the effect ----
    uint8_t     strength;        // hit weight in STANDARD HITS, not health points (§7.5)
    uint8_t     roleTag;         // 0 = generic; the player's "role" for a per-game table (§7.3)
    uint16_t    targetImmunityMs;// min gap between two hits on the SAME target (§7.4)

    // ---- game: handling and feedback ----
    uint16_t    readyMs;         // deploy time after a switch before the first shot (§2.5.5)
    const LightAir_UICtrl::UIAction* shotAction;  // sound/vibration/colour of a shot (§8.1)
    const uint8_t* icon;         // 8x8 PROGMEM bitmap shown beside this projector's energy (§8.2)
};
```

The struct lands at ~32 bytes with padding. Eight of them is a quarter of a
kilobyte of flash, so there is no reason to contort the layout for size.

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
switch**, in the setter — so a non-integer exponent (2.7, 3.2) costs nothing at
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

### 2.5 The shot economy

`maxEnergy` and the two recharge fields are the least speculative addition in
this document, because **they do not add a feature — they centralise one that is
already copy-pasted five times.** `GameFreeForAll`, `GameFlag`, `GameTeams`,
`GameKingOfHill` and `GameUpkeep` each carry this block verbatim:

```cpp
if (triggerWasActive && !triggerActive) releaseAt = millis();
triggerWasActive = triggerActive;

if (!triggerActive && energy < startEnergy) {
    if ((millis() - releaseAt) >= (uint32_t)rechargeSecs * 1000)
        energy = startEnergy;
}
```

plus the matching `startEnergy` / `rechargeSecs` config vars, the `releaseAt`
and `triggerWasActive` statics, and the `energy--` / `energySpent++` in the
trigger loop. Five identical copies, and a sixth variant in `GameOutflow` where
energy is also drained passively. Moving the pool into `ProjectorCtrl` deletes
all five.

#### 2.5.1 `ProjectorCtrl` always owns the pool

An earlier revision had a `Recharge::RULESET` mode meaning "the framework manages
nothing here". That was an ownership escape hatch dressed as a behaviour, and it
existed because two genuinely different problems had been conflated:

- **A migration concern** — five rulesets manage energy today and must not change
  behaviour.
- **A game-design case** — `GameOutflow`, where energy never refills because it
  *is* the life total.

Separated, neither needs an opt-out. The migration concern is answered by giving
`BASE` the values those rulesets already use (§4), and the design case is a real
refill behaviour, `Recharge::NONE` (§2.5.2). So:

> **`ProjectorCtrl` owns the energy pool for every projector.** A ruleset that
> needs different numbers *states* them; it does not opt out of the mechanism.

The DM's `Energy` and `Recharge` config vars keep their authority through one
explicit call, rather than through a mode that switches the framework off:

```cpp
// in the ruleset's onBegin — the config vars still win:
runner.projector().setPool(startEnergy, (uint16_t)rechargeSecs * 1000);
```

One line added, the eight-line recharge block deleted — a net reduction in every
ruleset that has one, and the intent is visible at the call site instead of
hidden behind an enum value.

**One constraint this creates, and it is not optional.** `GameOutflow` grants the
shooter `startEnergy` on an elimination, *uncapped* — its own header says so. So
`ProjectorCtrl` must **clamp to `maxEnergy` only when refilling, never on a
direct write by a ruleset.** Clamping every write would silently break a
mechanic that already exists.

`ProjectorCtrl` exposes the active projector's pool as a global `int`
(`extern int projectorEnergy;`) for the same reason as the name buffer (§8.3):
`MonitorVar` tables are `static const`, so the bound pointer must be a
compile-time constant.

#### 2.5.2 Recharge is a named mode, not a magic number

"Time for recharge" is two numbers *and* a rule about how they are read. Once
"never refills" and "consumed when spent" are in scope, encoding the rule in the
numbers themselves (0 = instant, 0xFFFF = never, …) becomes a set of magic values
that nobody will remember. One byte makes every case explicit:

```cpp
enum class Recharge : uint8_t {
    REFILL = 0,   // after rechargeDelayMs idle, jump straight to maxEnergy
    RAMP,         // after rechargeDelayMs idle, +1 every rechargeMs / maxEnergy
    NONE,         // never refills; stays in the inventory when empty
    CONSUMED,     // never refills; the projector is DROPPED when it reaches 0
};
```

| mode | `rechargeDelayMs` | `rechargeMs` | behaviour |
|---|---|---|---|
| `REFILL`  | 10000 | — | full refill after 10 s idle — today's rule, framework-side |
| `RAMP`    | 1500 | 4000 | brief pause, then a visible refill ramp |
| `NONE`    | — | — | a fixed reserve; what happens at 0 is the ruleset's business |
| `CONSUMED`| — | — | `maxEnergy` shots, then gone |

The four values are a clean 2×2 — two that refill, differing in *shape*; two that
do not, differing in *what happens at zero* — with no member that means "ignore
this struct". That shape is itself evidence the abstraction is right; the version
with an escape hatch did not have it.

Integer stepping for `RAMP` — `rechargeStepMs = rechargeMs / maxEnergy`, one unit
per step — avoids floats entirely.

**`NONE` is what `GameOutflow` needs**, and it is the opposite of `CONSUMED`
rather than a weaker version of it — a distinction an earlier revision of this
document got wrong by dropping `NONE` as redundant. In Outflow, reaching zero is
the *elimination condition*, handled by the ruleset's own `pendingDepletion`
transition; the projector must stay in hand so the player can respawn with it.
`CONSUMED` would delete the projector at exactly the moment the ruleset needs it.
Outflow's projector is `NONE`, it keeps `tickDrain()` and its depletion rule
untouched, and it *deletes* its own `energy--` from the shot loop because
`ProjectorCtrl` now does that.

**`CONSUMED` is what "single-time charge" should be.** A projector that can never
refill but stays in the inventory is dead weight occupying a slot the player
cannot reclaim; one that *leaves* when spent is a resource used wisely, frees its
slot automatically, and drops the holder back to the baseline projector with no
special case. A one-shot power pickup is simply:

```cpp
{ ..., Recharge::CONSUMED, /*energyCost*/ 1, /*maxEnergy*/ 1, 0, 0, ... }
```

This is also exactly where re-granting earns its keep (§2.5.3): handing a
`CONSUMED` projector to someone who still holds it is a **restock**, which is the
only reading under which "refill on re-grant" clearly makes sense.

A projector held only for its `roleTag`, with no shooting role at all, is just
`NONE` with `maxEnergy = 0` — no fifth mode needed.

#### 2.5.3 Every projector carries its own energy

There is no "the pool" to transfer on a switch: **each owned projector has its
own energy, full when it is first earned.** That is both more intuitive (each
projector has its own battery) and strictly simpler than any carry-over rule —
switching cannot create or destroy energy, so the tap-switch-to-reload exploit
never exists in the first place, and `readyMs` (§2.5.5) is left as the only cost
of switching.

It does mean a flat "which ones do I have" bitmask is not sufficient. What a player
holds becomes an **ordered inventory**:

```cpp
struct ProjectorSlot {
    uint8_t  id;            // ProjectorId
    uint8_t  energy;        // this projector's own live pool
    uint32_t acquiredAt;    // millis() when earned — fixes the eviction order
    uint32_t lastShotAt;    // per-slot, so rechargeDelayMs is per-projector
};
```

Two consequences worth deciding explicitly rather than discovering:

- **Only the projector in hand recharges.** Time spent in the inventory is dead
  time: a spare left empty is still empty when you come back to it. This is what
  makes carrying several a real decision rather than a way to fire continuously
  by cycling — put a projector away dry and you have to earn the pause to refill
  it. It also makes the tick trivial: one slot per game cycle, not a loop.
  The `rechargeDelayMs` gate is measured from that slot's own `lastShotAt`, so
  switching back after a long absence starts refilling immediately (you gained
  nothing while away, but you are past the just-fired grace period), while
  `readyMs` independently blocks firing for a moment after the switch.
- **Re-granting one you already hold refills it and leaves `acquiredAt`
  untouched.** Refilling makes a pickup worth taking even when you already have
  the projector — most meaningfully for `CONSUMED` profiles, where a re-grant is
  a restock. Keeping the timestamp stops re-granting from being used to dodge
  eviction.

#### 2.5.4 The baseline projector is structural, not inventory

An earlier revision of this document exempted a `fallbackId` from eviction, which
was a convoluted way of reaching a rule that is far better stated directly:

> **`BASE` is not part of the inventory. It is always held, never counted, never
> lost.** `maxOwned` counts only the *powered* projectors carried alongside it,
> and eviction can only ever remove one of those.

| `maxOwned` | the player holds |
|---|---|
| 0 | `BASE` only |
| 1 | `BASE` + one powered projector |
| 3 | `BASE` + up to three powered projectors |

Concretely, `BASE` occupies slot 0 permanently and the inventory is
`ProjectorSlot powered[MAX_OWNED]`; **eviction never looks at slot 0.** That
removes the special case entirely rather than defending against it — there is no
rule to forget, because the baseline is not in the array eviction walks. It also
means the availability fallback of §6.4 always has somewhere to go, which was the
real reason the exemption existed.

This merges two `ProjectorSet` fields into one: `startId` is now *the permanent
slot-0 projector* — the one you always have, never lose, and fall back to — and
the separate `fallbackId` is deleted. A ruleset that wants a different baseline
than `BASE` simply names it in `startId`.

**Eviction is first-in-first-out** on `acquiredAt` among the powered slots. Taking
a fourth powered projector with `maxOwned = 3` drops the one held longest, and if
that happens to be the one currently in hand, the switch goes to the newly granted
projector — which is what a player expects after picking one up. Eviction must be
*visible*: a tray message plus `UIEvent::ProjectorChange`, or a player silently
loses something they were counting on.

`maxOwned` reaches the framework as a pointer to the ruleset's own config var,
mirroring the `LightAir_Game::gameTimeLeft` idiom that already exists for exactly
this "framework reads a live ruleset int" case:

```cpp
// in ProjectorSet:
const int* maxOwned;    // nullptr = ProjectorLimits::DEFAULT_MAX_OWNED

// in the ruleset:
static int maxProjectors = 3;
static const ConfigVar configVars[] = {
    { "Projectors", &maxProjectors, ProjectorLimits::MIN_OWNED,
                                    ProjectorLimits::MAX_OWNED, 1 },
};
```

`MIN_OWNED = 1`, so a DM cannot accidentally set a game to zero powered slots and
silently make every totem reward and quest unlock a no-op. The `maxOwned = 0` row
above is therefore unreachable through the menu — and it does not need to be
reachable, because the natural way to express a `BASE`-only game is to leave
`LightAir_Game::projectors` as `nullptr`.

#### 2.5.5 `readyMs` — the cost of switching

Per-projector energy removes the free-reload exploit, but not the other one:
flicking to another projector and back would otherwise dodge a long *cooldown*.
`readyMs` — a deploy time after a switch before the first shot is allowed — is
the guard, and it doubles as characterisation: a heavy projector that takes
600 ms to bring up *feels* heavy. `ProjectorCtrl` implements it by pushing a
one-shot cooldown into `Enlight` on switch, so it costs no new state. With
parallel recharge across the inventory (§2.5.3) it is also the main lever
against inventory-cycling, so it matters more than it first appears.

#### 2.5.6 `shotUiEvent` and `icon` — the projector is the weapon's identity

Every ruleset currently fires the same `out.ui.triggerEnlight(cycleTime())` and
binds the same `ICON_ENERGY`, so every projector would look and feel identical.
Two fields fix that, and both are designed so a projector definition is
**self-contained** — a new profile carries its own sound and its own artwork,
and no other file has to be edited to add one. The standard projectors get
first-class UI events of their own rather than borrowing `Custom1..4` slots
(§8.1), and the icon is a bitmap pointer rather than an enum value (§8.2).

### 2.6 What else was considered, and rejected

Kept out deliberately, with the reason, so the same ideas don't get relitigated:

| Idea | Why not |
|---|---|
| `minLives`, team restriction, role restriction | All three are `isAvailable()` (§6.4) written differently. One predicate already covers every conditional-availability rule. |
| Heat / overheat | A second resource with the same shape as energy. `energyCost` + `rechargeMs` already expresses "fires fast, then must stop". |
| Unlock price / economy | No consumer. The inventory already answers "do I have this"; what it *cost* is a game that doesn't exist yet. |
| Accuracy / spread | The optics have no such knob. `rangeM` and `cycles` are the only real levers on detection, and both are already exposed. |
| `hitCooldownMs` | Still deferred per §2.1 — cheapest of the deferred fields, but wait for a ruleset to ask. |

**Correction to an earlier draft of this document:** the per-projector `icon`
was rejected here for lack of CGRAM slots. That was wrong — the display is an
SSD1306 graphic OLED and `DisplayCtrl::drawIcon()` already ends in
`_display.drawBitmap(x, y, 8, 8, ptr)`. There is no CGRAM and no slot budget;
the "reuses the hourglass CGRAM slot" comment in `LightAir_Display_Icons.h:18`
is a leftover from an earlier character-LCD implementation. The icon is in, and
in the better form — see §8.2.

### 2.7 A naming note

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
    constexpr uint8_t  MIN_STRENGTH = 0,  MAX_STRENGTH = 10;   // standard hits — see §7.5
    constexpr uint8_t  MAX_ENERGY_COST = 10;
    constexpr uint8_t  MAX_ENERGY      = 200;   // pool size ceiling (config vars cap at 100 today)
    constexpr uint16_t MAX_RECHARGE_MS = 60000; // empty -> full, and the idle delay before it
    constexpr uint16_t MAX_READY_MS    = 5000;  // deploy time after a switch
    constexpr uint16_t MAX_IMMUNITY_MS = 30000; // min gap between hits on the same target
    constexpr uint8_t  MIN_OWNED       = 1;     // never zero — see §2.5.4
    constexpr uint8_t  MAX_OWNED       = 8;     // powered-slot array bound (BASE not counted)
    constexpr uint8_t  DEFAULT_MAX_OWNED = 3;   // used when ProjectorSet::maxOwned is null
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

| | cycles | burst | cooldown | period | rangeM | strength | energy | recharge | ready | character |
|---|---|---|---|---|---|---|---|---|---|---|
| `BASE`   | 10 | 80 ms  | **10 ms** | 90 ms   | 0 (device max) | 1 | `REFILL` 50 × 1 | 10000 ms | 0 | the default profile |
| `FAST`   | 4  | 32 ms  | 60 ms     | 92 ms   | 8              | 1 | `RAMP` 30 × 1 | 1500 / 3000 ms | 150 ms | quick, short reach, drains fast |
| `LONG`   | 30 | 240 ms | 400 ms    | 640 ms  | 0 (device max) | 1 | `REFILL` 20 × 1 | 4000 ms | 400 ms | slow, maximum reach |
| `STRONG` | 15 | 120 ms | 900 ms    | 1020 ms | 15             | 3 | `REFILL` 8 × 2  | 6000 ms | 600 ms | slow, heavy, mid reach |

(`energy` reads *mode, pool × cost per shot*; `recharge` reads
*`rechargeDelayMs` / `rechargeMs`*, with `rechargeMs` shown only for `RAMP`.)

Identity and anti-spam, per §7.4, §8.1 and §8.2:

| | `targetImmunityMs` | `shotAction` | `icon` |
|---|---|---|---|
| `BASE`   | 3000 (today's `HIT_IMMUNITY_MS`) | `nullptr` — the standard `Enlight` row | `ICON_ENERGY_BITMAP` (unchanged) |
| `FAST`   | 1200                             | `PROJ_FAST_ACTION`   | `PROJ_FAST_ICON` |
| `LONG`   | 3000                             | `PROJ_LONG_ACTION`   | `PROJ_LONG_ICON` |
| `STRONG` | 6000                             | `PROJ_STRONG_ACTION` | `PROJ_STRONG_ICON` |

`BASE` leaving `shotAction` null means today's shot feedback is untouched, in the
same way `ICON_ENERGY_BITMAP` keeps today's icon.

`FAST`'s shorter immunity is what makes it feel fast against a single target;
`STRONG`'s longer one is what stops `strength = 3` from erasing someone in a
second. Those two numbers are now the main balance dial, and they are per
projector rather than one global constant.

These are starting points for playtesting, with one exception that is an
architectural commitment rather than a tuning value:

> **`BASE` is today's behaviour apart from one deliberate change.**
> `cycles = 10` is the current `Enlight` member initialiser, `strength = 1` is
> the implicit `lives--`, and `rangeM = 0` reduces the classifier comparison to
> exactly what it does now (§2.2). **`cooldownMs = 10` is the one accepted
> behavioural change**, applying to all six existing rulesets.

`BASE`'s energy line is today's numbers, not an exemption from the mechanism:
`REFILL / 50 / 10000 ms` is exactly `startEnergy = 50` and `rechargeSecs = 10`,
the defaults every one of the five energy rulesets already declares. A ruleset
whose DM changes those config vars keeps its numbers with one `setPool()` call in
`onBegin` (§2.5.1) while deleting its eight-line recharge block — same behaviour,
smaller file.

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
    uint8_t          startId;      // the permanent slot-0 projector: always held, never
                                   //   evicted, and the availability fallback (§2.5.4)
    const int*       maxOwned;     // powered slots; -> ruleset config var, null = DEFAULT

    // Per-cycle availability predicate — see §6.4.  nullptr = always available.
    bool (*isAvailable)(uint8_t projectorId);
};

// in LightAir_Game, one new field:
const ProjectorSet* projectors;    // nullptr = standards-only default
```

Usage in a ruleset stays at the density the codebase is written for:

```cpp
static const Projector customProjectors[] = {
    //  id                 name     cyc  cool  rng | cost pool delay rech | str role | ready ui
    { ProjectorId::CUSTOM1, "SNIPER", 40,  900,   0,    3,   6, 5000,   0,   2,   0,    800, 0 },
    { ProjectorId::CUSTOM2, "SPRAY",   3,   40,   4,    1,  60, 2000, 4000,  1,   0,    100, 0 },
};

static const ProjectorSet projectorSet = {
    customProjectors, 2,
    (1u<<ProjectorId::BASE) | (1u<<ProjectorId::FAST) |
    (1u<<ProjectorId::STRONG) | (1u<<ProjectorId::CUSTOM1),
    ProjectorId::BASE, &maxProjectors,
    projAvailable,
};
```

`catalogMask` does triple duty: it is the list any picker offers, the set a totem
or quest may grant, and the validity check on any runtime `select()`. What a
player actually *holds* is the ordered inventory of §2.5.3, seeded with `startId`
at full energy; a `select()` of an unowned or non-catalogued id is rejected and
logged rather than silently applied.

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
    void select(uint8_t id);       // switch to one already owned
    void give(uint8_t id);         // add to the inventory at full energy, don't switch
    void grant(uint8_t id);        // give + select
    void drop(uint8_t id);         // remove from the inventory
    void next();                   // cycle forward through owned + available slots
    void prev();                   // cycle backward
};
```

`LightAir_GameRunner::flushOutput()` applies it after all logic has run:

1. Validate against `catalogMask`, the inventory, and `isAvailable()`.
2. On `give`/`grant`: if already owned, refill that slot and keep its
   `acquiredAt`; otherwise append at full energy, evicting FIFO if the
   inventory is at `maxOwned` (§2.5.4).
3. If `enlight.isActive()`, hold any switch pending and retry next cycle —
   never reconfigure a measurement in flight.
4. Push `cycles`, `cooldownMs`, `rangeM` into `Enlight`, and arm `readyMs`.
5. Point `projectorName`, `projectorEnergy` and `projectorIcon` at the new slot.
6. Queue `UIEvent::ProjectorChange` (and a tray message on eviction).

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
the active projector just became unavailable, switches to the permanent slot-0
projector (§2.5.4) and fires `UIEvent::ProjectorChange` plus a tray message. Cheap, and it makes the rule
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
static void awardStrong(LightAir_DisplayCtrl& d, GameOutput& out) {
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

### 7.4 `targetImmunityMs` — moving anti-spam to the attacker

Moving `HIT_IMMUNITY_MS` out of `GameFreeForAll.cpp:63` and into the projector,
enforced by the *attacker* refusing to send a second `MSG_LIT` to the same target
too soon, is a good change. Four reasons, one of which I got wrong in an earlier
draft:

1. **It is semantically faithful, not a behaviour change.** Today's guard is
   already keyed per attacker — `litAt[pkt.senderId]` in `notImmune()`. "This
   attacker cannot hit me again within 3 s" and "I cannot hit this target again
   within 3 s" are the same rule seen from the two ends. Relocating it preserves
   the meaning exactly.
2. **Feedback gets dramatically faster.** Today the attacker learns it was
   suppressed through `REPLY_IMMUNE`, subject to `RadioDefaults::REPLY_TIMEOUT_MS`
   (2000 ms) if the reply is lost. Enforced locally, `UIEvent::Immune` fires the
   instant the shot is suppressed.
3. **It saves radio traffic** — suppressed at the source instead of sent, parsed
   and rejected. With `FAST` in a 16-player game that is a lot of packets never
   transmitted.
4. **The duplicate-packet objection does not apply.** This was my reservation,
   and checking the code dissolves it: `LightAir_Radio::processPacket` already
   dedups on `(senderId, timestamp, msgType)` over a 32-entry window
   (`RADIO_DEDUP_WINDOW`), unconditionally for every packet — the comment at
   `LightAir_Radio.cpp:239` records that gating it on `resend > 0` used to let
   duplicates through and double-count exactly this kind of counter. Receiver-side
   immunity is therefore *not* what protects against duplicate delivery; the
   transport already does, one layer down.

It also deletes real code: `litAt[PlayerDefs::MAX_PLAYER_ID]`, `notImmune()`, and
the `litButImmune` / `REPLY_IMMUNE` rule pair, from every ruleset that has them.

**Two conditions, and they matter.**

- **The suppression table lives in `ProjectorCtrl` and must survive a switch.**
  It is per-*target* state (`uint32_t lastLitAt[MAX_PLAYER_ID]`); only the
  *duration* comes from the active projector. Reset it on switch and you have
  recreated the tap-switch exploit in a new place — the same class of bug
  §2.5.3 and §2.5.5 exist to close.
- **Enforcement becomes voluntary**, since a modified firmware can simply not
  suppress. That sounds like a real loss and is not one here: the wire is
  *already* fully trusted — a modified sender can claim any `strength`, any
  `roleTag`, any `senderId`. Attacker-side immunity does not weaken a property
  the system actually has, so it is consistent with the existing trust model
  rather than a new hole in it. Worth stating in the header comment so nobody
  later mistakes it for a security boundary.

`REPLY_IMMUNE` still earns its place for the *receiver's* own rules — a
respawn-protection window, for instance — which is a different thing from
per-attacker anti-spam and should not be folded into the projector.

### 7.5 `strength` counts standard hits, not health points

Migrating `GameOutflow` (§12) exposed a problem with the obvious reading of
`strength`. Health is not denominated the same way across rulesets:

| ruleset | health | one ordinary hit costs |
|---|---|---|
| `GameFreeForAll`, `GameFlag`, `GameTeams`, … | `lives`, 1–5 | **1** |
| `GameOutflow` | `energy`, 50–200 | **`hitDmg`, 25–200** |

Two orders of magnitude apart. A `strength` holding health points would need a
range of 1–200, `MAX_STRENGTH` would become meaningless as a balance limit, and
`STRONG` would have to be redefined per game to mean anything.

> **`strength` is the hit's weight in *standard hits*.** `BASE` = 1, `STRONG` =
> 3. Each ruleset decides what one standard hit costs in its own currency.

```cpp
// lives-based rulesets:  one standard hit = one life
lives  -= dmg(pkt);
// GameOutflow:           one standard hit = hitDmg energy
energy -= dmg(pkt) * hitDmg;
```

This is better than a per-game fix in every way that matters: `MAX_STRENGTH = 10`
stays a real limit, `STRONG` means "three times an ordinary hit" in *every* game
without redefinition, and `hitDmg` stays a DM-tunable config var instead of being
frozen into a compile-time projector field.

---

## 8. UI and display linkage

### 8.1 The shot's feedback travels inside the projector too

#### What the trap actually is

The `Enlight` UI event is unlike every other event in the table: its **duration
is decided at runtime**, because the light burst lasts `cycles × MS_PER_REP` and
that varies per projector. The mechanism is an `extraMs` override, and it is
gated on one exact enum value — `LightAir_UICtrl.cpp:352`:

```cpp
uint16_t duration =
  (_current.id == UIEvent::Enlight && _current.extraMs > 0)
    ? _current.extraMs                          // the real burst length
    : _current.action->durations[_currentStep]; // the table's fixed 10 ms
```

with a matching hardcode at `.cpp:129`, where `triggerEnlight()` calls
`applyPolicy(UIEvent::Enlight, ms)`.

So if a `FAST` projector fired a *different* event — `UIEvent::EnlightFast` —
that equality test would be false, `extraMs` would be discarded, and the buzzer,
vibration and LED would run for the table's fixed 10 ms instead of the actual
32 ms burst. No crash, no compile error: just feedback that feels subtly wrong,
reported later as "the beep is off" and hard to trace back to a missing enum
value in a conditional.

#### Your suggestion is the better fix — take it

Rather than adding events and defending the conditional against them, **keep
exactly one `UIEvent::Enlight` and swap the action behind it when the projector
changes.** The trap then cannot occur, because the enum value never changes.

`LightAir_UICtrl` already has the hook. `resolveAction()` consults an override
table before falling back to the static one:

```cpp
const UIAction& LightAir_UICtrl::resolveAction(UIEvent event) {
  uint8_t idx = (uint8_t)event;
  if (idx >= (uint8_t)UIEvent::Custom1 && idx <= (uint8_t)UIEvent::Custom4) {
    uint8_t ci = idx - (uint8_t)UIEvent::Custom1;
    if (_customDefined[ci]) return _customActions[ci];
  }
  return _actionTable[idx];
}
```

The change is to give the `Enlight` slot the same treatment — one extra stored
action plus a `setEnlightAction(const UIAction&)` (or a `defineCustomAction()`
whose slot check accepts `Enlight`). `ProjectorCtrl` calls it on every switch,
passing the active projector's `shotAction`, and a null `shotAction` leaves the
standard table entry in place.

What this buys, beyond avoiding the trap:

| | events-per-projector | action swapped at switch |
|---|---|---|
| `UICtrl.cpp:352` / `:129` | must both change | **untouched** |
| `UIEvent` enum + action table | grows per standard projector | unchanged |
| A custom ruleset projector | limited to the 4 `Custom` slots | same mechanism as the standards |
| Adding a projector | edit the enum, the table, and the projector | **edit one table row** |

That last row is the same self-containment property as the icon (§8.2), reached
the same way: the definition carries its own artwork *and* its own sound.

**One risk, and it is closed already.** Swapping the action while an `Enlight`
event is mid-playback would let the remaining steps run with the new action —
a glitch, not a crash, since the storage is a fixed member and the
`ScheduledEvent`'s pointer stays valid. It cannot happen in practice: a switch is
already deferred while `enlight.isActive()` (§6.2), and the `Enlight` UI event
lasts exactly `cycleTime()`, i.e. the burst it accompanies. The two windows
coincide, so the switch always lands after playback ends.

#### One genuinely new event

`ProjectorChange` — fired on a switch, a grant, an eviction, or an automatic
fallback. This one carries **no trap**: it has a fixed duration like every other
event in the table, takes no `extraMs`, and touches neither `.cpp:352` nor
`:129`. Add it to the enum and give it one `_actionTable` row.

### 8.2 The icon travels inside the projector definition

**Yes, this works, and it is the better shape.** `DisplayCtrl::drawIcon()`
already ends in `_display.drawBitmap(x, y, 8, 8, ptr)` — the icon path is
bitmap-based all the way down, so a projector can carry its own artwork instead
of referencing a shared enum:

```cpp
// in LightAir_Projector.h, right beside the standard table:
static const uint8_t PROJ_FAST_ICON[8]   PROGMEM = { /* ... */ };
static const uint8_t PROJ_LONG_ICON[8]   PROGMEM = { /* ... */ };
static const uint8_t PROJ_STRONG_ICON[8] PROGMEM = { /* ... */ };
```

`BASE.icon` points at the existing `ICON_ENERGY_BITMAP`, so `BASE` keeps the
familiar energy icon and nothing about today's display changes until another
projector is held.

Two small additions make it work end to end:

1. **`DisplayCtrl` accepts a raw bitmap.** `VariableBinding` gains a nullable
   `const uint8_t* const* iconBitmapPtr` which, when set, wins over the
   `IconType`. It is a pointer-to-pointer on purpose: it aims at a global
   `const uint8_t* projectorIcon;` that `ProjectorCtrl` rewrites on every switch,
   so the icon follows the active projector with **no re-binding** — the same
   trick already used for `projectorName` and `projectorEnergy`.
2. **`MonitorVar` gains a factory** that takes that pointer, e.g.
   `MonitorVar::IntDyn("Energy", &projectorEnergy, mask, &projectorIcon, col, row)`.

The result is what you asked for: adding a projector means adding one row to a
table and eight bytes of bitmap next to it. `Display_Icons.h`, `UICtrl`'s action
table and every ruleset stay untouched.

### 8.3 The rest, reusing what exists

- **Name on the OLED.** `MonitorVar::Str("Proj", projectorName, ...)` bound to a
  buffer `ProjectorCtrl` rewrites on every switch. Zero new display code, and it
  doubles as the picker readout in §6.3. `MonitorVar` tables are `static const`,
  so the buffer must be a global — as must `projectorEnergy` and
  `projectorIcon`, for the same reason.
- **Cooldown bar.** `DisplayCtrl::bindCooldownVariable(..., cooldownTimeMs, ...)`
  already exists and is unused by the rulesets; the active projector's
  `cooldownMs` is exactly its argument, and it becomes visible feedback now that
  `BASE` has a non-zero cooldown.
- **Burst indication, already correct.** `cycleTime()` is
  `_repetitions * MS_PER_REP`, so the UI burst length tracks the projector's
  `cycles` automatically — provided the trap in §8.1 is handled.

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
| `src/game/LightAir_ProjectorCtrl.h/.cpp` | **new** — inventory + FIFO eviction over powered slots, the energy pool + `setPool()` + active-slot recharge tick, `setEnlightAction()` on switch, availability re-check, Enlight push, per-target immunity table, name/energy/icon globals |
| `src/game/LightAir_GameOutput.h` | add `ProjectorOutput proj` |
| `src/ui/player/display/LightAir_DisplayCtrl.h/.cpp` | binding variant taking `const uint8_t* const*` instead of `IconType` (§8.2) |
| `src/game/LightAir_Game.h` | add `const ProjectorSet* projectors`; drop `const` on `onBegin`'s runner |
| `src/game/LightAir_GameRunner.h/.cpp` | register the set in `begin()`, apply queue + availability in `flushOutput()`, `projector()` accessor |
| `src/game/LightAir_GameVar.h` | optional `labels` on `ConfigVar` (§6.6); `MonitorVar::IntDyn` for a runtime icon (§8.2) |
| `src/game/LightAir_GameSetupMenu.cpp` | render `labels` when present |
| `src/enlight/Enlight.h/.cpp` | `setRangeM`, range-derived thresholds in `classify()`, `RANGE_FALLOFF_EXP` |
| `src/nvs_config.h/.cpp` | `refFarR/G/B` keys; retire `limpow` |
| `src/tools/EnlightTestMode.cpp` | replace the local `MAX_REPS` with `ProjectorLimits::MAX_CYCLES` |
| `src/tools/EnlightCalibRoutine.cpp` | step 1 prompt, step 2 saves the reference, step 4 shows `Rmax` |
| `src/ui/player/LightAir_UICtrl.h/.cpp` | `UIEvent::ProjectorChange` + one table row; `setEnlightAction()` extending the `resolveAction()` override hook (§8.1). `.cpp:352` and `:129` stay untouched. |
| `src/LightAir.h` | include the two new headers |
| `src/rulesets/*.cpp` (×5) | one `projectors` field; `dmg(pkt)` in the lit conditions; `onBegin` signature + one `setPool()` call; the identical recharge block deleted (§2.5); `litAt[]`/`notImmune()`/`REPLY_IMMUNE` removed (§7.4); a `Projectors` config var |
| `src/rulesets/GameOutflow.cpp` | as above, but a `Recharge::NONE` projector: `tickDrain()` and the depletion rule stay; its own `energy--` in the shot loop goes; the uncapped kill reward writes the pool directly (§2.5.1) |

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
4. **Naming of `strength`** (§2.7).
5. **Per-colour range accuracy.** The gate necessarily uses a reference target's
   reflectivity, so darker players are gated closer (§2.3). A per-colour
   reference table with a post-classification gate would fix it; probably v2.
6. **Persistence across games.** An unlocked-projector bitmask in NVS would turn
   totem rewards into meta-progression. It changes the NVS layout and has real
   fairness implications between players with different play histories — suggest
   explicitly leaving it out of v1.

---

## 12. Worked migration: `GameOutflow`

`GameOutflow` is the hardest of the six — energy is simultaneously ammo and life
total, it is the only ruleset with passive drain, and it is the reason
`Recharge::NONE` exists. If the design survives it, the other five are
mechanical. Line numbers are from the current file.

### 12.1 The projector it declares

```cpp
static const Projector outflowProjectors[] = {
  { ProjectorId::CUSTOM1, "OUTFLOW",
    /* cycles          */ 20,     // was enlightPtr->setRepetitions(20)  L209
    /* cooldownMs      */ 20,     // was enlightPtr->setCooldown(20)     L208
    /* rangeM          */ 0,      // device max — today's classifier behaviour
    /* recharge        */ Recharge::NONE,
    /* energyCost      */ 1,      // was the bare energy-- at L303
    /* maxEnergy       */ 100,    // startEnergy default; setPool() overrides it
    /* rechargeDelayMs */ 0, /* rechargeMs */ 0,
    /* strength        */ 1,      // one standard hit = hitDmg energy (§7.5)
    /* roleTag         */ 0,
    /* targetImmunityMs*/ 0,      // Outflow has NO immunity today — must stay 0
    /* readyMs         */ 0,      // single projector, nothing to switch to
    /* shotAction      */ nullptr,             // keep the standard Enlight action
    /* icon            */ ICON_ENERGY_BITMAP,  // keep today's display
  },
};

static const ProjectorSet projectorSet = {
    outflowProjectors, 1,
    (1u << ProjectorId::CUSTOM1),
    ProjectorId::CUSTOM1,   // the permanent slot-0 projector — not BASE
    nullptr,                // maxOwned unused: nothing else is grantable
    nullptr,                // isAvailable: always
};
```

Two values in there are load-bearing and easy to get wrong:
`targetImmunityMs = 0`, because Outflow is the one ruleset with no `litAt[]`
table and no immunity rule at all — inheriting `BASE`'s 3000 ms would silently
change the game; and `startId = CUSTOM1` rather than `BASE`, because §2.5.4 makes
`startId` the permanent projector and Outflow's baseline is its own.

### 12.2 Every edit, in file order

| # | Line | Today | After |
|---|---|---|---|
| 1 | 78 | `static int energy = 100;` | **deleted** — the pool is `projectorEnergy` |
| 2 | 97 | `{ "Energy", &energy, … }` | `{ "Energy", &startEnergy, … }` — **fixes a live bug, see §12.4** |
| 3 | 107 | `MonitorVar::Int("Energy", &energy, …)` | `&projectorEnergy` |
| 4 | 119-120 | `energy > hitDmg` / `energy <= hitDmg` | `projectorEnergy > dmg(pkt)*hitDmg` / `<=` |
| 5 | 124-125 | `energy -= hitDmg; if (energy < 0) energy = 0;` | `projectorEnergy -= dmg(pkt)*hitDmg;` + same clamp |
| 6 | 160 | `energy += startEnergy;` (uncapped kill reward) | `projectorEnergy += startEnergy;` — a **direct write, never clamped** (§2.5.1) |
| 7 | 196 | `energy = startEnergy;` | `runner.projector().setPool(startEnergy, 0);` |
| 8 | 208-209 | `setCooldown(20); setRepetitions(20);` | **deleted** — the projector carries both |
| 9 | 232 | `energy--;` (passive drain) | `projectorEnergy--;` — the drain stays entirely Outflow's |
| 10 | 268 | `energy = startEnergy;` (respawn) | `projectorEnergy = startEnergy;` |
| 11 | 296-298 | `out.radio.sendTo(r.id, MSG_LIT)` | `sendTo(r.id, MSG_LIT, payload, 3)` with strength/id/roleTag (§7.2) |
| 12 | 299-305 | `if ((energy > 0) && (enlightPtr->run())) { energy--; energySpent++; triggerEnlight(…); }` | `if (projector.fire()) energySpent++;` (§12.3) |
| 13 | 312 | `if (energy == 0 && !pendingShone)` | `if (projectorEnergy <= 0 && !pendingShone)` — `<=` because a hit can now overshoot |
| 14 | descriptor | — | `/* projectors */ Outflow::projectorSet,` |

Net: roughly ten lines changed, two deleted, one added. The passive drain,
`pendingShone` / `pendingDepletion`, the respawn timer, the state machine, the
scoring and all five config vars are untouched.

### 12.3 `fire()` — selection is queued, firing is immediate

Line 12 above replaces the four-part shot idiom that all six rulesets repeat.
`ProjectorCtrl::fire()` performs, in one place, what each copy does by hand:

1. reject if `readyMs` has not elapsed since the last switch (§2.5.5);
2. reject if `projectorEnergy < energyCost`;
3. call `enlight.run()`, and reject if *it* refuses (still in cooldown);
4. on success only: deduct `energyCost`, queue the shot's UI action with
   `cycleTime()`, and stamp `lastShotAt` for the recharge delay.

Note the ordering: energy is deducted **only when `enlight.run()` returns true**,
which is exactly today's `(energy > 0) && (enlightPtr->run())` short-circuit. A
refused shot has never cost energy and still must not.

`fire()` is a **direct call, not a queued output** — unlike `out.proj.select()`.
That is deliberate and matches the existing code: `enlightPtr->run()` is already
called straight from the behavior, the measurement must start on this tick, and
`Enlight` self-guards re-entry. Queueing the shot would delay it by a loop
iteration for no benefit. The ruleset keeps its own statistics (`energySpent++`)
because those are game data, not projector state.

### 12.4 A live bug the migration forces into the open

```cpp
static int startEnergy = 100;                       // L71 — never menu-bound
static const ConfigVar configVars[] = {
    { "Energy", &energy, 50, 200, 25 },             // L97 — targets the RUNTIME var
};
static void onBegin(…) { energy = startEnergy; … }  // L196 — overwrites it
```

The `Energy` config var writes `energy`, which `onBegin` immediately overwrites
with the hardcoded `startEnergy = 100`. **The DM's Energy setting has no effect
today**, and neither do the kill reward (L160) or respawn (L268), which both read
`startEnergy`.

This is worth knowing for two reasons. It is a one-word fix (`&energy` →
`&startEnergy`) that is safe at the default — the menu opens at 100, which is
what the game already uses, so a DM who leaves it alone sees no change. And the
migration **cannot preserve the bug even accidentally**: edit #1 deletes the
variable the config var wrongly points at, so the compiler forces the question.

### 12.5 Equivalence check, mechanic by mechanic

| Mechanic | Preserved? |
|---|---|
| 1 energy per shot | Yes — `energyCost = 1`, deducted only on a successful `run()`, as today |
| Passive drain every `10000/drainRate` ms | Yes — `tickDrain()` untouched, now writing `projectorEnergy` |
| Hit costs `hitDmg`, clamped at 0 | Yes — `dmg(pkt) = 1` from a `strength = 1` projector, so `1 * hitDmg` (§7.5) |
| Kill grants `startEnergy`, **uncapped** | Yes — direct write; `NONE` mode never refills, and clamping is refill-only |
| Energy 0 ⇒ `pendingDepletion` ⇒ OUT_GAME | Yes — condition moves to `<= 0` |
| Respawn restores `startEnergy` | Yes |
| No recharge, ever | Yes — that is `Recharge::NONE` |
| Projector never lost at 0 energy | Yes — `NONE`, not `CONSUMED`; the ruleset owns what zero means |
| Cooldown 20 ms, 20 cycles | Yes — carried by the projector instead of two `onBegin` calls |
| No hit immunity | Yes — **only because `targetImmunityMs = 0` is set explicitly** |
| Energy shown with `ICON_ENERGY` | Yes — the projector's `icon` is that same bitmap |
| Shot sound / vibration | Yes — `shotAction = nullptr` keeps the standard `Enlight` action |
| Energy may exceed 255 after kills | Yes — **but see §12.6** |

### 12.6 Two type constraints Outflow imposes on the framework

Both are framework-side and would be silent corruption if missed:

- **`ProjectorSlot::energy` must be `int16_t`, not `uint8_t`.** Outflow's kill
  reward is uncapped and additive: three kills without spending puts the pool at
  400. `maxEnergy` can stay `uint8_t` — it is only the refill target and the
  starting value, never a ceiling on a direct write — but the live pool cannot.
  `projectorEnergy` is likewise `int`, which `MonitorVar::Int` requires anyway.
- **`ProjectorLimits::MAX_ENERGY` bounds `maxEnergy` only.** Outflow's config var
  allows 200, within the 200 limit; the *live* pool is deliberately unbounded
  above. Clamping the pool to `MAX_ENERGY` on every write would break the kill
  reward exactly as clamping to `maxEnergy` would.
