# Host test suite

PC-side tests for the Lua game engine and the TotemVM — no ESP32
toolchain or hardware required.  `stubs/` contains minimal stand-ins for
the Arduino/ESP-IDF headers; everything else under test is the **real**
firmware source compiled for the host.

```
make -C test/host        # build + run all three suites
```

| Target | What it proves |
|---|---|
| `games` | every `games/*.lua` loads against a stubbed `la` kernel; all handlers, rules and totem tables execute; every TotemVM program encodes within the single-packet budget (`totemvm.lua` is the reference encoder — the executable spec of the wire format) |
| `totemvm` | the real `LightAir_TotemVM` interpreter, fed reference-encoder programs, reproduces the five standard roles' behaviour (beacons, animations, ownership windows, scoring, cooldowns) and rejects malformed programs |
| `luagame` | the real `LightAir_LuaGame` binding (with the vendored Lua 5.5 core) loads all seven game files, and a scripted Free-for-All session runs begin / messages / replies / rules / update ticks through the synthesized `LightAir_Game` descriptor exactly as `GameRunner` drives it on-device |

Requires `g++` and `lua5.4`.
