#ifndef LIGHTAIR_PROJECTORCTRL_H
#define LIGHTAIR_PROJECTORCTRL_H

#include <stdint.h>
#include "../config.h"
#include "LightAir_Projector.h"
#include "LightAir_ProjectorOutput.h"

class Enlight;
class LightAir_UICtrl;

// ================================================================
// LightAir_ProjectorCtrl — owns which projectors a player holds, their
// energy, and the push of optical settings into Enlight.
//
// Responsibilities, all of which used to be copy-pasted per ruleset:
//   - the inventory: the structural baseline plus up to maxOwned powered
//     projectors, FIFO-evicted when full
//   - each projector's own energy pool and its refill rule
//   - the shot: readyMs, energy, Enlight::run(), the UI action
//   - attacker-side per-target anti-spam (targetImmunityMs)
//   - keeping the display globals pointed at the active projector
//
// Layering: Enlight never learns what a Projector is.  This class
// decomposes the optical fields into setRepetitions / setCooldown /
// setRangeM and nothing else crosses that line.
//
// Wiring (mirrors how the sketch already binds hardware):
//   setup():          projector.begin(enlightPtr, &uiCtrl);
//   GameRunner::begin(): projector.setGame(game.projectors);
// ================================================================

// ---------------------------------------------------------------
// Display globals.
//
// MonitorVar tables are `static const`, so a bound pointer must be a
// compile-time constant — the reason these are globals rather than
// members.  ProjectorCtrl keeps them pointed at the active projector,
// so the OLED follows a switch with no re-binding.
//
//   MonitorVar::Str   ("Proj",   projectorName,   mask, ICON_ROLE,   0, 0)
//   MonitorVar::IntDyn("Energy", &projectorEnergy, mask, &projectorIcon, 1, 0)
//
// projectorEnergy is the AUTHORITY for the active projector's pool: a
// ruleset may write it directly (GameOutflow's passive drain and its
// uncapped reward both do).  Its value is saved into the slot on a switch
// and reloaded from the next one.  It is `int`, not uint8_t, precisely
// because a ruleset may push it past 255.
// ---------------------------------------------------------------
extern char           projectorName[ProjectorLimits::NAME_LEN];
extern int            projectorEnergy;
extern const uint8_t* projectorIcon;

class LightAir_ProjectorCtrl {
public:
    // Bind hardware.  Call once from setup(), before any game starts.
    void begin(Enlight* enlight, LightAir_UICtrl* ui);

    // Bind the running game's declaration and reset the inventory to the
    // baseline at full energy.  set may be nullptr = standard BASE only.
    void setGame(const ProjectorSet* set);

    // ---- the shot -------------------------------------------------
    // Direct call, not queued (see LightAir_ProjectorOutput.h).
    // Returns true only if the shot actually started, and only then is
    // energy deducted — preserving today's (energy > 0) && run() short-circuit,
    // under which a refused shot has never cost anything.
    bool trigger();

    // ---- attacker-side anti-spam ----------------------------------
    // mayLight() is false while this target is still inside the active
    // projector's targetImmunityMs window.  The table is per-TARGET state and
    // deliberately survives a switch: resetting it would turn switching into a
    // way to bypass the window.
    bool mayLight(uint8_t targetId) const;
    void noteLit(uint8_t targetId);

    // ---- inventory ------------------------------------------------
    bool select(uint8_t id);   // switch to one already held
    bool give(uint8_t id);     // add at full energy, or refill if already held
    bool grant(uint8_t id);    // give + select
    bool drop(uint8_t id);     // remove (never the baseline)
    void next();
    void prev();

    // Per-cycle: applies any deferred switch, ticks the refill, re-checks
    // availability.  Called by GameRunner in the OUTPUT phase.
    void update();
    void apply(const ProjectorOutput& out);

    // ---- ruleset overrides ----------------------------------------
    // Point the baseline's pool at the DM's config vars.  Call from onBegin;
    // sets the live pool to max as the old `energy = startEnergy` did.
    void setPool(int maxEnergy, uint16_t rechargeDelayMs);

    // ---- queries --------------------------------------------------
    const Projector& active() const { return _defs[_slots[_activeIdx].id]; }
    uint8_t activeId()        const { return _slots[_activeIdx].id; }
    uint8_t ownedCount()      const { return _slotCount; }          // includes the baseline
    uint8_t ownedId(uint8_t i) const;
    bool    owns(uint8_t id)  const { return findSlot(id) >= 0; }
    int     maxEnergy()       const;

    // True once per change of held projector; GameRunner turns it into
    // UIEvent::ProjectorChange.  Reading it clears it.
    bool consumeChanged();
    // Set when FIFO eviction dropped something, for the tray message.
    // Reading it clears it; evictedName() stays valid until the next eviction.
    bool        consumeEvicted();
    const char* evictedName() const { return _evictedName; }

private:
    struct Slot {
        uint8_t  id;
        int16_t  energy;      // int16: a ruleset may push the pool past 255
        uint32_t acquiredAt;  // fixes the FIFO eviction order
        uint32_t lastShotAt;
        uint32_t rampAt;      // next RAMP step due
    };

    Enlight*            _enlight = nullptr;
    LightAir_UICtrl*    _ui      = nullptr;
    const ProjectorSet* _set     = nullptr;

    // Resolved, clamped definitions indexed by ProjectorId.
    Projector _defs[ProjectorId::COUNT];
    bool      _defined[ProjectorId::COUNT] = {};

    // Slot 0 is the baseline: always present, never counted against
    // maxOwned, never evicted.  Slots 1.._slotCount-1 are powered.
    Slot     _slots[1 + ProjectorLimits::MAX_OWNED];
    uint8_t  _slotCount = 1;
    uint8_t  _activeIdx = 0;

    int16_t  _pendingIdx = -1;   // deferred switch; applied when Enlight is idle
    bool     _pendingDrop = false;
    uint32_t _readyAt   = 0;     // millis() before which trigger() refuses
    bool     _changed   = false;
    bool     _evicted   = false;
    char     _evictedName[ProjectorLimits::NAME_LEN] = {};

    // Baseline pool override from setPool(); see §"ruleset overrides".
    bool     _poolOverridden      = false;
    int      _poolMax             = 0;
    uint16_t _poolRechargeDelayMs = 0;

    uint32_t _lastLitAt[PlayerDefs::MAX_PLAYER_ID] = {};

    int8_t  findSlot(uint8_t id) const;
    uint8_t maxOwned() const;
    bool    available(uint8_t id) const;
    void    requestSelect(uint8_t slotIdx);
    void    applyPending();
    void    activate(uint8_t slotIdx);
    void    tickRecharge();
    void    evictOldest();
    void    registerDefs();
    void    cycle(int8_t dir);
    uint16_t rechargeDelayMs() const;
};

// The one instance.  Rulesets call projector.trigger() directly, matching
// the existing `extern Enlight* enlightPtr` idiom for the optical layer.
extern LightAir_ProjectorCtrl projector;

#endif // LIGHTAIR_PROJECTORCTRL_H
