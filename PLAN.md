# Totem Architecture Refactor Plan

## Goal

Extract totem roles into self-contained registered descriptors, one file per role,
eliminating composite runners, hardcoded index splits, and the GenericTotemRoles
special-case encoding.

---

## New Concepts

### `TotemRole` struct  —  `src/totem/LightAir_TotemRole.h`

Analogous to `LightAir_Game`. Pure static descriptor — no count information.
Count constraints are always game-specific and live in `TotemRequirement`.

```cpp
struct TotemRole {
    uint8_t               roleId;   // globally unique; 0 = NONE sentinel
    const char*           name;     // display label e.g. "Base O", "Flag X", "CP"
    uint8_t               team;     // 0=O, 1=X, 0xFF=no team
    LightAir_TotemRunner* runner;   // per-role instance; must not be nullptr
};
constexpr uint8_t TOTEM_ROLE_NONE = 0;
```

### `TotemRoleIds.h`  —  `src/totem-rulesets/TotemRoleIds.h`

Central registry of all `roleId` constants (like `GameTypeIds.h`).
All IDs are sequential — no special offset for optional roles.

```cpp
namespace TotemRoleId {
    constexpr uint8_t BASE_O = 1;
    constexpr uint8_t BASE_X = 2;
    constexpr uint8_t FLAG_O = 3;
    constexpr uint8_t FLAG_X = 4;
    constexpr uint8_t CP     = 5;
    constexpr uint8_t BONUS  = 6;
    constexpr uint8_t MALUS  = 7;
}
```

### `TotemRoleManager`  —  `src/totem/LightAir_TotemRoleManager.h/.cpp`

Flat array of `const TotemRole*`, populated at boot via `registerRole()`.
Provides `findById(roleId)`, `count()`, `role(i)`.
Capacity: `TotemDefs::MAX_TOTEM_ROLES = 32`.

The manager is a **game-independent static registry**. It knows nothing about
which game is running or how many totems are needed. That information belongs
entirely in `TotemRequirement` inside each game's ruleset.

### Updated `LightAir_Game` — replace totemVars with requirements

All count constraints (min and max instances per role) are defined here,
inside each game ruleset. There are no global defaults — every role's count
is determined by the game configuration.

```cpp
struct TotemRequirement {
    uint8_t roleId;    // references a registered TotemRole
    uint8_t minCount;  // min slots the DM must assign; 0 = optional
    uint8_t maxCount;  // max slots the DM may assign; 0 = up to MAX_TOTEMS
};

// In LightAir_Game — replace totemVars/totemVarCount/totemRunner:
const TotemRequirement* totemRequirements;
uint8_t                 totemRequirementCount;
```

Example for Flag:
```cpp
static const TotemRequirement flagRequirements[] = {
    { TotemRoleId::BASE_O, 1, 4 },   // 1..4 O bases
    { TotemRoleId::BASE_X, 1, 4 },   // 1..4 X bases
    { TotemRoleId::FLAG_O, 1, 1 },   // exactly 1 O flag
    { TotemRoleId::FLAG_X, 1, 1 },   // exactly 1 X flag
    { TotemRoleId::BONUS,  0, 4 },   // 0..4 bonus totems (optional)
};
```

---

## Runner State Machines

Each runner has a well-defined state machine. The driver handles the
UNACTIVATED → ACTIVE transition (finding the runner by roleId on first
`MSG_TOTEM_BEACON+1`). Runners begin in their first active state.

### BASE runner  (`BaseTotem.cpp`)

Two distinct instances: `base_o_runner` (team=O) and `base_x_runner` (team=X),
same class, team baked in at construction. **Not stateless.**

```
ACTIVATED ──► BEACONING ──► RESPAWNING ──► BEACONING
```

- **ACTIVATED** (entry): plays brief identity animation — one LED of strip blinks
  in team colour (cyan for O, magenta for X). Transitions to BEACONING when animation ends.
- **BEACONING**: sends `MSG_BASE_O` or `MSG_BASE_X` periodically via `update()`.
  LED: steady team colour on RGB LED, strip shows team-colour pattern.
  On receiving a player reply (`OUT_GAME` player responds to beacon):
  → RESPAWNING, storing replying player's team.
- **RESPAWNING**: wipes LED strip with the respawned player's team colour.
  Transitions back to BEACONING when animation ends.

### FLAG runner  (`FlagTotem.cpp`)

Two instances: `flag_o_runner` (team=O, taken by X players) and
`flag_x_runner` (team=X, taken by O players). Same class, team baked in.

```
FLAG_IN ──► FLAG_OUT ──► FLAG_IN
```

- **FLAG_IN**: sends `MSG_FLAG_IN` periodically. Shows "flag present" animation.
  On receiving a reply from an enemy-team player: → FLAG_OUT.
- **FLAG_OUT**: no beacon. Shows "flag taken" animation (e.g. blinking in
  enemy team colour). Silent to player messages.
  On `MSG_FLAG_RETURN` received: → FLAG_IN (flag returned without scoring).
  On `MSG_FLAG_SCORE` received: → FLAG_IN (flag scored; score is tracked by the
  player who scored, not the totem).

### CP runner  (`CPTotem.cpp`)

Single instance `cp_runner` (no team). Tracks per-team scores internally.

```
NEUTRAL ──► OWNED_O ──► OWNED_X ──► OWNED_O …
              │               │
           (scores O)      (scores X)
```

- **NEUTRAL**: sends `MSG_CP_NEUTRAL`. No scoring.
  If all replies in the most recent beacon cycle are from team O: → OWNED_O.
  If all replies are from team X: → OWNED_X.
  Mixed replies (both teams) or no replies: stays NEUTRAL.
- **OWNED_O**: sends `MSG_CP_OWNED_O`. Scores 1 point for team O every 10 s;
  broadcasts `MSG_CP_SCORE` with cumulative O/X totals after each tick.
  Records scores internally for roster report.
  If all replies in most recent beacon cycle are from team X: → OWNED_X.
- **OWNED_X**: mirror of OWNED_O for team X.

**Score reporting**: scores are broadcast in real-time via `MSG_CP_SCORE` so
players stay informed. At game end (`onRoster()`), the CP includes cumulative
totals in its roster response payload. The host takes the roster response as
authoritative (guards against radio packet loss in real-time broadcasts).
Data fusion: if host tracked real-time `MSG_CP_SCORE` messages, it compares
against the roster totals and uses the roster values as ground truth.

### BONUS / MALUS runner  (`BonusTotem.cpp`, `MalusTotem.cpp`)

```
READY ──► COOLDOWN ──► READY
```

- **READY**: sends `MSG_BONUS` / `MSG_MALUS` periodically.
  On receiving a player reply: store last-trigger timestamp → COOLDOWN.
- **COOLDOWN**: no beacon; silent for configurable duration.
  After cooldown expires: → READY.

What BONUS and MALUS do is defined in player-side game logic; the totem
only signals availability and records the last trigger time.

---

## New Files

| File | Contents |
|---|---|
| `src/totem/LightAir_TotemRole.h` | `TotemRole` struct, `TOTEM_ROLE_NONE` |
| `src/totem/LightAir_TotemRoleManager.h/.cpp` | `TotemRoleManager` class |
| `src/totem/AllTotems.h/.cpp` | `registerAllTotems(manager)` — one call per role |
| `src/totem-rulesets/TotemRoleIds.h` | `TotemRoleId::` namespace constants |
| `src/totem-rulesets/BaseTotem.cpp` | `BaseRunner` class; defines `base_o_runner`, `base_x_runner`, `role_base_o`, `role_base_x` |
| `src/totem-rulesets/FlagTotem.cpp` | `FlagRunner` class; defines `flag_o_runner`, `flag_x_runner`, `role_flag_o`, `role_flag_x` |
| `src/totem-rulesets/CPTotem.cpp` | `CPRunner` class; defines `cp_runner`, `role_cp` |
| `src/totem-rulesets/BonusTotem.cpp` | `BonusRunner` class; defines `bonus_runner`, `role_bonus` |
| `src/totem-rulesets/MalusTotem.cpp` | `MalusRunner` class; defines `malus_runner`, `role_malus` |

---

## Modified Files

### `src/game/LightAir_Game.h`
- Remove `totemVars`, `totemVarCount`, `totemRunner`.
- Add `totemRequirements`, `totemRequirementCount`.
- `TotemVar` struct deprecated (remove after Phase 2).

### `src/rulesets/GameTeams.cpp`
- Replace `totemVars[8]` with `totemRequirements[]` referencing `BASE_O` and
  `BASE_X` with per-game min/max counts.
- Remove `baseO_ids[]`, `baseX_ids[]`, `BaseRunner` inner class.

### `src/rulesets/GameFlag.cpp`
- Replace `totemVars[10]` with `totemRequirements[]`: BASE_O, BASE_X, FLAG_O,
  FLAG_X, optionally BONUS.
- Remove `FlagCompositeRunner` and all id arrays.

### `src/rulesets/GameUpkeep.cpp`
- Replace `totemVars[12]` with `totemRequirements[]`: CP, BASE_O, BASE_X,
  with per-game min/max counts.
- Remove `UpkeepCompositeRunner` and all id arrays.

### `src/game/LightAir_GameRunner.h/.cpp`

**`TotemEntry`:** `roleIdx` → `roleId`.

**Device-ID store** (replaces `int* id` pointers in `TotemVar`):
```cpp
struct TotemIdEntry { uint8_t roleId; uint8_t instance; uint8_t deviceId; };
TotemIdEntry _totemIdMap[GameDefaults::MAX_PARTICIPANTS];
uint8_t      _totemIdCount = 0;

void    addTotemId(uint8_t roleId, uint8_t instance, uint8_t deviceId);
uint8_t getTotemId(uint8_t roleId, uint8_t instance) const;  // 0 = not assigned
```

**`onBegin` signature:** Add `LightAir_GameRunner*` as fourth parameter so ruleset
callbacks can call `runner->getTotemId()`. All existing `onBegin` implementations
gain this parameter (may be ignored if unused).

**Beacon intercept (step 2a):** Simplified to a single branch — `payload[0] = _totems[t].roleId`.
The `0xFF` two-byte generic path is gone entirely; all roles are plain sequential `roleId` values.

**`_genericRoles[]` array:** Removed.

### `src/game/LightAir_GameSetupMenu.h/.cpp`

**Constructor:** Accepts `LightAir_TotemRoleManager&`.

**`_totemAssignment[]` encoding:** Values become plain `roleId` (0 = NONE).
The `GenericTotemRoles::COUNT + tvIdx` offset arithmetic is gone entirely.

**`nextTotemRole()`:** Options built from `game.totemRequirements`. Instance
slots for roles with `maxCount > 1` shown with suffixes ("Base O 1", "Base O 2"…).
The DM cannot assign more instances of a role than its `maxCount`.

**`totemRoleLabel()`:** `_totemRoleMgr.findById(val)->name`, asterisk if `minCount > 0`.

**`validateTotems()`:** For each requirement, check that the number of assigned
slots for that `roleId` is within `[minCount, maxCount]`.

**`commitToRunner()`:** Single loop — for each assigned slot call
`_runner.addTotem(id, roleId)` and `_runner.addTotemId(roleId, instance, id)`.
No named-vs-generic branch.

**`isRoleAvailable()`:** Checks that assigning this role+slot does not exceed
`maxCount` for the role and does not duplicate a device already assigned.

### `src/totem/LightAir_TotemDriver.h/.cpp`

**Constructor:** Takes `LightAir_TotemRoleManager&`; drops `LightAir_GameManager&`.

**`findRunner()`:** `findRunner(typeId)` → `findRunner(uint8_t roleId)`:
```cpp
LightAir_TotemRunner* findRunner(uint8_t roleId) const {
    const TotemRole* r = _totemRoleMgr.findById(roleId);
    return r ? r->runner : nullptr;
}
```

**Activation path:** On first `MSG_TOTEM_BEACON+1` with `payloadLen >= 1`:
```cpp
uint8_t roleId = ev.packet.payload[0];
_runner = findRunner(roleId);
if (_runner) {
    _radio.setTypeId(ev.packet.typeId);
    _runner->reset();
    _runner->onMessage(ev.packet, out);
}
```
`typeId` still comes from the packet header for radio filtering.
Runners no longer need `_role == ROLE_UNKNOWN` guards; the driver dispatches
to the correct runner before the first `onMessage` call. `reset()` is called
once here to initialise runner state (entering the first active state).

### `src/config.h`
- Add `TotemDefs::MAX_TOTEM_ROLES = 32`.
- Remove `GenericTotemRoles` namespace (replaced by `TotemRoleId::BONUS`, `MALUS`).

### Config blob format  —  `game_serialize_config()` / `game_apply_config()`

Old format: `totemVarCount × int32_t` named IDs + `16 × int32_t` generic roles.
New format:

```
[uint16_t typeId]
[configVar0..N × int32_t]
[uint8_t totemAssignment[16]]   — roleId per slot; 0 = NONE
[uint8_t totemDeviceId[16]]     — hardware device ID per slot
[teamBitmask × int32_t]         — if hasTeams; unchanged
[uint8_t sessionToken]
```

Smaller (example: Flag shrinks from ~113 bytes to ~69 bytes). No longer
requires `totemVarCount` to be known at the receiver. Both arrays are fixed
at 16 entries (= `TotemDefs::MAX_TOTEMS`).

---

## Migration Phases

### Phase 1 — Totem firmware only (non-breaking)

Can land without changing player firmware or config blob format.

1. Create `TotemRoleIds.h`, `LightAir_TotemRole.h`, `LightAir_TotemRoleManager`.
2. Create five totem-ruleset files with full state machines (no delegation to old runners).
3. Create `AllTotems.h/.cpp`.
4. Update `TotemDriver` to accept `TotemRoleManager`; add `findRunner(roleId)` alongside
   old `findRunner(typeId)`. Driver tries `roleId` path first, falls back to old path.
5. Totem firmware independently testable with new structure; player firmware unchanged.

### Phase 2 — Full cut-over (breaking, deploy together)

6. Update `LightAir_Game` struct (remove `totemVars`/`totemRunner`, add `totemRequirements`).
7. Update all game ruleset files to use `totemRequirements` + query device-ID store.
8. Update `GameRunner`: `_totemIdMap`, `getTotemId()`, simplified beacon intercept, `onBegin` signature.
9. Update `GameSetupMenu`: new `_totemAssignment` encoding, min/max validation, `TotemRoleManager` integration.
10. Update `game_serialize_config` / `game_apply_config` for new blob format.
11. Remove `GenericTotemRoles`, old composite runners, `TotemVar` struct.
12. Remove old `findRunner(typeId)` fallback from `TotemDriver`.

---

## Open Questions

1. **`onBegin` signature change** — adding `LightAir_GameRunner*` touches every game file.
   Alternative: a free-function accessor in a thin `LightAir_TotemIdStore.h` that
   `commitToRunner` populates. Avoids the signature change at the cost of a hidden global.

2. **Config blob versioning** — mismatched old/new firmware will fail silently on `typeId`
   mismatch (same as today). Acceptable for now; a version byte can be added later.

3. **CP score fusion details** — the host takes roster response totals as authoritative
   over real-time `MSG_CP_SCORE` tracking. If multiple CP totems exist (e.g. Upkeep with 6),
   each reports independently; the host sums across all CP roster responses.
   Non-responding totems (after the existing retry logic exhausts) leave a gap in the
   final tally — this is accepted behaviour.
