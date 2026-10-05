# Bench checklist — on-device checks still owed

Changes that passed the host suite and the ESP32 build but have not yet run
on hardware.  Each entry says what to do, what to expect, and what to bring
back.  Delete an entry once it has passed on a projector (ESP32-S3 N4, no
PSRAM).

Serial: USB CDC at 115200; the firmware logs at INFO.

---

## 1. One LED DMA buffer for both powers — commit `04be7b7`

The optics used to hold the LED waveform twice (full and low power, 31.2 KB
each).  Now one buffer is rewritten between two cycles when a shot switches
to low power, and back at the start of the next shot.  The DMA bytes are
proven identical on the host; what only the bench can show is timing and
measurement behaviour.

For A/B comparisons keep one projector on the previous firmware (`dd992a0`),
or reflash between runs.  Same target, same distance, same repetitions.

| # | Check | How | Pass |
|---|---|---|---|
| 1.1 | **The games that failed now load** | Start flag, upkeep, kingofhill (and festasportsasso) from the game list | each loads; no "Game failed: not enough memory" |
| 1.2 | Heap gained | At boot, note `Lua: allocator using internal RAM (N B free internal)`; when selecting a game, note `GameStore: loading … heap … largest block …` | free heap about 26 KB higher than on `dd992a0` |
| 1.3 | Low-power switch time | Settings → Test Mode, reps ~20 (`<` / `>`), aim at a close retroreflector until the screen shows `L` after the saturation %; fire with TRIG1 | serial prints `Enlight test: low power, LED buffer rewrite N us`; expected 50–150 µs, investigate above 500 µs |
| 1.4 | Readings unchanged — close | Test Mode, saturating target, 10 shots per firmware | same hit result; `r`/`a` and r:g:b within the old firmware's shot-to-shot spread; `L` shown in both |
| 1.5 | Readings unchanged — mid and far | Test Mode, one target at mid range and one near the maximum range, 10 shots each per firmware | same hit rate; coordinates within the old spread |
| 1.6 | Full power comes back | Fire a saturating close shot (`L`), then immediately a far shot | the far shot reads like a far shot with no close shot before it (no `L`, same coordinates) |
| 1.7 | No stalls in play | A match with a fast projector (FAST) firing rapidly at close range | trigger never sticks; no `run did not complete` or `lost the cycle task` on serial |
| 1.8 | Calibration still completes | Settings → Calibration, all steps | finishes and the new values hold in Test Mode |

Bring back: the numbers from 1.2 and 1.3, and a note of any difference seen
in 1.4–1.6.

---

## 2. Lua libraries without debug info, string table at two per bucket

`std.lua` and `projector.lua` are stripped of debug information after they
compile, and Lua's string table grows later.  Host-measured: about 12 KB off
every ruleset that loads both libraries (upkeep 109.9 → 97.9 KB).

| # | Check | How | Pass |
|---|---|---|---|
| 2.1 | Every game loads and plays | Each game in the list: start it, play a minute | no load failure, no Lua fault on serial |
| 2.2 | Lua cost dropped | Compare `LuaGame: loaded '…' (N KB of Lua)` with the same line on an older build | about 10–12 KB lower per game (tirobersaglio, which loads one library, less; freeforall loads both from the area-service build on, so compare it only between builds that agree) |
| 2.3 | Library errors still name the library | Upload a custom game that calls `proj.define{ max_owned = "3" }` | failure screen reads `projector.lua:-1: attempt to compare …` |

---

## 3. Splash: the firmware's area service

A SPLASH projector bursts in every game, with no game-file wiring: the
player it hits broadcasts an area beacon, players near them lose lives by
distance (close: 2, further: 1) under the game's own rules, and a knock-out
by the area scores for the shooter.  Needs three players: shooter, victim,
bystander.  Hand the shooter SPLASH from a BONUS totem set to SPLASH
(Totems submenu, O key).

| # | Check | How | Pass |
|---|---|---|---|
| 3.1 | Bystander close | Teams, friendly fire off; victim and bystander on the other team, ~1 m apart; shooter hits the victim with SPLASH | victim −1 life; bystander −2 |
| 3.2 | Bystander further | Same, bystander ~5–10 m from the victim | bystander −1 |
| 3.3 | Out of reach | Bystander well away (> 20 m) | bystander unchanged |
| 3.4 | Friendly fire | Bystander on the shooter's team, friendly fire off | bystander unchanged |
| 3.5 | No self-splash | Shooter standing next to the victim, friendly fire on | shooter unchanged |
| 3.6 | Missed or refused hit | Shooter hits a victim who is immune (second hit within 3 s) | no bystander loses anything |
| 3.7 | Area knock-out credits the shooter | 3.1 with the bystander on 2 lives | bystander out; shooter's points +1 and "<bystander> SHONE!" on the shooter's screen |
| 3.8 | Area hits open no immunity window | 3.1, then the shooter hits the bystander directly at once | the direct hit lands (−1) |
| 3.9 | Other games | Repeat 3.1 in freeforall, flag and upkeep | same behaviour |

In Virus a SPLASH area hit is a clean hit: it puts viruses down (§5.9) and
does nothing to clean players.

Bring back: the distances at which the 2-life and 1-life bands actually
switch (RSSI bands −55 / −70 dBm), since body shadowing moves them.

---

## 4. Outflow: powered projectors on the life pool

Outflow's BONUS totems now hand out SPLASH, FAST, LONG and STRONG, and every
projector draws from the one energy pool that is the player's life.  Two
players; Outflow at its defaults (Energy 100, LitCost 50); a BONUS totem
whose option is changed with O between checks.

| # | Check | How | Pass |
|---|---|---|---|
| 4.1 | A pickup heals nothing | Drain to ~60, claim BONUS STRONG | "BONUS STRONG", STRONG icon in the energy cell, energy still ~60 |
| 4.2 | Beams cost the life pool | Fire STRONG a few times | energy −1 per beam, as with the baseline |
| 4.3 | Strength counts | The target claims BONUS LIFE first (→ 200), then takes a STRONG hit | target −150 |
| 4.4 | No recharge of its own | Claim FAST, leave the trigger alone for 30 s | energy only drains |
| 4.5 | Going out drops it | Get shone out, wait for the respawn | back with the standard energy icon and 100 energy |
| 4.6 | SPLASH area | Third player ~1 m from the target of a SPLASH hit | bystander −100 (2 × LitCost) |

---

## 5. Virus: down, and back at a totem

A clean player's beam (or a clean SPLASH's area) puts a virus down for
`virus_respawn_secs` (Respawn in the menu, default 30 s); then the virus
walks to any totem and is back when the totem answers its touch.  Three
players and one BONUS totem; Virus at its defaults.

| # | Check | How | Pass |
|---|---|---|---|
| 5.1 | Friendly fire | A clean player shines another clean player | no effect; the shooter hears the friendly-fire cue |
| 5.2 | Down | A clean player shines the virus | virus: "Down" cue, red pulse stops, DOWN bar filling over 30 s, "Wait to respawn" / "LIT by …"; shooter: "<virus> is DOWN! +2" |
| 5.3 | No early respawn | The down virus stands at the totem during the 30 s | nothing happens |
| 5.4 | Back at a totem | Wait out the 30 s, then walk to the totem | tray switches to "Go to a totem"; within ~2 m the totem plays the chaser in the virus's colour and the virus is back ("Up", red pulse, energy full, "Safe for 5 s") |
| 5.4b | Grace | Shine the virus within 5 s of it coming back | nothing; the shooter hears "Immune". After 5 s it goes down again |
| 5.5 | Too far | Wait out the 30 s at ~5 m from the totem | nothing until the virus walks closer |
| 5.6 | Pickup untouched | Respawn at a BONUS totem that is READY | it stays READY: a clean player can still claim it right after |
| 5.7 | Cooldown no obstacle | Respawn at a BONUS totem just claimed (in cooldown) | the chaser plays and the virus is back |
| 5.8 | No totems | A match with no totems assigned | the virus is back when the 30 s are up, on the spot |
| 5.9 | SPLASH | A clean player holding SPLASH (BONUS) shines a virus standing ~1 m from another virus | both go down; the shooter sees both "is DOWN! +2" |
| 5.10 | Virus on virus | One virus shines another | the target goes down; the shooter sees "is DOWN! +1" |
| 5.11 | Points | Infect a clean player; play until one clean player is left | infection "+5"; the last clean player sees "Last clean! +10"; the end screen and the winner follow points, then time stayed clean |

Bring back: how far from the totem the touch is answered (gate −55 dBm),
since the totem's antenna and the player's body move it.

---

## Data to collect while there

Not pass/fail — these calibrate the host memory model used to size the next
RAM cuts (debug-info stripping, string table, the memory-budget test).

- For every game in the list: the `GameStore: loading …` line (heap and
  largest block before the load) and the `LuaGame: loaded '…' (N KB of Lua)`
  line after it.
- Which games, if any, still refuse to load, and the exact text on the
  failure screen.
