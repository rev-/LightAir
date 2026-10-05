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

A player who absorbs a SPLASH beam becomes the centre of an area: their
device broadcasts a beacon, and bystanders take graded damage from its RSSI.
The bands come from the projector's area policy, the credit goes to the
**shooter**, and the victim only relays.

RSSI is the right tool here and the wrong one in §4, for three reasons: a
splash radius is inherently fuzzy, graded bands degrade by one step rather
than between hit and nothing, and there is no optical measurement to a
bystander who was never aimed at — so RSSI is not a worse choice than
something better, it is the only choice.

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
},
```

and `define()` registers it with `la.area_policy` under the projector's own
id.  Every device runs the same file, so every device holds the same
policies; no game file mentions splash.  From there the firmware does it:

- **Trigger (the victim).** A LIT from that projector which the ruleset
  answered TAKEN or SHONE (`la.hit`) makes this player the centre: the
  runner broadcasts `MSG_AREA [policy, shooter, shooter's team]`,
  single-hop.  `on = "shone"` triggers on the knock-out alone.  A refused
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

**Virus.**  An area hit carries its policy's `role_tag` (SPLASH: 0), so in
Virus it is a clean hit: nothing to a clean player, and down for a virus —
so a clean SPLASH puts down the viruses standing near the one it hits.

Guards, each with a test that fails when it is removed:

| Guard | Why |
|---|---|
| an area hit never triggers a beacon, and `la.area_emit` refuses inside one | otherwise one beam cascades across the field |
| the area flag is local: a LIT from the air has it cleared | a peer could otherwise skip every immunity window |
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

Ids are **fixed and reserved**, because a projector id travels on the wire:
a LIT names the projector that fired, an area beacon names its policy by
the same id, and every receiver looks either up by that id locally. A game's own profiles start above this range.

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
