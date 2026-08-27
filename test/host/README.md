# Host test suite

PC-side tests for the Lua game engine and the TotemVM — no ESP32
toolchain or hardware required.  `stubs/` contains minimal stand-ins for
the Arduino/ESP-IDF headers; everything else under test is the **real**
firmware source compiled for the host.

```
make -C test/host        # build + run all five suites
```

| Target | What it proves |
|---|---|
| `games` | every `games/*.lua` loads against a stubbed `la` kernel; all handlers, rules and totem tables execute; every TotemVM program encodes within the single-packet budget (`totemvm.lua` is the reference encoder — the executable spec of the wire format) |
| `totemvm` | the real `LightAir_TotemVM` interpreter, fed reference-encoder programs, reproduces the five standard roles' behaviour (beacons, animations, ownership windows, scoring, cooldowns), keeps a stored RSSI signed in its `int16_t` registers, and rejects malformed programs |
| `radio` | `LightAir_Radio`'s reply bookkeeping: every reply to a broadcast reaches the sender (a totem beacon is answered by everyone in range, so closing on the first answer loses the rest), a unicast's slot still closes on its one answer, and timeouts fire only where a missing answer means something |
| `strip` | the totem LED strip's one-shot queue: animations triggered together play in arrival order instead of replacing one another (two players respawning at one base), the queue is bounded, and a late arrival waits its turn |
| `luagame` | the real `LightAir_LuaGame` binding (with the vendored Lua 5.5 core) loads all eight game files, and a scripted Free-for-All session runs begin / messages / replies / rules / update ticks through the synthesized `LightAir_Game` descriptor exactly as `GameRunner` drives it on-device; a Teams section then proves `pkt.rssi` reaches the Lua handlers (what makes a proximity gate a gate at all) and that a beacon outside the gate draws no reply; a FestaSportSasso section drives one whole turn of the endless ruleset — BASE hand-over, shone, clock out, stats screen, admin chord restart |

Requires `g++` and `lua5.4` (`apt install lua5.4`).  The `games` suite
runs the pure-Lua game files under the system lua5.4 interpreter; only
`luagame` embeds the real vendored 5.5 core.

## Reading a failure

- **games** stops at the first failed `assert` with a Lua traceback;
  the `file:line` points either at `test_games.lua` (the expectation
  that broke) or into the game file that misbehaved.
- **totemvm** / **radio** / **strip** / **luagame** print one `FAIL: <what> (line N)`
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
| `faulty_begin.lua` | a failing `on_begin` — the one fatal fault: the game must refuse to play (forced straight into `scoring_state`) |

`totemvm.lua` is the reference encoder (the executable spec of the
TotemVM wire format); `gen_programs.lua` uses it to emit the binary
programs the `totemvm` suite feeds to the real interpreter.
