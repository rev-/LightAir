#include "LightAir_ProjectorCtrl.h"
#include "../enlight/Enlight.h"
#include "../ui/player/LightAir_UICtrl.h"
#include <Arduino.h>
#include <string.h>
#include "esp_log.h"

static const char* TAG = "Projector";

char           projectorName[ProjectorLimits::NAME_LEN] = "BASE";
int            projectorEnergy = 0;
const uint8_t* projectorIcon   = ICON_ENERGY_BITMAP;

LightAir_ProjectorCtrl projector;

/* ============================================================
 *   begin() / setGame()
 * ============================================================ */
void LightAir_ProjectorCtrl::begin(Enlight* enlight, LightAir_UICtrl* ui) {
    _enlight = enlight;
    _ui      = ui;
}

void LightAir_ProjectorCtrl::setGame(const ProjectorSet* set) {
    _set = set;
    registerDefs();

    memset(_slots, 0, sizeof(_slots));
    memset(_lastLitAt, 0, sizeof(_lastLitAt));
    _slotCount        = 1;
    _activeIdx        = 0;
    _pendingIdx       = -1;
    _pendingDrop      = false;
    _readyAt          = 0;
    _evicted          = false;
    _poolOverridden   = false;

    _slots[0].id         = ProjectorId::BASE;
    _slots[0].acquiredAt = millis();
    _slots[0].lastShotAt = millis();
    _slots[0].energy     = (int16_t)_defs[ProjectorId::BASE].maxEnergy;

    activate(0);
    _changed = false;   // the initial baseline is not a "change" to announce
}

// Resolve every id this game can produce into a clamped definition.
// The same projectorClamp() runs over the standard table and the ruleset's,
// so bounds cannot drift between the two paths.
void LightAir_ProjectorCtrl::registerDefs() {
    memset(_defined, 0, sizeof(_defined));

    for (uint8_t i = 0; i < ProjectorId::STD_COUNT; i++) {
        _defs[i]    = projectorClamp(ProjectorDefs::kStandard[i]);
        _defined[i] = true;
    }

    if (!_set) return;

    if (_set->baseOverride) {
        const Projector& b = *_set->baseOverride;
        if (b.id != ProjectorId::BASE) {
            ESP_LOGE(TAG, "baseOverride id=%u is not BASE - ignored", b.id);
        } else if (b.recharge == Recharge::CONSUMED) {
            ESP_LOGE(TAG, "baseOverride is CONSUMED - the baseline is undroppable - ignored");
        } else {
            _defs[ProjectorId::BASE] = projectorClamp(b);
        }
    }

    const uint8_t n = (_set->customCount < ProjectorLimits::MAX_CUSTOM)
                    ? _set->customCount : ProjectorLimits::MAX_CUSTOM;
    for (uint8_t i = 0; i < n && _set->custom; i++) {
        const Projector& c = _set->custom[i];
        if (c.id < ProjectorId::CUSTOM1 || c.id >= ProjectorId::COUNT) {
            ESP_LOGE(TAG, "custom projector id=%u out of range - ignored", c.id);
            continue;
        }
        _defs[c.id]    = projectorClamp(c);
        _defined[c.id] = true;
    }
}

/* ============================================================
 *   inventory helpers
 * ============================================================ */
int8_t LightAir_ProjectorCtrl::findSlot(uint8_t id) const {
    for (uint8_t i = 0; i < _slotCount; i++)
        if (_slots[i].id == id) return (int8_t)i;
    return -1;
}

uint8_t LightAir_ProjectorCtrl::ownedId(uint8_t i) const {
    return (i < _slotCount) ? _slots[i].id : ProjectorId::BASE;
}

uint8_t LightAir_ProjectorCtrl::maxOwned() const {
    int v = (_set && _set->maxOwned) ? *_set->maxOwned
                                     : ProjectorLimits::DEFAULT_MAX_OWNED;
    if (v < ProjectorLimits::MIN_OWNED) v = ProjectorLimits::MIN_OWNED;
    if (v > ProjectorLimits::MAX_OWNED) v = ProjectorLimits::MAX_OWNED;
    return (uint8_t)v;
}

// The baseline is never consulted: it is the fallback, so a baseline that
// could report itself unavailable would leave the player unable to shine.
bool LightAir_ProjectorCtrl::available(uint8_t id) const {
    if (id == ProjectorId::BASE) return true;
    if (!_set || !_set->isAvailable) return true;
    return _set->isAvailable(id);
}

int LightAir_ProjectorCtrl::maxEnergy() const {
    if (_activeIdx == 0 && _poolOverridden) return _poolMax;
    return (int)active().maxEnergy;
}

uint16_t LightAir_ProjectorCtrl::rechargeDelayMs() const {
    if (_activeIdx == 0 && _poolOverridden) return _poolRechargeDelayMs;
    return active().rechargeDelayMs;
}

void LightAir_ProjectorCtrl::setPool(int maxEnergyValue, uint16_t rechargeDelay) {
    _poolOverridden      = true;
    _poolMax             = maxEnergyValue;
    _poolRechargeDelayMs = rechargeDelay;
    _slots[0].energy     = (int16_t)maxEnergyValue;
    if (_activeIdx == 0) projectorEnergy = maxEnergyValue;
}

/* ============================================================
 *   switching
 * ============================================================ */
void LightAir_ProjectorCtrl::requestSelect(uint8_t slotIdx) {
    if (slotIdx >= _slotCount) return;
    if (slotIdx == _activeIdx && _pendingIdx < 0) return;
    _pendingIdx = (int16_t)slotIdx;
    // Apply straight away when nothing is in flight; update() retries otherwise.
    if (!_enlight || !_enlight->isActive()) applyPending();
}

void LightAir_ProjectorCtrl::applyPending() {
    if (_pendingIdx < 0) return;
    if (_enlight && _enlight->isActive()) return;   // never reconfigure mid-run
    const uint8_t idx = (uint8_t)_pendingIdx;
    _pendingIdx = -1;
    if (idx < _slotCount) activate(idx);
}

void LightAir_ProjectorCtrl::activate(uint8_t slotIdx) {
    // projectorEnergy is the authority for the active slot, so bank it before
    // leaving; a ruleset may have written it directly since the last switch.
    if (slotIdx != _activeIdx) _slots[_activeIdx].energy = (int16_t)projectorEnergy;

    _activeIdx = slotIdx;
    const Projector& p = active();

    projectorEnergy = _slots[slotIdx].energy;
    projectorIcon   = p.icon ? p.icon : ICON_ENERGY_BITMAP;
    strncpy(projectorName, p.name ? p.name : "", ProjectorLimits::NAME_LEN - 1);
    projectorName[ProjectorLimits::NAME_LEN - 1] = '\0';

    if (_enlight) {
        _enlight->setRepetitions(p.cycles);
        _enlight->setCooldown(p.cooldownMs);
        _enlight->setRangeM(p.rangeM);
    }
    if (_ui) _ui->setEnlightAction(p.shotAction);

    _readyAt = millis() + p.readyMs;
    _changed = true;
}

bool LightAir_ProjectorCtrl::select(uint8_t id) {
    const int8_t idx = findSlot(id);
    if (idx < 0) {
        ESP_LOGW(TAG, "select(%u): not held", id);
        return false;
    }
    if (!available(id)) {
        ESP_LOGW(TAG, "select(%u): unavailable", id);
        return false;
    }
    requestSelect((uint8_t)idx);
    return true;
}

bool LightAir_ProjectorCtrl::give(uint8_t id) {
    if (id >= ProjectorId::COUNT || !_defined[id]) {
        ESP_LOGW(TAG, "give(%u): undefined", id);
        return false;
    }
    if (id == ProjectorId::BASE) return true;   // always held already
    // No ProjectorSet at all means "the standard baseline alone", so nothing is
    // grantable — the guard must reject on a null set, not skip past it.
    if (!_set || !(_set->catalogMask & (1u << id))) {
        ESP_LOGW(TAG, "give(%u): not in this game's catalogue", id);
        return false;
    }

    // Already held: refill, but keep acquiredAt so a re-grant cannot be used
    // to dodge eviction.  Refilling is what makes a re-grant of a CONSUMED
    // projector a restock.
    const int8_t held = findSlot(id);
    if (held >= 0) {
        _slots[held].energy = (int16_t)_defs[id].maxEnergy;
        if (held == _activeIdx) projectorEnergy = _defs[id].maxEnergy;
        return true;
    }

    if (_slotCount - 1 >= maxOwned()) evictOldest();
    if (_slotCount >= 1 + ProjectorLimits::MAX_OWNED) return false;

    Slot& s     = _slots[_slotCount];
    s.id         = id;
    s.energy     = (int16_t)_defs[id].maxEnergy;
    s.acquiredAt = millis();
    s.lastShotAt = millis();
    s.rampAt     = millis();
    _slotCount++;
    return true;
}

bool LightAir_ProjectorCtrl::grant(uint8_t id) {
    if (!give(id)) return false;
    return select(id);
}

bool LightAir_ProjectorCtrl::drop(uint8_t id) {
    const int8_t idx = findSlot(id);
    if (idx <= 0) return false;   // slot 0 is structural and never dropped
    const bool wasActive = ((uint8_t)idx == _activeIdx);

    for (uint8_t i = (uint8_t)idx; i + 1 < _slotCount; i++) _slots[i] = _slots[i + 1];
    _slotCount--;

    if (wasActive) {
        // Point at the baseline BEFORE activating so activate() skips its
        // usual "bank projectorEnergy into the outgoing slot" step: the
        // outgoing slot no longer exists and its energy went with it.
        _activeIdx = 0;
        activate(0);
    } else if ((uint8_t)idx < _activeIdx) {
        _activeIdx--;               // the compaction shifted us down
    }
    return true;
}

// FIFO over the powered slots only; slot 0 is not in the walk at all, so the
// baseline cannot be evicted and there is no exemption rule to forget.
void LightAir_ProjectorCtrl::evictOldest() {
    if (_slotCount <= 1) return;
    uint8_t oldest = 1;
    for (uint8_t i = 2; i < _slotCount; i++)
        if ((int32_t)(_slots[i].acquiredAt - _slots[oldest].acquiredAt) < 0) oldest = i;

    const char* gone = _defs[_slots[oldest].id].name;
    strncpy(_evictedName, gone ? gone : "?", ProjectorLimits::NAME_LEN - 1);
    _evictedName[ProjectorLimits::NAME_LEN - 1] = '\0';
    _evicted = true;
    drop(_slots[oldest].id);
}

void LightAir_ProjectorCtrl::cycle(int8_t dir) {
    if (_slotCount <= 1) return;
    for (uint8_t step = 1; step < _slotCount; step++) {
        int16_t idx = (int16_t)_activeIdx + (int16_t)(dir * (int8_t)step);
        while (idx < 0)                  idx += _slotCount;
        while (idx >= (int16_t)_slotCount) idx -= _slotCount;
        if (available(_slots[idx].id)) { requestSelect((uint8_t)idx); return; }
    }
}

void LightAir_ProjectorCtrl::next() { cycle(+1); }
void LightAir_ProjectorCtrl::prev() { cycle(-1); }

/* ============================================================
 *   the shot
 * ============================================================ */
bool LightAir_ProjectorCtrl::trigger() {
    if (!_enlight) return false;
    if ((int32_t)(millis() - _readyAt) < 0) return false;   // still deploying

    const Projector& p = active();
    if (p.energyCost > 0 && projectorEnergy < (int)p.energyCost) return false;

    // Energy is spent only if the run actually started — Enlight refuses while
    // still in cooldown, and a refused shot has never cost anything.
    if (!_enlight->run()) return false;

    if (p.energyCost > 0) projectorEnergy -= p.energyCost;
    _slots[_activeIdx].lastShotAt = millis();
    _slots[_activeIdx].rampAt     = millis() + rechargeDelayMs();

    if (_ui) _ui->triggerEnlight((uint16_t)_enlight->cycleTime());

    // A spent CONSUMED projector leaves, but not now: the run is in flight and
    // switching would reconfigure Enlight mid-measurement.  update() does it.
    if (p.recharge == Recharge::CONSUMED && projectorEnergy <= 0 && _activeIdx != 0)
        _pendingDrop = true;

    return true;
}

/* ============================================================
 *   anti-spam (attacker side)
 * ============================================================ */
bool LightAir_ProjectorCtrl::mayLight(uint8_t targetId) const {
    if (targetId >= PlayerDefs::MAX_PLAYER_ID) return true;
    const uint16_t window = active().targetImmunityMs;
    if (window == 0) return true;
    if (_lastLitAt[targetId] == 0) return true;
    return (uint32_t)(millis() - _lastLitAt[targetId]) >= window;
}

void LightAir_ProjectorCtrl::noteLit(uint8_t targetId) {
    if (targetId < PlayerDefs::MAX_PLAYER_ID) _lastLitAt[targetId] = millis();
}

/* ============================================================
 *   per-cycle
 * ============================================================ */
void LightAir_ProjectorCtrl::update() {
    if (_pendingDrop && (!_enlight || !_enlight->isActive())) {
        _pendingDrop = false;
        drop(activeId());
    }

    applyPending();

    // A projector that just became unavailable hands back to the baseline.
    if (_activeIdx != 0 && !available(activeId())) requestSelect(0);

    tickRecharge();
}

void LightAir_ProjectorCtrl::apply(const ProjectorOutput& out) {
    for (uint8_t i = 0; i < out.count; i++) {
        switch (out.msgs[i].op) {
            case ProjOutMsg::SELECT: select(out.msgs[i].id); break;
            case ProjOutMsg::GIVE:   give  (out.msgs[i].id); break;
            case ProjOutMsg::GRANT:  grant (out.msgs[i].id); break;
            case ProjOutMsg::DROP:   drop  (out.msgs[i].id); break;
            case ProjOutMsg::NEXT:   next(); break;
            case ProjOutMsg::PREV:   prev(); break;
        }
    }
}

// Only the projector in hand refills.  Time spent in the inventory is dead
// time, which is what makes carrying several a decision rather than a way to
// shine continuously by cycling.
void LightAir_ProjectorCtrl::tickRecharge() {
    const Projector& p = active();
    if (p.recharge == Recharge::NONE || p.recharge == Recharge::CONSUMED) return;

    const int max = maxEnergy();
    if (max <= 0 || projectorEnergy >= max) return;

    Slot& s = _slots[_activeIdx];
    const uint32_t now = millis();
    if ((uint32_t)(now - s.lastShotAt) < rechargeDelayMs()) return;

    if (p.recharge == Recharge::REFILL) {
        projectorEnergy = max;
        return;
    }

    // RAMP: one unit every rechargeMs / maxEnergy, stepped in integers.
    uint16_t stepMs = (uint16_t)(p.rechargeMs / (uint16_t)max);
    if (stepMs == 0) stepMs = 1;
    while ((int32_t)(now - s.rampAt) >= 0 && projectorEnergy < max) {
        projectorEnergy++;
        s.rampAt += stepMs;
    }
    if (projectorEnergy >= max) s.rampAt = now;
}

/* ============================================================
 *   flags
 * ============================================================ */
bool LightAir_ProjectorCtrl::consumeChanged() {
    const bool v = _changed;
    _changed = false;
    return v;
}

bool LightAir_ProjectorCtrl::consumeEvicted() {
    const bool v = _evicted;
    _evicted = false;
    return v;
}
