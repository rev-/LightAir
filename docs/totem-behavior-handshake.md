# TotemVM — totem behaviour configured over the handshake

Totems cannot be assumed to hold any `.lua` files: games are shared
projector-to-projector while the totems may be off, out of range, or brand
new.  A totem also cannot afford a heavyweight download when it wakes up
*mid-game* — handshakes do not only happen during the pre-game countdown,
and the channel is busy with game traffic.

The design therefore puts a **general, fixed state-machine interpreter in
the totem firmware** ("TotemVM") and lets each Lua ruleset send a
**configuration of it — a few dozen bytes — inside the single 0xF1
activation reply** that the handshake already sends today.  Activating a
totem costs exactly one packet, warm or cold, for standard and custom roles
alike.

Measured encoded sizes of the five standard roles (reference encoder):

| Role | bytes | | Role | bytes |
|---|---|---|---|---|
| BASE (any team) | 41 | | FLAG | 91 |
| BONUS / MALUS | 53 | | CP (the worst case) | 213 |

Budget: 225 bytes of program in the 237-byte 0xF1 payload.  Programs are
data, so the projector validates (and can even simulate) them at
game-load time — a malformed role fails on the bench, not in the field.

Design doctrine that keeps the VM small and future-proof: **totems are dumb
on purpose.**  They beacon, they referee presence, they render; decisions
live player-side, where the Lua files are.  The existing roles already work
this way (the flag totem is driven entirely by player broadcasts and
inherits the players' RSSI gates).

---

## 1. The machine

Fixed resources (RAM-cheap headroom; limits cost nothing on the wire):

| Resource | Count | Purpose |
|---|---|---|
| States | 8 | current mode; state 1 is initial |
| Registers R0..R7 | 8 | bytes (owner slot, counters, …) |
| Accumulator ACC | 1 × u16 | presence/voting bits, with two derived reads: `ACC.CLASS ∈ {EMPTY, SINGLE, MANY}` and `ACC.LOW` (lowest set bit index) |
| Timers T0..T3 | 4 | ms clocks restarted by `start` |
| Message context | — | `SENDER`, `SENDER_TEAM`, `PAYLOAD[i]` of the event being handled |

Each state holds an **ordered rule list**; a rule is *trigger + guards +
actions*:

- **Triggers**: `enter` (state entered, incl. activation for state 1),
  `every ms` (drift-free periodic; equal periods in one state share phase),
  `msg msgType` (request received), `reply msgType` (reply to our own
  broadcast received).
- **Guards** (all must hold): payload byte vs const/register, payload
  length, register vs const/register, `ACC.CLASS`, `ACC.LOW` compare,
  timer elapsed compare, RSSI compare.  Comparators: `== ~= < >= <= >`.
- **Actions**: `goto state` · `set Rn ← value` · `accbit value` (ACC |=
  bit(value−1)) · `accclr` · `start Tn` · `bcast msg, byte template…` ·
  `reply subtype` · `anim event, colorsource, rhythm?`.

**Semantics** (the decisions adopted from review):

1. Rules are evaluated **in order**; a firing rule **consumes** the event
   unless it carries the `cont` flag — that is what lets several
   `every 2000` rules decompose a CP window evaluation, with a final
   epilogue rule.
2. `goto` is **immediate**: the new state's `enter` rules run right away,
   then the remaining actions of the current rule continue (so "restore
   idle background, then flash a one-shot over it" is expressible);
   remaining rules of the old state do not run for this event.
3. **Auto-reply convention kept**: a request matched by a `msg` rule is
   auto-replied (sub-type 0 unless a `reply n` action overrides), so
   player-side `on_reply` logic keeps today's contract.  The mirror of it
   matters for a `reply` rule.  A totem's beacon is a broadcast, and two
   things now make its answers trustworthy: players answer only when their
   ruleset acted on the beacon (a handler that returns nothing sends
   nothing — see `docs/lua-games-design.md`, "Answering is the signal"), and
   `LightAir_Radio` keeps a broadcast's reply window open for its whole
   timeout instead of closing it on the first reply, so a role that wants
   *every* answer — CP counting who is standing on it — gets them all.
4. **RSSI is readable, not policy**: the guard and the value operand both
   exist so a future proximity-reactive role (trap totems, "serve the
   closest") needs no firmware change, but no standard role uses either —
   proximity gating stays player-side by doctrine.
5. **One-shot animations queue** rather than replace one another: two
   players respawning at one base in the same cycle each get their own run
   of the strip, and a rule that fires two `anim` actions shows both.  Depth
   is `LightAir_LEDStrip::MAX_ONESHOTS`; overflow is dropped, because a base
   still animating for a player who has left is worse than a missed frame.
6. **Animation vocabulary is firmware-fixed**: `anim` references the
   existing `TotemUIEvent` set by id, with a color *source* (constant RGB,
   team-of-value, player-of-sender, team-of-sender, or raw args) and an
   optional team rhythm.  New visuals need firmware (they are hardware
   anyway); new behaviours never do.  Two renderer additions: the `Control`
   effect gains a slot-based arg form (`0xFE, slot`, for team CP games) and
   a second one (`0xFD, slot`, for teamless CP games — see
   `std.totems.cp()`'s `opts.teamless`) that both do the team-vs-player
   color mapping internally, so the VM needs no arithmetic; and
   `ControlScore`, a one-shot sparkle burst using the same arg forms, for
   "this CP just paid a point" distinct from `Control`'s steady hold fill.

Value operands anywhere a value is accepted: literal, `R n`,
`PAYLOAD[i]` (1-based, like `pkt:byte(i)` in game files), `ACC.LOW`,
`SENDER`, `SENDER_TEAM`, `RSSI` — the receive-side signal strength of the
packet in hand, in dBm — and `CFG`, the role's config seconds, resolved by
the projector at serialization time from the game's `totem_slots`
`config_var` (or the program's `cfg_default`).

Registers are **signed and 16-bit** (`int16_t`).  They have to hold both the
0xFF "neutral owner" sentinel the CP role uses and a negative RSSI reading,
which eight bits cannot represent distinctly either way round.  Note the two
forms of RSSI: `{"rssi"}` is the *value* — storable in a register, so two
readings can be compared against each other — while `{"rssi", op, dbm}` is
the *guard*, which only tests it against a literal.  Proximity gating stays
player-side by doctrine; both forms exist for proximity-reactive roles.

---

## 2. Authoring — programs are Lua data

The `totems` section of a game file no longer contains functions: each role
is a **pure data table** describing the machine.  The projector never
executes totem code — it validates the table and serializes it.

```lua
totems = {
  BONUS = { vm = 1, cfg_default = 30, states = {
    { -- state 1: READY
      { enter = true,  run = { {"anim", "BonusIdle", {"rgb", 0, 180, 0}} } },
      { every = 2000,  run = { {"bcast", MSG.BONUS_BEACON, 0} } },
      { reply = MSG.BONUS_BEACON,
        run = { {"start", 0}, {"anim", "Bonus"}, {"goto", 2} } },
    },
    { -- state 2: COOLDOWN
      { every = 250, when = { {"elapsed", 0, ">=", {"cfg"}} },
        run = { {"goto", 1} } },
    },
  } },
}
```

`games/lib/std.lua` provides factories that build these tables —
`std.totems.base(team)`, `.bonus()`, `.malus()`, `.flag(team)`, `.cp()` —
so most games write one-liners; `games/freeforall.lua` spells the tables
out in full as the tutorial.  The CP program in `std.lua` is the acid test:
the hardest existing role is nine rules / 199 bytes (26 of the 225-byte
budget free), using ordered `cont` rules over one 2 s window.

CP owners are conquered, not merely occupied: the one payload byte a
presence reply carries is one of two disjoint kinds, decided entirely
player-side (RSSI stays player-side by doctrine, §0 point 4) —
- **"conquest"** (sub-type 1..16, the replying player's own slot): I am
  within the *tight* ring, trying to take this hill.  Feeds `ACC` via
  `accbit` exactly as any other role's presence collection.  Exactly one
  distinct conquest sender each window takes (or keeps) the hill,
  regardless of who else is holding it — an owner merely holding never
  blocks a lone challenger, who has to physically reach the tight ring.
  Two or more distinct conquest senders contest it instead: no owner
  change, no point paid.
- **"hold"** (the reserved sub-type 17, never 0 — see the note below):
  I *am* the recorded owner, within the looser hold ring, at any
  distance inside it — standing right at the totem does not upgrade
  this to a conquest reply.  Never touches `ACC`; it only proves the
  owner is still around, via a dedicated register, so an uncontested
  owner keeps scoring without needing to win a tight-range contest
  every period.  Not validated against the sender's identity or team:
  a stray hold from a just-deposed owner can't corrupt a capture
  (capture only ever reads `ACC`) and self-corrects within one
  broadcast cycle.

17 rather than 0: `RadioOutput::reply()` (`src/game/LightAir_RadioOutput.h`)
treats `subType == 0` as "send no payload at all", which the totem would
then read back as -1 (`V_PAYLOAD`'s own bounds check) — 0 cannot
silently carry a real value on this wire, so the reserved marker has to
be non-zero.

Collection needs no length or range guard on either rule: `accbit`'s own
runtime already rejects anything outside 1..16
(`LightAir_TotemVM.cpp`), and a missing payload byte reads as -1 from
the VM's own bounds check, which fails every comparison on its own — a
guard here would only repeat a check the action, or the next rule's
guard, already makes.

Release — an owned CP that gets a fully empty window (no conquest, no
hold) — releases to neutral rather than going on broadcasting an owner
who is not there.  It is **not** gated on the contest flag: with no
dedicated "settle" rule, nothing else is guaranteed to clear a leftover
contest flag when a hill goes fully silent (no conqueror to trigger
capture, no holder to trigger the hold-sustain tick), so gating release
on it would leave a CP that was contested and then abandoned stuck
forever.  Release's own action still falls through into the "empty and
unowned" rule within the same tick (clearing the contest flag as a side
effect, same trick as before), so a genuinely abandoned hill needs no
grace window — there is nobody left to show one to.  A CP that stays
*held* through a contest is unaffected: release is separately gated on
"no hold reply this window", so it never fires while the owner keeps
holding, contest or not; the leftover contest flag then clears itself
whenever the hold-sustain tick next comes due, at most one scoring
period later.

Rule shape (authoring):

```lua
{ enter = true | every = ms | msg = type | reply = type,   -- exactly one
  when = { guard, ... },      -- optional
  run  = { action, ... },     -- at least one
  cont = true }               -- optional
```

---

## 3. Wire format

### Extended 0xF1 activation reply

```
payload: [0]   roleId          (as today)
         [1]   sessionToken    (as today)
         [2:3] gameTimeLeft    (as today; watchdog budget)
         [4]   vmVersion       (1)
         [5:6] progLen (u16le)
         [7..] program bytes   (≤ 225)
```

This is the only activation form: the pre-VM firmware's short 4–5 byte
reply is retired together with the native totem runners.

### Program encoding (normative, v1)

All multi-byte integers little-endian.  Times on the wire are u16
**deciseconds** (100 ms granularity, up to 109 min).

```
program := [vm=1][nStates] state*
state   := [nRules] rule*
rule    := [trig][trigOperand][flags][nWhen] guard* [nRun] action*
  trig: 0 enter (no operand) | 1 every (+u16 ds) | 2 msg (+u8) | 3 reply (+u8)
  flags: bit0 = cont

value   := [0][u8] | [1][u16] | [2][reg] | [3][payloadIdx]
         | [4]=ACC.LOW | [5]=SENDER | [6]=SENDER_TEAM
  ({"cfg"} is resolved by the serializer into a [1][u16 ds] immediate)
cmp     := 0 == | 1 ~= | 2 < | 3 >= | 4 <= | 5 >

guard   := [1][cmp][idx][value]      payload byte compare
         | [2][cmp][n]               payload length
         | [3][cmp][reg][value]      register compare
         | [4][class]                ACC.CLASS (0 empty, 1 single, 2 many)
         | [5][cmp][value]           ACC.LOW compare
         | [6][cmp][timer][value]    elapsed (value in ds)
         | [7][cmp][i8]              RSSI dBm

action  := [1][state]                goto
         | [2][reg][value]           set
         | [3][value]                accbit (ACC |= 1 << (value-1))
         | [4]                       accclr
         | [5][timer]                start
         | [6][msg][n][value×n]      bcast with payload template
         | [7][sub]                  reply sub-type override
         | [8][animId][color][rhythm] anim
  color := [0] none | [1][r][g][b] | [2][value] team | [3] sender-player
         | [4] sender-team | [5][n][value×n] raw args
  rhythm:= team byte, 0xFF = none/default
```

A reference encoder in Lua (used by the test harness to validate every
game's programs and assert the ≤225-byte budget) doubles as the executable
specification for the C++ serializer and interpreter.

### Versioning

The idle 0xF0 beacon — currently informationless — advertises
`[fw api, vmVersion]`.  The setup menu's totem-assignment screen (S4c)
compares that against what the selected game's programs need; an
incompatibility surfaces as a menu message during setup, never as a dead
totem mid-field.  The VM only ever grows additively (new guard/action
opcodes ⇒ version bump).

---

## 4. Failure containment

| Failure | Behaviour |
|---|---|
| Malformed table in a game file | rejected by the projector at game load, with a message |
| Program too big / bad opcode on the wire | totem's defensive decoder rejects → stays IDLE, fault animation |
| Totem vmVersion older than the game needs | caught in S4c during setup |
| Lost 0xF1 | totem keeps beaconing; host replies to every beacon, as today |
| Host restarts mid-game | re-activation is one packet; program is stateless to re-send |
| End of game | `MSG_TOTEM_ROSTER` / watchdog revert to IDLE, unchanged |

Runtime cost on the totem: interpreting a handful of rules per tick is
microseconds; no Lua state is created on totems at all for VM roles, which
also frees the totem path's RAM.

---

## 5. When a future role doesn't fit the VM

In order of preference:

1. **Move the logic player-side** — usually the right answer (doctrine §0):
   let players broadcast decisions; the totem just renders and beacons.
2. **Add a primitive** — a new guard/action opcode is a small, additive
   firmware change; the beacon-advertised vmVersion keeps mixed fleets
   honest.
3. **Code transfer as a last-resort extension** — a v2 handshake form could
   carry a compact Lua chunk for genuinely algorithmic roles (the option
   explored and shelved during review: multi-packet transfer conflicts with
   mid-game wake-ups).  Nothing in the v1 wire format precludes adding it
   as a distinct payload form later.

---

## 6. C++ implementation (done, host-tested)

| Piece | Contents |
|---|---|
| `src/totem/LightAir_TotemVM.{h,cpp}` | decoder + interpreter; implements `LightAir_TotemRunner`; validated on host against reference-encoder programs for all five roles plus malformed-program rejection |
| `src/lua/LightAir_TotemEncoder.cpp` (serializer) | walks a role's data table → program bytes; validates limits at load; `{"cfg"}` sites recorded and patched with the live config value when the program is fetched at reply time |
| `LightAir_GameRunner::replyToTotemBeacon` | sends the 0xF1 reply `[role][session][timeLeft][vmVersion][progLen][program]` when `game->totemProgram` provides a program for the role; sends **no reply** otherwise (there is no short form — a role without a program leaves the totem IDLE) |
| `LightAir_TotemDriver` | accepts VM-form 0xF1 only (native runners and the role manager are deleted); routes packets to the VM RSSI-aware (`onPacket`) |
| `LightAir_TotemUICtrl` | `Control`/`ControlScore` effects: slot-based arg forms (`0xFE, slot` for team CP games; `0xFD, slot` for teamless ones, since 0xFE's slots 0/1 collide with player ids 1/2 there) |
| 0xF0 beacon | carries `[fw api, vmVersion]`; the S4c menu compatibility check is still TODO |
