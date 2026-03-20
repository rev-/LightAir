# Totem Architecture Refactor Plan

## Goal

Extract totem roles into self-contained registered descriptors, one file per role,
eliminating composite runners, hardcoded index splits, and the GenericTotemRoles
special-case encoding.

---

## New Concepts

### `TotemRole` struct  —  `src/totem/LightAir_TotemRole.h`

Analogous to `LightAir_Game`. Each role file defines one (or two, for team-split roles).

```cpp
struct TotemRole {
    uint8_t               roleId;         // globally unique; 0 = NONE sentinel
    const char*           name;           // display label e.g. "Base O", "Flag X", "CP"
    uint8_t               team;           // 0=O, 1=X, 0xFF=no team
    uint8_t               requiredCount;  // min instances needed (0 = optional)
    LightAir_TotemRunner* runner;         // singleton; must not be nullptr
};
constexpr uint8_t TOTEM_ROLE_NONE = 0;
```

### `TotemRoleIds.h`  —  `src/totem-rulesets/TotemRoleIds.h`

Central registry of all `roleId` constants (like `GameTypeIds.h`).

```cpp
namespace TotemRoleId {
    constexpr uint8_t BASE_O = 1;
    constexpr uint8_t BASE_X = 2;
    constexpr uint8_t FLAG_O = 3;
    constexpr uint8_t FLAG_X = 4;
    constexpr uint8_t CP     = 5;
    constexpr uint8_t BONUS  = 128;   // optional roles start at 128
    constexpr uint8_t MALUS  = 129;
}
```

### `TotemRoleManager`  —  `src/totem/LightAir_TotemRoleManager.h/.cpp`

Flat array of `const TotemRole*`, populated at boot via `registerRole()`.
Provides `findById(roleId)`, `count()`, `role(i)`. Capacity: `TotemDefs::MAX_TOTEM_ROLES = 32`.

### Updated `LightAir_Game` — replace totemVars with requirements

```cpp
struct TotemRequirement {
    uint8_t roleId;   // references a registered TotemRole
    uint8_t count;    // max instances this game needs of this role
};

// In LightAir_Game — replace totemVars/totemVarCount/totemRunner:
const TotemRequirement* totemRequirements;
uint8_t                 totemRequirementCount;
```

---

## New Files

| File | Contents |
|---|---|
| `src/totem/LightAir_TotemRole.h` | `TotemRole` struct, `TOTEM_ROLE_NONE` |
| `src/totem/LightAir_TotemRoleManager.h/.cpp` | `TotemRoleManager` class |
| `src/totem/AllTotems.h/.cpp` | `registerAllTotems(manager)` — one `assert(mgr.registerRole(...))` per role |
| `src/totem-rulesets/TotemRoleIds.h` | `TotemRoleId::` namespace constants |
| `src/totem-rulesets/BaseTotem.cpp` | `BaseRunner` (single stateless runner, shared by `role_base_o`+`role_base_x`); responds to `MSG_TOTEM_BEACON+1` with Respawn colour from `msg.team` |
| `src/totem-rulesets/FlagTotem.cpp` | `FlagRunnerO` + `FlagRunnerX` (team hardcoded at construction, no NVS load); each handles `MSG_FLAG_EVENT` filtered by `_myTeam`; defines `role_flag_o`, `role_flag_x` |
| `src/totem-rulesets/CPTotem.cpp` | `CPRunner` — full CP beacon/window/scoring logic extracted from `UpkeepCompositeRunner`; defines `role_cp` |
| `src/totem-rulesets/BonusTotem.cpp` | `BonusRunner` stub; defines `role_bonus` |
| `src/totem-rulesets/MalusTotem.cpp` | `MalusRunner` stub; defines `role_malus` |

**`BaseRunner` note:** Stateless — can be shared by both `role_base_o` and `role_base_x`
as a single static instance. Respawn colour comes from `msg.team`, not the role's own team.

**`FlagRunnerO/X` note:** Two distinct class instances with `_myTeam` = 0 or 1 set at
construction. Eliminates the NVS team load that existed in `FlagCompositeRunner::reset()`.

---

## Modified Files

### `src/game/LightAir_Game.h`
- Remove `totemVars`, `totemVarCount`, `totemRunner`.
- Add `totemRequirements`, `totemRequirementCount`.
- `TotemVar` struct deprecated (remove after Phase 2).

### `src/rulesets/GameTeams.cpp`
- Replace `totemVars[8]` with `totemRequirements[2]` referencing `BASE_O (count=4)`, `BASE_X (count=4)`.
- Remove `baseO_ids[]`, `baseX_ids[]`.
- Remove `BaseRunner` inner class.
- `isMyTeamBase()` queries new device-ID store on `GameRunner` (see below).

### `src/rulesets/GameFlag.cpp`
- Replace `totemVars[10]` with `totemRequirements[4]`: `BASE_O(4)`, `BASE_X(4)`, `FLAG_O(1)`, `FLAG_X(1)`.
- Remove `FlagCompositeRunner`, `baseO_ids[]`, `baseX_ids[]`, `flagO_id`, `flagX_id`.
- `isMyTeamBase()`, `isEnemyFlag()` query device-ID store.

### `src/rulesets/GameUpkeep.cpp`
- Replace `totemVars[12]` with `totemRequirements[3]`: `CP(6)`, `BASE_O(3)`, `BASE_X(3)`.
- Remove `UpkeepCompositeRunner`, `cpIds[]`, `baseO_ids[]`, `baseX_ids[]`.
- `cpIndex()`, `isMyTeamBase()` query device-ID store.

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

**Beacon intercept (step 2a):** Simplifies to one branch — `payload[0] = _totems[t].roleId`.
The `0xFF` two-byte generic path is gone; generic roles are plain `roleId` values (128+).

**`_genericRoles[]` array:** Removed.

### `src/game/LightAir_GameSetupMenu.h/.cpp`

**Constructor:** Accepts `LightAir_TotemRoleManager&`.

**`_totemAssignment[]` encoding:** Values become plain `roleId` (0 = NONE). The
`GenericTotemRoles::COUNT + tvIdx` offset arithmetic is gone entirely.

**`nextTotemRole()`:** Options list built from `game.totemRequirements` (named roles)
plus `_totemRoleMgr` entries with `roleId >= 128` (optional/generic roles). Instance
slots for `count > 1` roles shown with suffixes ("Base O 1", "Base O 2"…).

**`totemRoleLabel()`:** `_totemRoleMgr.findById(val)->name`, asterisk if `requiredCount > 0`.

**`validateTotems()`:** For each requirement with `requiredCount > 0`, check at least
one slot holds that `roleId`.

**`commitToRunner()`:** Single loop — for each assigned slot call
`_runner.addTotem(id, roleId)` and `_runner.addTotemId(roleId, instance, id)`.
No named-vs-generic branch.

**`isRoleAvailable()`:** Uniqueness check now covers role+instance pairs for multi-count roles.

### `src/totem/LightAir_TotemDriver.h/.cpp`

**Constructor:** Takes `LightAir_TotemRoleManager&`; drops `LightAir_GameManager&`
(no longer needed for runner lookup).

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
    _runner->onMessage(ev.packet, out);
}
```
`typeId` still comes from the packet header (unchanged) for radio filtering.
Runners no longer need `_role == ROLE_UNKNOWN` guards; the driver dispatches
to the right runner before the first `onMessage` call.

### `src/config.h`
- Add `TotemDefs::MAX_TOTEM_ROLES = 32`.
- Remove `GenericTotemRoles` namespace (replaced by `TotemRoleId::BONUS`, `MALUS`).

### Config blob format  —  `game_serialize_config()` / `game_apply_config()`

Old format included `totemVarCount × int32_t` named IDs + `16 × int32_t` generic roles.
New format:

```
[uint16_t typeId]
[configVar0..N × int32_t]
[uint8_t totemAssignment[16]]   — roleId per slot; 0 = NONE
[uint8_t totemDeviceId[16]]     — hardware device ID per slot
[teamBitmask × int32_t]         — if hasTeams; unchanged
[uint8_t sessionToken]
```

Smaller (example: Flag shrinks from ~113 bytes to ~69 bytes). No longer requires
`totemVarCount` to be known at the receiver. Both arrays are fixed at 16 entries
(= `TotemDefs::MAX_TOTEMS`).

---

## Migration Phases

### Phase 1 — Totem firmware only (non-breaking)

Can land without changing player firmware or config blob format.

1. Create `TotemRoleIds.h`, `LightAir_TotemRole.h`, `LightAir_TotemRoleManager`.
2. Create five totem-ruleset files. Wire each runner as a wrapper delegating to the
   composite runner (keeps behaviour identical while establishing the new structure).
3. Create `AllTotems.h/.cpp`.
4. Update `TotemDriver` to accept `TotemRoleManager`; add `findRunner(roleId)` alongside
   old `findRunner(typeId)`. Driver tries `roleId` path first, falls back to old path.
5. Totem firmware independently testable with new structure; player firmware unchanged.

### Phase 2 — Full cut-over (breaking, deploy together)

6. Update `LightAir_Game` struct (remove `totemVars`/`totemRunner`, add `totemRequirements`).
7. Update all five game ruleset files to use `totemRequirements` + query device-ID store.
8. Update `GameRunner`: `_totemIdMap`, `getTotemId()`, beacon intercept, `onBegin` signature.
9. Update `GameSetupMenu`: new `_totemAssignment` encoding, `TotemRoleManager` integration.
10. Update `game_serialize_config` / `game_apply_config` for new blob format.
11. Remove `GenericTotemRoles`, old composite runners, `TotemVar` struct.
12. Remove old `findRunner(typeId)` fallback from `TotemDriver`.

---

## Open Questions

1. **`onBegin` signature change** — adding `LightAir_GameRunner*` touches every game file.
   Alternative: a free-function accessor in a thin `LightAir_TotemIdStore.h` that
   `commitToRunner` populates. Avoids the signature change at the cost of a hidden global.

2. **Dynamic count for CP/BASE** — `numControlPoints` and `numBases` are config vars, so
   the active count is runtime, not compile-time. `CPRunner` and `BaseRunner` stay
   unaware of the count; `cpIndex()` / `isMyTeamBase()` already guard against `deviceId == 0`
   entries. No structural change needed.

3. **Config blob versioning** — mismatched old/new firmware will fail silently on `typeId`
   mismatch (same as today). Acceptable for now; a version byte can be added later.
