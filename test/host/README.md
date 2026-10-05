# Host test suite

PC-side tests for the Lua game engine and the TotemVM — no ESP32
toolchain or hardware required.  `stubs/` contains minimal stand-ins for
the Arduino/ESP-IDF headers; everything else under test is the **real**
firmware source compiled for the host.

```
make -C test/host        # build + run all six suites
```

| Target | What it proves |
|---|---|
| `games` | every `games/*.lua` and `games/custom/*.lua` loads against a stubbed `la` kernel; the projector library declares a profile's area (SPLASH's) as an `la.area_policy` under its own id and nothing else does; `std.lit_target` takes an area hit (`pkt.area`) through the game's own rule without checking or opening an immunity window, keeps friendly fire for it, reports its knock-out as SHONE, and leaves an own area hit to the policy's `self`; the projector's shared pool (`shared_pool`, Outflow) is the baseline's — a pickup or a switch moves no energy, a powered projector's own recharge never runs on it, DIM halves it once, each beam costs the projector in hand's cost — and Outflow hands out the catalogue on it; `std.totem_touch` sends a single-hop `[gate, ACK]` at most once per period; Virus: a clean beam on a clean player is friendly fire, any beam (a viral one too) or a clean SPLASH's area puts a virus down for `virus_respawn_secs`, it touches once a second only after the wait, a totem's answer (and no stray one) brings it back refilled with 5 s of grace against every beam, and with no totems it is back on time; points: infection +5, virus downing a virus +1, clean downing a virus +2, the last clean player +10 (not on a time-out), winners by points then time clean; every game that takes hits weighs them by strength (Outflow in `lit_cost` units) and lets an area hit past its immunity window — none re-implements the hit ladder; no game reads the firmware's A+B chord; all handlers, rules and totem tables execute; every TotemVM program encodes within the single-packet budget (`totemvm.lua` is the reference encoder — the executable spec of the wire format) |
| `totemvm` | the real `LightAir_TotemVM` interpreter, fed reference-encoder programs, reproduces the five standard roles' behaviour (beacons, animations, ownership windows, scoring, cooldowns), keeps a stored RSSI signed in its `int16_t` registers, and rejects malformed programs |
| `radio` | `LightAir_Radio`'s reply bookkeeping: every reply to a broadcast reaches the sender (a totem beacon is answered by everyone in range, so closing on the first answer loses the rest), a unicast's slot still closes on its one answer, and timeouts fire only where a missing answer means something |
| `strip` | the totem LED strip's one-shot queue: animations triggered together play in arrival order instead of replacing one another (two players respawning at one base), the queue is bounded, and a late arrival waits its turn |
| `ledwave` | the optics LED buffer (`EnlightLedWave`): the one DMA buffer holds a whole cycle of exactly the requested power — every period, the last included — a power switch rewrites it once and a repeated request writes nothing, returning to full power restores the first fill byte for byte, and the low-power period lights the LEDs `LOW_POWER_FACTOR` as long as the full one |
| `totemdriver` | what a totem answers outside its program — the touch (`MSG_TOTEM_TOUCH`): an idle totem ignores it; an active one answers an acknowledge read at or above its gate (gate 0: any distance) with `[ACK, role]` and the arrival chaser in the toucher's colour, plays no chaser over another one-shot and at most one per tick, ignores reserved actions and short packets, and never lets a touch reach its program (a probe program would broadcast if it did); after the roster it is idle again |
| `luagame` | the real `LightAir_LuaGame` binding (with the vendored Lua 5.5 core) loads every game file (the flashed ones and `games/custom/`), and a scripted Free-for-All session runs begin / messages / replies / rules / update ticks through the synthesized `LightAir_Game` descriptor exactly as `GameRunner` drives it on-device; a Teams section then proves `pkt.rssi` reaches the Lua handlers (what makes a proximity gate a gate at all) and that a beacon outside the gate draws no reply; a keypad section proves the input verbs reach the whole report — key by name, state ladder, release edge, enumeration of keys the ruleset never named — and a FestaSportSasso section drives one whole turn of the endless ruleset: welcome screen, BASE start on a full clock, shone, clock out, stats screen, admin `<`+`>` restart (and A+B, the firmware's menu chord, leaves it alone); an in-game hold section drives A+B through the real runner with freeforall: a held player still takes and answers a LIT, leaves a BONUS beacon unanswered, cannot read a pending beam, keeps its clock running, and on END GAME exchanges scores at once while the end screen and its cue wait for the tool — then A held alone restarts the end screen and A+B does not; a tools-menu section and a `hold = { accept, on_enter, on_exit }` fixture cover the rest (an area hit is a LIT to `hold.accept`, a totem-centred one included); an area section drives the firmware's area service through the real binding, radio and runner with teams — a landed SPLASH hit broadcasts a single-hop beacon crediting the shooter, at most one per 250 ms, none for a refused hit or a projector without an area; a bystander takes the band's strength through teams' own LIT rule (nothing out of reach, from its own area, from a teammate, from an unknown policy), with no reply, no second beacon and no immunity window either way; an area knock-out sends a unicast credit that the shooter's runner acknowledges and scores as its own SHONE; a LIT from the air claiming the area flag is a direct hit; a Virus section puts patient zero down with a clean LIT, sees no touch during the down time, then a single-hop `[55, ACK]` touch, and a totem's 0xF5 answer bring it back — then an `area` fixture covers `la.area_emit` (a real hit emits, an area hit is refused), `friendly = "never"`, `self = true`, `credit = false`, a hold that takes no LIT, a credit's acknowledgement and timeout never reaching `on_reply`, a declaration after the load refused, and each malformed policy refusing the load |

Requires `g++` and `lua5.4` (`apt install lua5.4`).  The `games` suite
runs the pure-Lua game files under the system lua5.4 interpreter; only
`luagame` embeds the real vendored 5.5 core.

## Reading a failure

- **games** stops at the first failed `assert` with a Lua traceback;
  the `file:line` points either at `test_games.lua` (the expectation
  that broke) or into the game file that misbehaved.
- **totemvm** / **radio** / **strip** / **ledwave** / **luagame** print one `FAIL: <what> (line N)`
  per failed `CHECK` — the line number is in the corresponding test
  `.cpp` — and exit non-zero at the end of the run.
- The loud `[E] LuaGame[Faulty] fault … stack traceback …` blocks in
  the `luagame` output are **expected**: the fault-policy tests inject
  Lua errors on purpose (see fixtures below).  The verdict is the
  final `… TESTS PASS` / `ALL HOST TESTS PASS` line, not the noise
  along the way.

## Fixtures (`fixtures/`)

| File | Failure mode it exercises |
|---|---|
| `faulty.lua` | runtime faults mid-match: `update` errors every tick, and the lit handler errors *after* a partial mutation — faults must be counted per call-site, pre-error effects must stand, and the match must continue |
| `libdebug_lib.lua` / `libdebug_game.lua` | an error inside a stripped library names the library but no line; an error in the game file keeps its line and local name |
| `faulty_begin.lua` | a failing `on_begin` — the one fatal fault: the game must refuse to play (forced straight into `scoring_state`) |

`totemvm.lua` is the reference encoder (the executable spec of the
TotemVM wire format); `gen_programs.lua` uses it to emit the binary
programs the `totemvm` suite feeds to the real interpreter.
