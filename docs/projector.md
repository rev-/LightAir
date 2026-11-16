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
accept a profile field written as a Lua function of the game's vars at all.

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

### `range_m` is a label

The projector does not gate on the estimate either, for now.  A profile's
`range_m` is a label (what the profile is meant to reach: BASE 40 m, FAST
30 m, LONG no limit, STRONG and SPLASH 40 m), and `proj.result()` hands
back every player hit with its estimated distance.  Until the open
questions below are measured, refusing a hit on that number would turn
estimation error into missed shots.  What actually buys reach is a
profile's `cycles`: more integration, more gain.

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

## 3. What a beam carries

MSG.LIT payload, eight bytes, built by `proj.payload(vars [, spare1, spare2])`:

| Byte | Content |
|---|---|
| 1 | strength, in the game's own units |
| 2 | kind: a mask of 8 the game gives its meaning to |
| 3 | role tag: the shining player's class or role |
| 4 | RSSI gate (§4) |
| 5–6 | effects: 16 bits, one per receiver-side effect (§12) |
| 7–8 | spare, the ruleset's |

**No projector id travels.**  A receiver acts on what a beam *is* — its
kind, its effects — never on which projector produced it.  The firmware
used the id for one thing, finding the area a beam triggers; that is an
effect bit now (§5).

### Strength and `game_unit`

A profile states `strength` in **standard units** (the baseline is 1,
STRONG is 3).  `proj.define{ game_unit = n }` says what one standard unit
is worth in this game's own currency, and the wire carries
`strength × game_unit`:

- `game_unit = 1` (the default): a coarse game, 1 to 5 lives;
- `game_unit = 10`: a fine game, about 100.

So one catalogue serves both, and a fine game may state fractions
(`strength = 0.5` is 5).  An area's bands scale the same way.  A profile
that only makes sense fine-grained declares `min_unit = 10`: a coarser game
does not know it, and does not offer it as a bonus.  `proj.units(n)` is the
same scaling for a field that is not scaled on its own.

Each ruleset still decides what a unit is absorbed as — a life in Teams, N
energy in Outflow.  `std.absorbed(pkt)` reads payload byte 1, and an empty
payload counts as 1.

### Kind and role tag

`kind` is the projector's: a profile declares it (a mask, so kinds combine),
and `proj.set_kind(mask, on [, sticky])` adds or removes bits at run time.
`role tag` is the player's: `proj.set_role_tag(n)`, set by the ruleset or
the player's class.  A profile no longer declares `role_tag`; one that does
refuses the load.

The rule for choosing between a kind and an effect: if the outcome is the
same for every target, it is an **effect**; if the target decides (its
class, the game's own rule), it is a **kind**.

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

A player who absorbs a SPLASH beam becomes the centre of an area: their
device broadcasts a beacon, and bystanders take graded damage from its RSSI.
The bands come from the projector's area policy, the credit goes to the
**shooter**, and the victim only relays.

RSSI is the right tool here and the wrong one in §4, for three reasons: a
splash radius is inherently fuzzy, graded bands degrade by one step rather
than between hit and nothing, and there is no optical measurement to a
bystander who was never aimed at — so RSSI is not a worse choice than
something better, it is the only choice.

**An area is triggered by an effect.**  A landed LIT whose effects word
carries a policy's bit starts that policy's area.  The standard one is the
`splash` effect (bit 8); a game profile that declares an `area` of its own
gets the next free bit (9–15, in id order, so every device agrees).  A
profile with an area always carries its effect, and because it is an
effect like any other, `proj.set_effect(vars, "splash", true)` makes
whatever is in hand splash.

**One of the four standard profiles carries it: `proj.standard.SPLASH`.**
Splash is loud,
in radio traffic and in play, and a field where every projector splashed
would be chaos rather than tactics — so the burst is a thing you choose to
pick up, not a property of shining. Its direct hit is a single standard hit;
the point is the two-band beacon it triggers around whoever it lands on. It
carries its own icon, its own shine feedback, a long cooldown and a slow
refill, so it reads and feels different in the hand.

### The area service is the firmware's, the hit is the ruleset's

Splash is one use of a general mechanism, the **area service** in
`LightAir_GameRunner` (wire format in `src/game/LightAir_AreaEffect.h`,
policy in `LightAir_Game.h` §7b).  A profile declares its area as data:

```lua
area = {
  on       = "lit",                      -- or "shone": the knock-out alone
  bands    = { { -55, 2 }, { -70, 1 } }, -- { rssi floor, hit strength }
  friendly = "game",                     -- or "never"
  self     = false,                      -- may the shooter be caught
  credit   = true,                       -- an area knock-out scores for the shooter
  effects  = { "dim" },                  -- optional: what a bystander's hit carries
},
```

and `define()` registers it with `la.area_policy` under the projector's own
id, with the effect bit that triggers it.  A bystander's hit carries the
policy's own `effects`, never the beam's — otherwise a `mute` beam with
splash would mute everyone nearby without anyone having chosen that.  Every device runs the same file, so every device holds the same
policies; no game file mentions splash.  From there the firmware does it:

- **Trigger (the victim).** A LIT from that projector which the ruleset
  answered TAKEN or SHONE (`la.hit`) makes this player the centre: the
  runner broadcasts `MSG_AREA [policy, shooter, shooter's team, role
  tag, kind]`, single-hop — the last two are the beam's, so a bystander's
  hit says who shone and with what kind, as the direct hit did.  `on = "shone"` triggers on the knock-out alone.  A refused
  hit (immune, friendly, already out, no effect) triggers nothing.
- **Receive (the bystander).** A beacon in reach becomes a LIT from the
  **shooter**, at the first band its RSSI reaches, flagged as an area hit
  (`pkt.area`), handed to the state's own LIT handler — so lives, friendly
  fire and being out mean exactly what that ruleset says they mean.  Its
  reply goes nowhere: the shooter never sent it.
- **Credit (the shooter).** An area hit the ruleset answered SHONE is sent
  back to the shooter (`MSG_AREA_CREDIT`, acknowledged), whose runner plays
  it to the ruleset as a SHONE reply to one of its own LITs — each game
  scores it exactly as it scores a direct knock-out, and `reply.sender`
  names who went down.

**Immunity is the beam's.**  `std.lit_target` refuses a direct hit inside
the shooter's immunity window and opens one, for SPLASH as for every
projector; an area hit neither checks the window nor opens it.  Hit B with
SPLASH and A and C, close by, take the area; hit A straight away and C takes
it again.

**Self is the policy's.**  With `self = false` (SPLASH) the shooter is never
caught in their own area; with `self = true` they are, and neither a
`friendly = "never"` policy nor the game's friendly-fire rule overrides it.

**The reply convention.**  The trigger reads two codes from the ruleset's
LIT reply, the named constants `la.hit.TAKEN = 1` and `la.hit.SHONE = 2`;
every other reply means the hit did not land.  A ruleset that takes hits
uses them for those two meanings (every game's `R` table starts from them).

**A ruleset's own area.**  `la.area_policy(id, spec)` with no `on` declares
an area nothing triggers; the ruleset starts it with `la.area_emit(id)`
from a handler, a rule or `update`, with this player as centre and
originator.  It returns false from inside an area hit's handler.

**Virus.**  An area hit carries the role tag of whoever's beam set it off.
Only a clean player can hold SPLASH (a virus's projector is its role), so
in Virus an area hit is a clean one: nothing to a clean player, and down
for a virus — a clean SPLASH puts down the viruses standing near the one it
hits.

Guards, each with a test that fails when it is removed:

| Guard | Why |
|---|---|
| an area hit never triggers a beacon, and `la.area_emit` refuses inside one | otherwise one beam cascades across the field |
| the area flag is local: a LIT from the air has it cleared | a peer could otherwise skip every immunity window |
| an area hit never carries its policy's trigger bit | a second guard against a cascade, should the first ever be touched |
| single-hop broadcast, never a relay | a flooded area is the opposite of a radius |
| one triggered beacon per 250 ms (`EMIT_MIN_GAP_MS`) | repeated hits would flood the channel |
| the shooter's id and team travel | friendly fire is judged against whoever fired, not the victim who relayed |
| `self` alone decides about the shooter | with friendly fire on, nothing else would spare them |
| an area hit's reply is dropped; only SHONE sends a credit | the shooter never sent a LIT to be answered |
| a held player takes an area hit only if `hold.accept` takes a LIT | an area hit is a LIT |
| policies are declared while the game loads, never later | the runner reads the count once |

---

## 6. The standard catalogue

Four ready-made profiles a game can drop into its `profiles` list, each
with its own icon, shine feedback, optics and economy.

### Why they are relative to the baseline

The menu owns the baseline's pool and recharge. With a catalogue of fixed
numbers, a host who set up a strong baseline (a big pool, a fast refill)
made every "powered" projector a weaker version of the one already in
hand. So every standard profile states its values as **relations to the
baseline's resolved value**, read through the game's config vars on the
tick:

```lua
cycles      = proj.rel("cycles", function(r) return r * 5 end),
cooldown_ms = proj.rel("cooldown_ms", function(k) return k // 2 end),
cost        = proj.rel("cost"),                       -- same as the baseline
```

That includes the values the baseline fixes today (cycles, cooldown):
they may become menu values, and a relation keeps holding when they do.
`proj.rel` follows a game's *own* baseline (id 0) when it declares one, and
clamps the result to the field's limits, like a literal at load. Every
receiver resolves against the same config, so a profile looked up by the
id a LIT carries (an area hit's included) means the same thing on every
device.

| | id | pool | recharge | cooldown | cycles | strength | range label | ready |
|---|---|---|---|---|---|---|---|---|
| BASE | 0 | menu (`start_energy`) | refill, menu (`recharge_secs`) | 50 ms | 10 | 1 | 40 m | 0 |
| SPLASH | 1 | B/2 | refill, B/2 | 2·B | B | B + burst | 40 m | 600 ms |
| FAST | 2 | B | ramp: idle B/2 − 500 ms, then 10 ms per unit | B/2 | B | B | 30 m | 150 ms |
| LONG | 3 | B/2 | refill, B | B/2 | 5·B | B | none | 400 ms |
| STRONG | 4 | B/2 | refill, B/2 | 2·B | B | **3** | 40 m | 400 ms |

B is the baseline's value. Every projector costs what the baseline costs
(1). A halved pool rounds down, never below 1. At the menu defaults (30
energy, 10 s) that is: FAST 30 energy, 4.5 s idle then 0.3 s of trickle;
STRONG and SPLASH 15 energy back in 5 s; LONG 15 energy of 400 ms beams.

The baseline declares its optics outright. It used to leave `cycles` to
whatever Enlight held, so switching back from LONG kept LONG's beam; a
profile that names no `cycles` now takes the baseline's.

Every duration a profile declares is in **milliseconds**. Seconds are too
coarse to separate a projector that snaps back from one that crawls. The
config menu still edits its own vars in seconds, and a profile that reads
one scales it with a function field:

```lua
recharge_delay_ms = function(vars) return vars.recharge_secs * 1000 end
```

A profile field may be a literal, the id of a game var, or a function of
vars — the var-id form is what keeps menu-owned values live.

Ids are **fixed and reserved**: an area beacon names its policy by the
projector's id, the BONUS menu lists projectors by it, and every device
must agree on both. A game's own profiles start above this range.

STRONG weighs **three standard hits**: in a lives game three lives, in
Outflow three times `lit_cost` of energy
from one beam.

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

The bar only shows while the pool is **empty**, so it times the wait until
energy starts coming back, and nothing more: `recharge_delay_ms` for every
mode. A `ramp` projector (FAST) starts trickling the instant that idle
ends, the pool leaves zero and the number takes over. Timing the bar to the
whole refill made it vanish a third of the way across.

The cell's **icon** follows the projector in hand. The projector writes an
`la.icons` value into the var it is given as `vars.icon`, and the energy
row reads it through `icon_var`; every game that offers projector bonuses
wires both, and the host suite fails one that does not:

```lua
proj.define{ vars = { ..., icon = "energy_icon" } }
vars    = { ..., { id = "energy_icon", default = la.icons.ENERGY } }
monitor = { { var = "energy", ..., icon_var = "energy_icon" } }
```

A monitor row spells it:

```lua
{ var = "energy", icon = "ENERGY", col = 1, row = 0, states = { S.IN_GAME },
  bar = true, bar_at = 0, fill_var = "reload_ms", start_var = "reload" }
```

The row is declarative because binding sets are built once in
`GameRunner::begin()` and lock on first activation — there is no later
moment at which the projector could add one. The *timing* behind both
pointers stays the projector's.

**The respawn wait uses the same shape.** Every ruleset with a respawn
timer shows a bar in the state where the player waits (OUT_GAME; DOWN in
festasportsasso), filling over `respawn_secs`. `std.respawn_wait(vars,
secs)` starts the wait and writes the three vars the row reads:

```lua
respawn_at = std.respawn_wait(vars, vars.respawn_secs)

{ var = "respawn_zero", icon = "DOWN", col = 1, row = 0, states = { S.OUT_GAME },
  bar = true, bar_at = 0, fill_var = "respawn_ms", start_var = "respawn_from" }
```

It is anchored on the instant the wait began, so a hold or a tools menu
mid-wait cannot restart it. Where the way back is a BASE, the bar covers
the timer only: once it is full the player still has to reach a base.

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
`freeforall`, the reference game every new one is copied from.

Two reasons it has to be all of them. A ruleset that fires or polls on its
own bypasses the energy cost, the reach, the hit weight and the splash. And
`la.shine_result()` and `la.shine_lit()` **both poll, and the poll is
read-and-clear**, so a game calling one while the projector calls the other
would eat measurements at random — an intermittent fault that would look
like flaky hardware.

`test/host/test_games.lua` enforces it: every game file is scanned for
`la.shine*` calls and the suite fails on any that reach Enlight directly.

The same read-and-clear poll is why the game is **unplugged** from Enlight
while a tool borrows it (the in-game calibration, opened with A+B): the
runner sets the `enlightPtr` handle every `la.shine*` verb guards on to
null for the whole hold, so the ruleset sees a device with no optics
rather than stealing the tool's measurements.  Optics the ruleset queues
meanwhile are kept and applied when the hold ends; the calibration itself
saves and restores the repetitions and cooldown it overrides.  See
`src/game/LightAir_GameHold.h`.

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

**Ramp policy.** A `ramp` recharge trickles from the **end of the wait**:
the first unit arrives the instant `recharge_delay_ms` has passed since the
release, then one every `recharge_step_ms` (FAST: 10 ms), or every
`recharge_ms / max_energy` for a profile that states the whole duration
instead. Nothing is credited for
the wait itself. (It used to count from the last beam, so the moment the
wait ended it paid out everything "earned" during it at once — half of
FAST's pool in one jump.)

---

## 10. Pickups: projector bonuses and DIM

A BONUS totem can hand out a projector, and a MALUS totem can DIM the
player. The DM picks which one each totem does, with O in the Totems
submenu. `std.pickup_effect` applies it when the player claims the totem.

**The standard catalogue is always known.** `define()` registers SPLASH,
FAST, LONG and STRONG in every game, declared or not, so a bonus can give
any of them. Known is not owned: the inventory still starts with the
baseline alone. SPLASH's area policy is registered in every game for the
same reason, so a SPLASH bonus splashes wherever it is picked up.

**`proj.bonus_options()`** builds the BONUS list: `LIFE`, the catalogue in
id order, then the game's own profiles. A profile marked `bonus = false`
(a practice or role projector: TRIAL, VIRUS) is left out. Every game with
pickups offers the catalogue, Outflow included (below). Labels are cut to
the menu's 8 characters, and `proj.bonus_id(label)` maps them back.

A projector bonus is `grant`ed: given at full energy and put in hand.
`max_owned` still applies, so it may evict the oldest powered projector.
The tray says which (`LOST FAST`).

**Switching.**  `<` and `>` step through what the player holds, on the
key's release, and the tray names the one now in hand.  Both keys together
are a chord some rulesets use (FestaSportSasso's restart), so a key that
was part of one switches nothing.  A switch costs the new projector's
`ready_ms`.  `proj.define{ switch_keys = false }` turns the keys off.

**A shared pool: when the pool is the player's life.** In Outflow energy is
ammo and life at once, so a projector bringing its own pool would swap the
player's life on every pickup. `proj.define{ shared_pool = true }` gives
every projector the player holds ONE pool, the baseline's:

- its size, its recharge (Outflow's: none) and DIM are the baseline's; a
  powered projector's own pool and recharge are never used, so holding
  FAST heals nothing;
- a pickup puts the projector in hand and leaves the pool as it is, and a
  re-grant refills nothing: "full energy" would be a free heal;
- switching moves no energy;
- each projector keeps everything else: optics, cost per beam, strength,
  ready delay, icon, feedback and area. A STRONG beam costs Outflow's 1
  energy and lands as 3 × `lit_cost`.

Going out still drops the powered projectors (`proj.strip`), and the pool
stays where it was.

**Feedback at the player.** The LCD names the effect for 2 s:
`BONUS LIFE`, `BONUS <projector name>` (SPLASH, FAST, LONG, STRONG or the
game's own, cut to 8 characters), `MALUS LIFE` or `MALUS DIM`. Each kind
has its own cue: `Bonus` for LIFE, `BonusProjector` for any projector,
`Malus` for MALUS LIFE (the ruleset's `Down` follows as the player goes
out), and `MalusDim` for DIM. The grant is quiet
(`proj.grant(vars, id, true)`), so `ProjectorChange` doesn't play on top.

**DIM** (`proj.set_dim(vars, on)`) halves the pool of every held projector
(current energy is clamped down to it; a shared pool is halved once). It doubles the recharge wait and
the ramp, and doubles the cooldown. A profile whose cooldown is 0
gets `DIM.min_cooldown_ms` instead, because doubling
nothing would not be "longer". The factors are constants at the top of
`projector.lua`.

**Going out loses what was picked up.** A ruleset calls `proj.strip(vars)`
in the rule that takes a player out (IN→OUT). It drops every projector a
BONUS can hand out, lifts DIM, and puts the baseline back in hand with its
own banked pool. Projectors declared `bonus = false` (a role such as VIRUS,
a practice TRIAL) are kept. It must run before a respawn writes the pool,
or the respawn would fill the powered projector instead. `reset()` lifts DIM too. In Virus a clean
player leaves play by infection, so DIM lifts there; a virus calls
`proj.strip` when it goes down, which keeps its VIRUS projector. Lifting
does not refill the pool; the ordinary recharge does.

A MALUS LIFE takes the player out with no player to credit, so the
"LIT by …" line reads `LIT by TOTEM`: `std.pickup_effect` calls the game's
`on_malus_life` hook, which sets the name.

The optics push now always carries an integer cooldown (0 when the profile
declares none), because a projector with no cooldown must undo a dimmed
one. That also means switching back to the baseline no longer keeps the
previous profile's cooldown.


---

## 11. A measurement belongs to the state it was fired in

Enlight keeps a completed result until `poll()` reads it, and only in-play
states read it (`proj.result`). So a beam still being measured when its
shooter was put out used to stay parked for the whole wait. On the first
tick back in play it went out as a LIT, aimed at whoever it hit seconds
earlier, wherever they are now. FestaSportSasso and TiroBersaglio had the
same leak across the welcome screen: a practice beam could land as a real
LIT once the turn began.

`GameRunner` now calls `Enlight::discardResult()` on every state change,
and at `begin()`. The pending result is then delivered as NO_HIT, and the
cooldown / re-arm sequence runs exactly as after a miss. The energy for
that beam stays spent, because the trigger really fired.

The consequence is deliberate: in near-simultaneous mutual fire, the
player who goes down first does not land their beam ("discard", not
"trade"). A trade would need every out-of-game state to read and send
results, game by game.

**Radio messages are not purged, and must not be.** Incoming requests are
already dispatched under the current state on the tick they arrive, and a
request with no handler in that state is dropped. There is no cross-state
queue to clean. Replies are kept on purpose: a SHONE reply that arrives
after the shooter went down is a point earned while in play.

---

## 12. What the shining side does on its own

None of this travels.  A receiver sees one LIT with a strength, whatever
the projector did to arrive at it.

### Fire modes

`mode = "auto"` (the default) fires while the trigger is held, `"semi"`
fires one beam per press, `"burst"` fires `burst` beams per press — the
burst finishes even if the trigger is let go.

### Charge

```lua
charge = { ms = 1500, max = 3 }
```

Hold to charge, release to fire **one** beam: its strength grows linearly
from 1× to `max`× over `ms`.  A game that declares `vars.charge` gets
0..100 to show.  The recharge wait starts at the beam, because the trigger
is already up.

### The lock ("tunneling")

```lua
lock = { grace_ms = 300, curve = "fib", max = 8, deliver = "release" }
```

While the trigger is held, every beam that lands on the **same** target is
one step.  Step *n* adds `curve(n)` standard units — `linear` 1,1,1…;
`square` 1,3,5…; `fib` 1,1,2,3,5…; a table lists them, its last value
repeating — times the profile's strength, and the total is capped at `max`.

- `deliver = "release"`: nothing is sent until the press ends, then one LIT
  weighing the total.  `"beam"` sends each step as it lands, weighing that
  step alone; it needs the `ignore_immunity` effect to get past a
  ruleset's immunity window.
- **Lost**, with what it had built: a beam lands on someone else, or no
  beam has landed for `grace_ms`.
- **Delivered**: the trigger is released, or the pool runs dry.
- Enlight shines in bursts, so "continuous" means consecutive bursts.  A
  grace shorter than one burst plus its cooldown would lose the lock
  between two beams that both landed, so it is never taken as less.
- Going out loses it (`proj.strip`), as does switching projector — the
  same "discard, not trade" as §11.
- `vars.lock`, when declared, shows the step reached.

**The target is warned.**  From step `warn_at` — default 1, the first beam
that lands — the projector broadcasts `MSG_LOCK` naming the target, and
again every second while the lock holds.  The target's device shows
`LOCKED` and plays the `Locked` cue (two urgent pips, a hard double buzz,
magenta) at each one, so it pulses for as long as the lock is on, and stops
by itself 1.5 s after the last: a released trigger, a broken lock and a
lost packet all end it the same way, with no "stop" message.  It never
names the shining player — a name says where to look, and `LOCKED` alone
already says "move".  The warning gives the target a second or more to
break line of sight, which makes `grace_ms` the main balance lever of a
locking projector.

`warn_at = 0` is a lock that does not warn: one that restores, or
protects, has nothing to warn about.  `warn_at = 3` warns from the third
step.

No game declares anything for this: see §16.

A fine game (§3) is where the curve has room: with 3 to 5 lives, `fib` is
out by the fifth step.

### The rest

| Field | Meaning |
|---|---|
| `spinup = { to_ms, beams }` | the wait between beams goes from `cooldown_ms` to `to_ms` over that many beams of one press, and back on release |
| `overheat_ms` | emptying the pool locks that projector for this long (doubled while dimmed) |
| `uses` | a pickup dropped after that many beams — once the last one has been measured |
| `lifetime_ms` | a pickup dropped that long after it was given; a re-grant restocks both |
| `on_lit_refund` | energy back for each beam that reaches a player, capped at the pool |
| `self_cost = { var, amount, floor }` | each beam also costs `amount` of any game var, and is refused if that would take it below `floor` (default 0); `amount = proj.units(1)` for a var counted in the game's units |

---

## 13. Effects: what a beam does at the receiver

Each effect is one bit of the LIT's 16-bit effects word (§3), so stacking
costs nothing on the wire.  A profile lists the ones it always carries, and
anything may switch one on or off at run time for whatever is in hand — a
power-up, a rule, a key:

```lua
{ id = 20, name = "FROST", effects = { "dim", "mute" } }

proj.set_effect(vars, "silent", true)          -- lost on going out
proj.set_effect(vars, "silent", true, true)    -- sticky: what the player IS
proj.set_effect(vars, "silent", false)
```

| Bit | Effect | At the receiver |
|---|---|---|
| 0 | `restore` | the strength is given, not taken: up to the starting lives (the pool in Outflow), from anyone, inside any immunity window |
| 1 | `dim` | dimmed for `ms` (5 s); a MALUS DIM outlasts it |
| 2 | `mute` | cannot shine for `ms` (3 s) |
| 3 | `expose` | the next beam within `ms` (5 s) weighs `mul` (2×), once |
| 4 | `siphon` | loses `amount` (5) energy — pair it with `on_lit_refund` for the shiner's gain |
| 5 | `beacon` | repeats `MSG_TRACE` every `every_ms` for `ms` (20 s); see below |
| 6 | `ignore_immunity` | the immunity window neither refuses the beam nor is opened by it |
| 7 | `silent` | the receiver is not told who shone: `proj.shiner(pkt)` returns `"?"` |
| 8 | `splash` | triggers the standard area (§5) — read by the firmware |
| 9–15 | | areas a game's own profiles declare, in id order |

**What an effect does is this table's, not the beam's.**  The wire carries
a bit; the duration or amount is looked up locally, and every device runs
the same file.  A power-up can therefore add an effect but not retune it —
a stronger variant is another entry.  A game retunes one for everybody:

```lua
proj.define{ effects = { dim = { ms = 8000 }, beacon = { to = "team" } } }
```

**Only receiver-side effects are here.**  A lock, a charge, a fire mode
(§12) are the shining side's and send nothing but a strength.

**Who applies them.**  `std.lit_target` does, with nothing to pass: it
finds the projector library on its own (§16; `proj = false` opts out).  A
ruleset with its own LIT handler reads the beam through the library, never
by offset: `proj.beam_has(pkt, name)`, `proj.kind(pkt)`, `proj.role(pkt)`,
`proj.shiner(pkt)`, `proj.weight(vars, pkt)` (the strength, more while
exposed) and `proj.landed(vars, pkt)` (apply the timed ones — call it when
the hit landed and the player stays in play).

**They pass only in play.**  `proj.tick` runs the clocks, and `proj.strip`
ends them all: going out is a clean slate, a trace included.

**`silent` hides less than it sounds.**  It removes the name from the
receiver's tray.  The shiner's own shine cue is still audible to anyone
nearby (it is the profile's `shine_action`, tunable separately) and the
end-of-game scores still credit the points.

### The beacon effect and `MSG_TRACE`

A traced player broadcasts `MSG_TRACE [shiner, shiner's team, to]`,
single-hop, at most **once a second** however a game retunes it — one
traced player is one more periodic sender on the channel.  `to` is who may
use it: `"shiner"`, `"team"` or `"all"`.  Nothing is shown to the traced
player.  The library keeps the last trace heard from each traced player
that is meant for this device (§16), and a ruleset asks:

```lua
local rssi, age_ms = proj.trace_of(id)        -- nil: never heard
local id, rssi     = proj.nearest_trace(3000) -- the strongest one, if recent
```

What a reading means (a distance bar, a count of seconds near someone) is
the ruleset's.  One that would rather take the message itself declares a
handler for `MSG.TRACE` and reads it with `proj.trace(pkt)`.


---

## 14. Gates: the reload and respawn mini-game

A wait the player can shorten by pressing the **secondary trigger** at the
right moment.  The engine is `std.gates` (in `games/lib/std.lua`, where the
rules are spelled out); the projector's recharge and the respawn wait are
its first two users, and anything with a bar to wait out can be a third.

```lua
-- a profile: gates on the recharge wait
gates = { gates = 2, window_ms = 250, lead_ms = 300, reward_pct = 50, penalty_pct = 25 }
```

**With no `gates` — the default — nothing changes**: the wait is the release
plus `recharge_delay_ms`, exactly as §9 describes.  With them, that delay
(stretched while dimmed) is the **nominal time**, and everything else is
derived from it.

- The wait is cut into as many equal segments as there are gates, one gate
  in each: a window of `window_ms` (never less than 150 ms) around the
  middle of its segment.
- **Each gate is moved at random**, by up to two windows either way, drawn
  afresh every time, so it can be neither learnt nor automated.
- **The marker shows only shortly before the gate**: `lead_ms` before the
  window opens, until it closes.  That is also when `GateOpen` plays, so a
  player watching the field gets it by ear and by hand.
- **Not pressing costs nothing.**  The wait lasts the nominal time.  The
  mini-game is an offer; a rule that punished looking away would make it
  an obligation, and would make DIM (which doubles the wait) brutal.

| Press | Result |
|---|---|
| inside the window | the gate is passed: `reward_pct` of its segment is skipped (50 = the rest of it, so one gate passed ends the wait at about half) |
| outside it, 1st miss | the wait grows by `penalty_pct` of the nominal time |
| 2nd miss | one more gate is added at the end, with its segment |
| 3rd miss | the whole wait starts over — **once** |
| later misses | as the first |

Each gate can be missed only once and a press with no gate left is
ignored, so the worst case is bounded.

**Room.**  A segment has to hold its jitter and the marker's lead (ten
windows, or four leads).  Gates that do not fit the nominal time are cut
down when the wait starts; a wait too short for even one is a plain wait.

### What the player gets

Four UI events, each with sound, light and vibration, usable by anything:
`GateOpen` (one sharp high tick and a firm buzz), `GatePassed` (two rising
notes, green), `GateMissed` (a low flat buzz, amber), `GateRestart` (three
falling notes, red).  With `label = "RELOAD"` (the projector's default) each
is also named on the tray: `RELOAD: NOW`, `OK`, `MISSED`, `AGAIN`.

**The bar's marker.**  Two brackets over the stretch of the bar the gate
covers, drawn by the display while the wait's owner says so.  There is
nothing to declare: every bar row with a `start_var` has a marker of its
own, and the owner names the wait by that var (`la.bar_mark(start_var,
value)`).  So a game that never heard of gates shows the marker the day a
totem hands out a gated projector.

**One marker per wait.**  A marker belongs to the bar that times that wait
and shows on no other.  Should two gated waits ever run at once, each has
its own gate, its own marker and its own bar, and **one press goes to one
of them**: the wait whose window is open takes it; with none open it is a
miss for the wait that began first.  The player can speed up only one of
them at a time.

A passed gate moves the wait's start back and a missed one moves it
forward, so the bar jumps with the wait and always shows what is left.
The energy bar only shows while the pool is **empty** (§6), so a partial
reload has the gates and their cues, but no bar to see them on.

### The respawn wait

```lua
local respawn = std.respawn_gates{ gates = 2 }                          -- at load
respawn.start(vars, vars.respawn_secs)                                  -- going out
update = { [S.OUT_GAME] = function(vars) respawn.tick(vars) end },
rules  = { { from = S.OUT_GAME, to = S.IN_GAME,
             when = function() return respawn.done() end, ... } },
```

It writes the vars `std.respawn_wait` writes, and with `gates = 0` it is
`std.respawn_wait` and nothing else.  A game that also shows the wait as a
number (`countdown_in`) will see the two disagree once a gate is passed:
the bar is the truth.


---

## 15. Player classes

`std.classes` (in `games/lib/std.lua`, where every field is listed) makes a
class out of a few numbers:

```lua
local classes = std.classes{ proj = proj, var = "class_name", list = {
  { name = "SCOUT", lives_mul = 0.5, cooldown_mul = 0.5, loadout = { FAST } },
  { name = "TANK",  lives_mul = 2, respawn_mul = 1.5, recharge_mul = 1.5,
    can_pickup = false, loadout = { STRONG },
    kinds = { [KIND_HEAT] = "immune", [KIND_COLD] = { minus = 1 } },
    spare_effects = { "dim" } },
  { name = "MEDIC", loadout = { P_HEAL }, targets = "team",
    aura = { id = 60, bands = { { -60, 1 } }, to = "team",
             effects = { "restore" }, every_ms = 3000 } },
} }
```

| What a class sets | Fields |
|---|---|
| how much it takes | `lives_mul` or `lives`, `respawn_mul` |
| its projectors | `energy_mul`, `recharge_mul`, `cooldown_mul`, `strength_mul` (`proj.set_mods`), `loadout`, `can_pickup` |
| what its beams say | `role_tag` (default: its place in the list), `kind`, `effects` |
| how beams land on it | `kinds = { [mask] = reaction }`, `roles = { [role_tag] = reaction }`, `spare_effects` |
| whom it shines | `targets = "others" | "team" | "all"` |
| what it radiates | `aura` |

- **A reaction** is `"immune"`, `{ minus = n }` (n standard units lighter,
  never below nothing) or `{ mul = x }`.  A kind is a mask, so one beam can
  match several: immune wins, then every `mul`, then every `minus`.  With
  1 to 5 lives only `"immune"` and a `minus` are worth using; fractions
  need a fine game (§3).
- **Resistances are per kind, not per projector**, because no projector id
  travels (§3).  With 8 kind bits, that is a real limit on how finely a
  class can discriminate — and it keeps the table readable.
- **A medic needs no mechanism of its own**: it is a class whose loadout is
  a projector carrying `restore`, with `targets = "team"`.  The ruleset
  asks `classes.targets()` when it decides whether to send a LIT, since
  only it knows who is an opponent.
- **A class is what the player is**: its role tag, kind, effects and
  multipliers survive going out.  Its loadout does not if a BONUS could
  hand it out (`proj.strip`), so the ruleset calls `classes.equip(vars)`
  when the player comes back.
- **The aura** rides the area service: a policy declared at load, emitted
  from `classes.tick(vars)` at most once a second.  `to = "team"` with
  `restore` is a buff — a restoring area reaches the originator's team
  only — and `to = "others"` spares teammates.  `weightless = true` lands
  the effects and takes nothing.  Each aura is one more periodic sender on
  the channel.

**Choosing.**  Each player picks at the start — a state whose update calls
`classes.pick(vars)`: `<` and `>` step through the list, the trigger
confirms — or the game assigns (`classes.assign(vars, name)`).

**The receiving side** is `std.lit_target{ classes = classes }`.
A kind or a role the class is immune to answers `reply.immune`.

**Not built.**  A limit of one class per team needs the devices to agree on
who took what — a protocol, not a parameter.  The engineer's own totem is
on hold: a totem is a separate device running a TotemVM program, and
whether one can be bound to an owner has not been looked into.


---

## 16. A ruleset transparent to its projectors

A ruleset is about the game: what happens when players meet, how states
change, who scores.  A projector is about power and effects.  The less a
ruleset has to know about the projectors that may turn up, the more of
them can be handed out on the field without touching a game file — so a
projector behaviour must not need a line in every ruleset.

### What needs nothing declared

| Behaviour | How it works without the ruleset |
|---|---|
| splash (§5) | the firmware's area service; bystanders' hits go to the ordinary LIT handler |
| effects at the receiver (§13) | `std.lit_target` finds the projector library itself |
| the lock warning (§12), traces (§13) | the library inbox, below |
| the gate marker (§14) | every owned bar has a marker; the owner names the wait |
| switching, timers, the trace being sent | the projector's own tick |

### The library inbox

`MSG_LOCK` and `MSG_TRACE` belong to the projector library, not to a
ruleset.  They live in a block of message types of their own (0x20–0x2F),
and the runner treats that block specially: a message of it that **no
handler of the ruleset took** in the current state is kept in a small
inbox (`src/game/LightAir_LibInbox.h`), and the library reads it on its
tick with `la.inbox()`.

- **A handler the ruleset declares wins.**  A ruleset that wants a library
  message for itself lists it in `on_message`; it then never reaches the
  inbox in that state.
- **In play only, for free.**  The library reads the inbox from
  `proj.tick`, which a ruleset calls in its in-play states and nowhere
  else.  A player who is out is not warned, with nothing declared.
- **Never answered.**  Library messages are single-hop broadcasts; a
  message for one player names them in its payload and the others ignore
  it.  Nothing waits for a reply and no slot is held.
- **A new library message needs no firmware change** beyond its name in
  `RadioMsg` and `la.msg`.

### What it costs — the downsides, deliberately accepted

- **It rests on a convention nobody enforces**: "in play" means "the
  ruleset ticks the projector".  A state that plays but skips the tick
  gets no warning and runs no effect clock.  Virus skips it for the 5 s of
  a returned virus's grace.
- **The inbox is small and lossy**: 8 messages, nothing older than 2 s,
  emptied on every state change.  Right for a warning repeated every
  second; wrong for anything that must arrive exactly once.
- **The game file no longer tells the whole story.**  Nothing in a ruleset
  says why `LOCKED` appears.  This section and the test suite carry that.
- **Every device carries every behaviour**, used or not: the RAM in
  `docs/parked-features.md` §3 is the price of the freedom on the field.

### What stays explicit, on purpose

Sending a LIT (the ruleset decides who may be shone: friendly fire is a
rule), `proj.strip` in the rule that takes a player out (only the ruleset
knows which transition that is), classes, a gated respawn, and what a
BONUS totem hands out.

### A ruleset may still shape its projectors

Transparency is the default, not a cage: a ruleset uses projectors as
tools when a game mode calls for it.

```lua
proj.define{ only   = { proj.standard.FAST.id, P_MINE } }   -- these exist, nothing else
proj.define{ except = { proj.standard.STRONG.id } }         -- everything but this one
```

A projector that does not exist in a ruleset is not known to it at all:
never given, and **never offered by a BONUS totem's menu**, because
`proj.bonus_options()` lists only what exists.  So what the DM can assign
to a power-up totem follows from the game's own declaration.  The
baseline always exists; a ruleset replaces it by declaring profile id 0,
forces one into hand with `proj.grant`, switches effects and kinds on and
off (§13), and turns the switch keys off with `switch_keys = false`.
