# Plan: Participant List Setup

## Problem
The game lifecycle currently has no step to define *who is playing* before `runner.begin()`.
The `clearRoster()` / `addToRoster()` API exists on `GameRunner` but nothing calls it.
Future games also need totems (bases, control points, flags, mines) whose count and roles
must be validated against per-game requirements before the game can start.

## New lifecycle step
```
setup():
  manager.selectGame(...)          // existing
  configMenu.run(...)              // existing (includes share prompt)
  participantMenu.run(runner, ...) // NEW — builds roster + totem list
  runner.begin(game, ...)          // existing
```

---

## 1. New struct: `TotemRole`  →  `src/game/LightAir_TotemRole.h`

```cpp
struct TotemRole {
    const char* name;      // e.g. "Respawn Base", "Control Point", "Flag"
    uint8_t     minCount;  // minimum of this role required before game can start
};
```

---

## 2. Extend `LightAir_Game`  (`src/game/LightAir_Game.h`)

Add three fields at the end of the struct:

```cpp
// ---- Participant requirements (optional) ----
uint8_t          minPlayers;       // min shooter-player count  (0 = no requirement)
const TotemRole* totemRoles;       // totem roles this game uses (nullptr = none)
uint8_t          totemRoleCount;   // number of entries in totemRoles[]
```

For FFA: `minPlayers=2`, `totemRoles=nullptr`, `totemRoleCount=0`.

---

## 3. Extend `LightAir_GameRunner`  (`src/game/LightAir_GameRunner.h/.cpp`)

Add a totem list parallel to the existing roster:

```cpp
// Totem management — call before begin()
void clearTotems();
void addTotem(uint8_t id, uint8_t roleIdx);  // roleIdx into game.totemRoles[]

uint8_t totemCount()               const;
uint8_t totemId(uint8_t i)         const;
uint8_t totemRole(uint8_t i)       const;
```

Private storage:
```cpp
struct TotemEntry { uint8_t id; uint8_t roleIdx; };
static constexpr uint8_t MAX_TOTEMS = 16;
TotemEntry _totems[MAX_TOTEMS];
uint8_t    _totemCount = 0;
```

Also add read-back for the roster (needed by the menu to validate):
```cpp
uint8_t rosterCount() const;
uint8_t rosterId(uint8_t i) const;
```

---

## 4. New class: `LightAir_ParticipantMenu`  (`src/game/LightAir_ParticipantMenu.h/.cpp`)

Blocking menu, similar pattern to `LightAir_GameConfigMenu`. Takes a reference to
`LightAir_GameRunner` and populates it.

### Flow
1. **Add-player phase**: display player-selector screen repeatedly until the user finishes.
   - `<>` scrolls through player IDs (1–15; ID 0 = None is skipped).
   - Already-added IDs are skipped.
   - `A` adds the selected player.
   - `B` ends the phase (only if `minPlayers` is already satisfied, otherwise shows error).
2. **Totem phases** (one phase per `game.totemRoles[r]`):
   - Same ID-selector but with role label shown.
   - `A` adds the selected ID as a totem with role `r`.
   - `B` ends this role's phase (only if `totemRoles[r].minCount` is satisfied).
3. **Summary screen** → `A:Start  B:Back` (B re-enters the last phase).
4. On confirm: the runner's roster and totem list are already populated; returns `MenuResult::Confirmed`.

### Display layout (per phase)

```
┌──────────────────┐
│ Players  2 added │   header: count added so far
│ < GRN >          │   selected player (short name)
│ A:Add  B:Done    │   B shown grey / locked if min not met
│ Min: 2           │   requirement reminder
└──────────────────┘
```

For a totem phase:
```
┌──────────────────┐
│ Respawn Base 1/2 │   role name + count / min
│ < YLW >          │   selected totem ID
│ A:Add  B:Done    │
│ Min: 2           │
└──────────────────┘
```

### Note on sharing
The participant list does NOT need a share broadcast for now. Each device sets up its
own list from local input (they all know the same game and config). A future enhancement
could broadcast the list via radio after the share-config step.

---

## 5. `src/config.h`

Add under `GameDefaults`:
```cpp
constexpr uint8_t MAX_TOTEMS = 16;
```

---

## 6. `src/LightAir.h`

Add includes for new files:
```cpp
#include "game/LightAir_TotemRole.h"
#include "game/LightAir_ParticipantMenu.h"
```

---

## 7. `src/rulesets/GameFreeForAll.cpp`

Update the `game_ffa` descriptor:
```cpp
/* minPlayers    */ 2,
/* totemRoles    */ nullptr,
/* totemRoleCount*/ 0,
```

---

## Files changed / created

| Action | File |
|--------|------|
| CREATE | `src/game/LightAir_TotemRole.h` |
| CREATE | `src/game/LightAir_ParticipantMenu.h` |
| CREATE | `src/game/LightAir_ParticipantMenu.cpp` |
| MODIFY | `src/game/LightAir_Game.h` |
| MODIFY | `src/game/LightAir_GameRunner.h` |
| MODIFY | `src/game/LightAir_GameRunner.cpp` |
| MODIFY | `src/LightAir.h` |
| MODIFY | `src/config.h` |
| MODIFY | `src/rulesets/GameFreeForAll.cpp` |
