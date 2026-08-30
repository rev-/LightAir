# The projector

The object between the game, the ruleset and `Enlight`: the light-beam
device a player carries.  It owns the optics in hand, the energy a beam
costs, how that energy comes back, how far the beam reaches, what a hit
weighs on the wire, and which projectors the player is carrying.

It lives in `games/lib/projector.lua`.  This file records the decisions
behind it — the things the code cannot say about itself.

---

## 1. Where the line is drawn

`Enlight` never learns what a projector is.  Three verbs carry everything
across the boundary:

| Verb | Direction |
|---|---|
| `la.shine_config{reps, cooldown_ms}` | the optics, pushed on a switch |
| `la.shine()` | start a burst; returns whether it was accepted |
| `la.shine_result()` | `status, id, metres, r, ang` |

Everything else — whether to fire, what it cost, whether the target was in
reach, what the hit weighs — is decided in Lua.

The reason is the rule in `docs/lua-games-design.md` §"API layering": a
kernel verb is admitted only for a *capability*, never a *policy*.  Energy
pools, recharge modes, reach and hit weight are balance, and balance
belongs where it can be retuned by shipping a file rather than reflashing.
The projector's C++ half is the eight calls above.

**A C++ projector would have needed a wider boundary, not a narrower one.**
Under a Lua game it would have to re-export its whole surface —
give/select/drop/next/energy/owns/… — as roughly nineteen verbs, plus
struct marshalling for profiles declared in a game file, and it could not
accept `is_available` as a Lua function at all.

---

## 2. Range is reported, never gated

`Enlight::classify()` computes an estimated distance and reports it.  It
gates on nothing but its own calibrated validity floor (`thresh_far_*`),
below which the colour coordinates are noise and a box match would be
meaningless.

The retired projector branch put a range gate *inside* `classify()`.  That
is not ported, deliberately. A gate in the driver freezes one policy into
firmware; reporting the measurement instead lets a profile gate on
distance, correct for target colour, or grade an effect by range — all as
data, none of it a firmware release.

### The model

Retroreflector return falls as 1/xⁿ, so one reference measurement at a
known distance fixes the whole curve:

```
R = refDist * (refSum / measSumPerCycle)^(1/n)
```

Both sides are baseline-subtracted and normalised per DMA cycle, which is
what makes the estimate independent of the repetition count the active
projector happens to have chosen.

`n` is `EnlightDefaults::RANGE_FALLOFF_EXP`, and the reference is captured
by calibration step 1 — which already shoots a clear target fifty times —
with the baselines subtracted in step 2, where they are finally known.

**Never export the raw correlator sums to Lua.** `rawMeasure()` returns
`_rout/_gout/_bout` *before* baseline subtraction, scaling with
`_repetitions` and `_activePeriods`. A Lua constant over those would be
both per-device and per-profile, which would break the file-sharing model
the whole architecture rests on.

### Fails open

A device with no reference calibration reports 0 metres, and a profile
that declares no `range_m` gates on nothing.  Either way the behaviour is
what it was before ranges existed — an uncalibrated device stays playable.

### Open questions

1. **`RANGE_FALLOFF_EXP = 3` and `CAL_REF_DIST_M = 5` are unmeasured.**
   Both are one-constant edits. `Rmax` in the calibration summary is the
   cheapest field check: walk it.
2. **Does the calibration actually support the README's 40 m?** `Rmax` will
   say. If it comes out well short, the classifier is already gating
   shorter than the hardware can see — independent of this feature, but
   this is what reveals it.
3. **Per-colour accuracy.** The estimate uses one reference target's
   reflectivity, so darker players read as further away. A per-colour
   correction table would fix it, and unlike in a C++ design it is a table
   in a Lua file, not a firmware change.

---

## 3. Hit weight

`strength` counts **standard hits**, not health points.  Each ruleset
decides what one standard hit is absorbed as in its own currency — a life
in Teams, N energy in Outflow. `std.absorbed(pkt)` reads payload byte 1,
and an **empty payload counts as one standard hit**, which is what lets a
ruleset move to the projector without every other ruleset moving with it.

MSG.LIT payload: `[strength, projector id, role tag, rssi gate]`.

---

## 4. RSSI is a plausibility bound, not a range control

A profile may declare `rssi_min`, which travels in the LIT payload and lets
the receiver refuse a hit. It is deliberately *not* the primary range
mechanism, for four reasons:

- the shooter already has a **calibrated optical distance**, which is
  line-of-sight by construction — gating at the receiver moves the decision
  to the party with worse information;
- body shadowing at 2.4 GHz costs 10–20 dB, and players hold the radios and
  turn constantly, so a threshold is a distance × orientation × luck;
- RSSI carries a per-device offset, so a constant in a shared `.lua` means
  different reach on different hardware;
- the failure mode is invisible: optics hit, packet arrives, receiver
  declines silently, and the shooter reads it as broken hardware.

So `std.lit_target` honours the gate **only for a ruleset that declares a
`far` reply to report the refusal with**. The gate is available exactly to
games that can say why they refused.

Every existing RSSI use in this codebase is a coarse proximity gate with
generous margin (−57 ≈ 2 m, −62 ≈ 3–4 m, −65 ≈ 3 m) against a *stationary*
totem. A tight gate against a moving human is a much harder ask.

### The measurement worth taking

Every LIT now carries both numbers: the attacker's optical `metres` and the
receiver's `pkt.rssi` for the same event. Log a few hundred hits at marked
distances.

- spread within ~6 dB at p90 → a gameplay-shaping `rssi_min` is viable;
- spread of 15 dB → keep it loose (~−85) as a sanity bound only.

Splash is unaffected either way: its radius is *meant* to be fuzzy.

---

## 5. Splash

A player who absorbs a beam broadcasts a beacon; bystanders take graded
damage from its RSSI. The reach comes from the **attacker's** profile and
is relayed by the victim.

RSSI is the right tool here and the wrong one in §4, for three reasons: a
splash radius is inherently fuzzy, graded bands degrade by one step rather
than between hit and nothing, and there is no optical measurement to a
bystander who was never aimed at — so RSSI is not a worse choice than
something better, it is the only choice.

MSG.SPLASH payload: `[projector id, strength, rssi gate, origin, shooter id]`,
single-hop.

**One of the four standard profiles carries it: `proj.standard.SPLASH`.**
Splash is loud,
in radio traffic and in play, and a field where every projector splashed
would be chaos rather than tactics — so the burst is a thing you choose to
pick up, not a property of shining. Its direct hit is a single standard hit;
the point is the two-band beacon it triggers around whoever it lands on. It
carries its own icon, its own shine feedback, a long cooldown and a slow
refill, so it reads and feels different in the hand.

---

## 6. The standard catalogue

Four ready-made profiles a game can drop into its `profiles` list, each
with its own icon, shine feedback, optics and economy:

| | id | cycles | cooldown | range | recharge | cost / pool | strength | ready |
|---|---|---|---|---|---|---|---|---|
| SPLASH | 1 | 20 | 900 ms | 12 m | refill 6000 ms | 2 / 8 | 1 + burst | 600 ms |
| FAST | 2 | 4 | 60 ms | 20 m | ramp 1500→3000 ms | 1 / 30 | 1 | 150 ms |
| LONG | 3 | 30 | 400 ms | device max | refill 4000 ms | 1 / 20 | 1 | 400 ms |
| STRONG | 4 | 15 | 900 ms | 20 m | refill 6000 ms | 1 / 8 | **3** | 400 ms |

Every duration a profile declares is in **milliseconds**. Seconds are too
coarse to separate a projector that snaps back from one that crawls. The
config menu still edits its own vars in seconds, and a profile that reads
one scales it with a function field:

```lua
recharge_delay_ms = function(vars) return vars.recharge_secs * 1000 end
```

A profile field may be a literal, the id of a game var, or a function of
vars — the var-id form is what keeps menu-owned values live.

Ids are **fixed and reserved**, because a projector id travels on the wire:
a splash beacon names the projector that fired and every receiver looks the
profile up by that id locally. A game's own profiles start above this range.

FAST's short reach is not an arbitrary nerf — four cycles is a short
integration and therefore genuinely less gain, so gating it keeps the
profile from producing unreliable long-range hits. LONG's `range_m = 0`
leaves the profile out of the way entirely and lets the calibrated floor
decide what the device can see.

STRONG weighs **three standard hits**, which in a lives game is three lives
from one beam. One energy per shot, but only eight of them and six seconds
to get them back.

### Shine feedback: the burst is the whole action, not each note

A projector has to be recognisable by its **pattern**, not by the pitch of a
single note — with four in the catalogue, one beep each is not enough to
tell them apart.

So `LightAir_UICtrl::burstStepMs()` reads a shine action's declared
durations as a **shape** and scales them to fit the burst: a 1:3 pair inside
a 300 ms beam plays 75 ms then 225 ms, and the total is exactly the beam.
Boundaries are computed cumulatively, so integer rounding cannot drift and
the last step always lands on the burst exactly. No step is ever
zero-length, or the ticker would stall.

That is a fix, not a constraint: previously each step took the *full* burst,
so an N-step action ran N times too long. The catalogue's signatures are now
two rising ticks (FAST), a chirp into a held tone (LONG), three descending
notes (STRONG), and a punch that flares (SPLASH).

Five guards, each with a test that fails when it is removed:

| Guard | Why |
|---|---|
| only a direct optical LIT emits (`origin`), and `on_splash` never calls `emit_splash` | otherwise one beam cascades across the field |
| single-hop broadcast, never a relay | a flooded splash is the opposite of a radius |
| a player never splashes themselves | the emitter already took the direct hit |
| one beacon per shot, rate-limited | repeated hits would flood the channel |
| the shooter's id travels | friendly fire is judged against whoever fired, not the victim who relayed |

**Bands are the reach.** When a profile declares them, the outermost band
is the cutoff and the flat `rssi` is just the one-band shorthand; the gate
byte on the wire carries the outermost band so a receiver falling back to
it admits everyone the bands would.

---

## 6. The reload bar

The LCD shows energy as a number, and as a filling bar while the pool is
empty. The clock cannot be the moment energy hit zero: with a `refill`
recharge the wait starts when the **trigger is released**, so a player
holding a dead trigger would watch a bar complete while nothing came back.

So the projector publishes both halves as ordinary game vars — the instant
the wait began, and how long it takes — and `bindBarVariable` reads both
through pointers. Pressing again clears the anchor, because with a refill
recharge nothing comes back until the next release either.

A monitor row spells it:

```lua
{ var = "energy", icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME },
  bar = true, bar_at = 0, fill_var = "reload_secs", start_var = "reload" }
```

The row is declarative because binding sets are built once in
`GameRunner::begin()` and lock on first activation — there is no later
moment at which the projector could add one. The *timing* behind both
pointers stays the projector's.

---

## 7. Deliberately not built

| Not built | Why |
|---|---|
| a range gate inside `Enlight` | §2 — policy in the driver |
| `ProjectorOutput` (the switch queue) | the output stack subsumes it; `la.shine_config` queues like every other effect |
| `ShinePolicy` | the game's `update` is the loop |
| `LightAir_ProjectorCtrl` in C++ | §1 — it would need a wider boundary, not a narrower one |
| persistence of unlocked projectors across matches | changes the NVS layout and has real fairness implications between players with different play histories |

---

## 8. The projector is the only route to Enlight

Every ruleset goes through it, with no exceptions — including
`freeforall`, which used to be the deliberately library-free tutorial.

Two reasons it has to be all of them. A ruleset that fires or polls on its
own bypasses the energy cost, the reach, the hit weight and the splash. And
`la.shine_result()` and `la.shine_lit()` **both poll, and the poll is
read-and-clear**, so a game calling one while the projector calls the other
would eat measurements at random — an intermittent fault that would look
like flaky hardware.

`test/host/test_games.lua` enforces it: every game file is scanned for
`la.shine*` calls and the suite fails on any that reach Enlight directly.

---

## 9. The recharge clock

The wait is anchored by the trigger release that **follows an accepted
beam**, and once anchored it runs to completion:

- a press that spends nothing — an empty pool — does **not** re-anchor it,
  so leaning on a dead trigger cannot push the refill further out;
- nor does it **block** it: the energy arrives on time even with the
  trigger held down across the moment it is due;
- only a press that actually fires restarts the clock, from its own
  release.

Nothing recharges between an accepted beam and its release, because until
that release the wait has not begun — otherwise a player who had been idle
would see the pool refill on the very tick they emptied it.
