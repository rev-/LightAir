# LightAir [![C/C++ CI](https://github.com/rev-/LightAir/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/rev-/LightAir/actions/workflows/c-cpp.yml)
[LightAir](https://www.light-air.it/) is a Laser Tag game designed to be open source and DIY-able, meaning feasible with a consumer type 3d printer (ref. Prusa MK3S).
It features an innovative hardware design using passive retroreflecting targets instead of active jackets, meaning it is less cumbersome to prepare and play with respect to the traditional active projector-active receiver setup.
It works even in full outdoor environments, with about 40m range. Features a very precise and visible light beam, uses an ESP32-S3 to share information allowing for complex game rules, for example implementing roles and interaction with other active in-game objects (totems).

<img width="3000" height="1983" alt="LightAir_V6R2_Assembly" src="https://github.com/user-attachments/assets/435b757f-cc38-42d4-a785-3cfa6ddafa06" />

This is the software library for LightAir. It is thought to allow for virtualization and testing and to hide the complex part inside objects.
It tries to make new game definitions as easy as possible, with the scope to promote the developing of new rule sets that work on the same platform.
It has to be compiled for ESP32-S3 and is written to be flashed using Arduino IDE, which is more simple than ESP-IDF.

## Build instruction
In order to compile you need `make`, `arduino-cli` and `python3` installed
(python only regenerates `src/lua/LightAir_GamesBundle.h` when a file under
`games/` changes).  The sketch is `LightAir.ino` at the repository root — one
image serves both player and totem devices; the role is chosen at boot from
NVS (see `sketches/LightAir_TotemProvisioning` to provision a totem).

```sh
make build/debug/LightAir.ino.bin
```

You can then upload the `LightAir.ino.bin` to the board using the Arduino IDE or Arduino CLI. For example:

```sh
arduino-cli upload --input-file ./build/debug/LightAir.ino.bin -p /dev/ttyACM0 -b esp32:esp32:esp32s3
```

### Host test suite (no hardware needed)
The Lua game engine, the game files and the TotemVM are covered by a PC-side
test suite that needs only `g++` and `lua5.4` — no ESP32 toolchain:

```sh
make -C test/host
```

See `test/host/README.md` for what each suite proves and how to read a
failure.  Run it before committing changes to `src/lua/`, `src/totem/`,
`src/game/LightAir_GameRunner.cpp` or `games/`.

### Unit testing
In the folder `src/test` there are several `.h` files, each one containing one or more unit test that can be run on the board. To add a test, define a new one using [AUnit](https://github.com/bxparks/AUnit) API, then include the corresponding file in the `src/test/LightAir_test.h` header.

To build the tests, run the following command:
```sh
make build/test/unit/LightAir.ino.bin
```
After uploading the sketch, the test output will be printed on the serial monitor.

### Build for WOKWI simulator

You can produce a binary targeting [WOWKI](https://wokwi.com/projects/new/esp32-s3) by using the corresponding profile:
```sh
PROFILE=ESP32-S3-WROOM-1-WOKWI make build/debug/LightAir.ino.bin
```
Then you can upload the binary by opening a new project on WOWKI and pressing F1 -> "Upload Firmware and Start Simulator" and selecting the desired binary.

## Documentation map
Game rulesets are **not C++**: every game is one `.lua` file under `games/`,
stored on the device's flash and exchangeable between devices over WiFi
(Settings → Share games).  Start here depending on what you want to do:

| I want to… | Read |
|---|---|
| write or modify a game ruleset | `docs/lua-games-design.md` (file format + the `la` API), then copy `games/freeforall.lua` — the fully-commented reference game |
| understand or extend the C++/Lua boundary | `docs/lua-embedding-guide.md` (stack discipline, GC, sandbox; §8 is the add-a-verb recipe) |
| understand or debug totem behaviour | `docs/totem-behavior-handshake.md` (TotemVM model + wire format; `test/host/totemvm.lua` is the executable reference encoder) |
| see what the tests prove | `test/host/README.md` |

## Design guidelines
### Nonviolent semantics
While a tag game is normally associated to a war simulation, LightAir wants to drop this label. We want to make clear a ray of light **is** a ray of light, not a metaphor for an ammunition or other means to offend people. This choice shows in many parts of the code, for example by the use of terms like LIT, SHONE, ENLIGHT instead of the common counterparts used in other tag games. Anyway, these terms have clear meanings and keep them throughout the code.
LightAir still represents conflict, but in a non-violent way where the interactions are a way to communicate and "recognize" each other, instead of submitting them.
This is not only an ethical choice, but mainly the base for more functional real-life interactions between players and also for the game lore.
### Open sourceness and participation
The whole LightAir project is based on open source - code is shared on github with GPL licence, while hardware parts are designed and realized with open source programs.
Participation is considered an important asset, so for example the software is structured to define a ruleset (a game type) with a file that is as simple as practically feasible.
### Game ruleset and configuration sharing
1. Game ruleset and configuration is decided by one designed player (NVM settings) right after boot.
2. once defined, the game and its configuration is shared with all the other players via ESP-NOW packets
3. therefore, players must all be within radio reach, while totems are not required to
4. Totems are "activated" by custom messages by the players. This allows totems to be planted outside radio reach at the beginning of the game
5. Game rulesets are being translated to Lua in order to be easily shareable as files, without re-compiling

