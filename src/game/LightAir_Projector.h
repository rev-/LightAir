#ifndef LIGHTAIR_PROJECTOR_H
#define LIGHTAIR_PROJECTOR_H

#include <stdint.h>
#include "../config.h"
#include "../ui/player/LightAir_UIAction.h"
#include "../ui/player/display/LightAir_Display_Icons.h"

// ================================================================
// Projector — the profile of the light-beam device a player carries.
//
// One value type gathering what used to be hardcoded in Enlight member
// initialisers and set imperatively by individual rulesets.  Its fields
// serve three different consumers, and keeping that split visible is what
// stops the Projector from becoming a god-object:
//
//   optical   cycles, cooldownMs, rangeM   -> pushed into Enlight
//   game      the shot economy, strength   -> LightAir_ProjectorCtrl + rules
//   identity  name, icon, shotAction       -> the UI layer
//
// Enlight never sees this type: LightAir_ProjectorCtrl decomposes the
// optical fields into the three Enlight setters.
//
// A projector definition is SELF-CONTAINED — it carries its own display
// bitmap and its own shot feedback — so adding one means adding a row to a
// table and the artwork beside it, with no edits anywhere else.
// ================================================================

// ----------------------------------------------------------------
// Recharge — how (and whether) a projector's energy pool refills.
//
// Named rather than encoded in the timing fields: once "never refills"
// and "consumed when spent" are in scope, sentinel values (0 = instant,
// 0xFFFF = never) become magic numbers nobody remembers.
//
// A clean 2x2: two modes refill, differing in shape; two do not,
// differing in what happens at zero.
// ----------------------------------------------------------------
enum class Recharge : uint8_t {
    REFILL = 0,  // after rechargeDelayMs idle, jump straight to maxEnergy
    RAMP,        // after rechargeDelayMs idle, +1 every rechargeMs / maxEnergy
    NONE,        // never refills; stays in hand when empty (what zero means
                 //   is then the ruleset's business — see GameOutflow)
    CONSUMED,    // never refills; the projector is DROPPED when it reaches 0
};

struct Projector {
    uint8_t     id;               // ProjectorId value
    const char* name;             // <= ProjectorLimits::NAME_LEN-1 chars

    // ---- optical: pushed into Enlight on every switch ----
    uint16_t    cycles;           // Enlight::setRepetitions(); burst = cycles * MS_PER_REP
    uint16_t    cooldownMs;       // Enlight::setCooldown(); also the fire-rate limiter
    uint8_t     rangeM;           // approximate reach in metres; 0 = whatever the device sees

    // ---- game: the shot economy ----
    Recharge    recharge;
    uint8_t     energyCost;       // energy per shot; 0 = free
    uint8_t     maxEnergy;        // pool when full, or the number of charges
    uint16_t    rechargeDelayMs;  // idle time before refilling starts
    uint16_t    rechargeMs;       // empty -> full duration (RAMP only)

    // ---- game: the effect, sent on the wire ----
    uint8_t     strength;         // weight in STANDARD HITS, not health points:
                                  //   each ruleset decides what one standard hit
                                  //   costs in its own currency (1 life, or hitDmg
                                  //   energy).  BASE = 1, STRONG = 3.
    uint8_t     roleTag;          // 0 = generic; the holder's role for a per-game table
    uint16_t    targetImmunityMs; // min gap between two hits on the SAME target,
                                  //   enforced by the attacker (see ProjectorCtrl)

    // ---- game: handling and feedback ----
    uint16_t    readyMs;          // deploy time after a switch before the first shot
    const LightAir_UIAction* shotAction;  // nullptr = the standard Enlight action
    const uint8_t*           icon;        // 8x8 PROGMEM bitmap; nullptr = ICON_ENERGY
};

// ----------------------------------------------------------------
// ProjectorId — plain uint8_t so one value serves as array index, menu
// selection, radio payload byte and totem reward id with no conversions.
//
// BASE is structural: it is always held, never counted against maxOwned,
// and never evicted.  A game whose baseline needs different *values*
// retunes them with ProjectorSet::baseOverride rather than naming a
// different id.
// ----------------------------------------------------------------
namespace ProjectorId {
    constexpr uint8_t BASE      = 0;
    constexpr uint8_t FAST      = 1;
    constexpr uint8_t LONG      = 2;
    constexpr uint8_t STRONG    = 3;
    constexpr uint8_t STD_COUNT = 4;
    constexpr uint8_t CUSTOM1   = 4;
    constexpr uint8_t CUSTOM2   = 5;
    constexpr uint8_t CUSTOM3   = 6;
    constexpr uint8_t CUSTOM4   = 7;
    constexpr uint8_t COUNT     = STD_COUNT + ProjectorLimits::MAX_CUSTOM;  // 8
}

// ----------------------------------------------------------------
// projectorClamp — the single definition of the bounds logic.
//
// C++11-safe constexpr (one return statement), so the same function
// serves compile-time checks on the standard table and the runtime clamp
// applied to ruleset-local tables at registration.  There is no second
// place bounds can drift to.
// ----------------------------------------------------------------
template <typename T>
constexpr T projectorClampValue(T v, T lo, T hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

constexpr Projector projectorClamp(const Projector& p) {
    return Projector{
        p.id, p.name,
        projectorClampValue<uint16_t>(p.cycles,
            ProjectorLimits::MIN_CYCLES, ProjectorLimits::MAX_CYCLES),
        projectorClampValue<uint16_t>(p.cooldownMs,
            ProjectorLimits::MIN_COOLDOWN_MS, ProjectorLimits::MAX_COOLDOWN_MS),
        // rangeM 0 is "device max" and must survive the clamp untouched.
        (p.rangeM == 0) ? (uint8_t)0
                        : projectorClampValue<uint8_t>(p.rangeM,
                              ProjectorLimits::MIN_RANGE_M, ProjectorLimits::MAX_RANGE_M),
        p.recharge,
        projectorClampValue<uint8_t>(p.energyCost, 0, ProjectorLimits::MAX_ENERGY_COST),
        projectorClampValue<uint8_t>(p.maxEnergy, 0, ProjectorLimits::MAX_ENERGY),
        projectorClampValue<uint16_t>(p.rechargeDelayMs, 0, ProjectorLimits::MAX_RECHARGE_MS),
        projectorClampValue<uint16_t>(p.rechargeMs, 0, ProjectorLimits::MAX_RECHARGE_MS),
        projectorClampValue<uint8_t>(p.strength,
            ProjectorLimits::MIN_STRENGTH, ProjectorLimits::MAX_STRENGTH),
        p.roleTag,
        projectorClampValue<uint16_t>(p.targetImmunityMs, 0, ProjectorLimits::MAX_IMMUNITY_MS),
        projectorClampValue<uint16_t>(p.readyMs, 0, ProjectorLimits::MAX_READY_MS),
        p.shotAction,
        p.icon,
    };
}

// ----------------------------------------------------------------
// ProjectorSet — a ruleset's whole projector declaration.
//
// One pointer on LightAir_Game; nullptr means "standard BASE only",
// which is exactly today's behaviour for a ruleset that says nothing.
// ----------------------------------------------------------------
struct ProjectorSet {
    const Projector* custom;        // ruleset-local powered profiles; may be nullptr
    uint8_t          customCount;   // <= ProjectorLimits::MAX_CUSTOM

    // Bit i set = powered ProjectorId i exists in this game.  The baseline is
    // structural and needs no bit.  Doubles as the picker list, the set a totem
    // or quest may grant, and the guard on any runtime select().
    uint16_t         catalogMask;

    // This game's baseline values, retuned in place.  nullptr = the standard
    // BASE.  Must keep id == ProjectorId::BASE and must not be CONSUMED — the
    // baseline is undroppable, so "delete me at zero" is a contradiction.
    const Projector* baseOverride;

    // Powered slots the player may hold at once, as a pointer to the ruleset's
    // own config var (the LightAir_Game::gameTimeLeft idiom).  nullptr =
    // ProjectorLimits::DEFAULT_MAX_OWNED.
    const int*       maxOwned;

    // Per-cycle availability predicate, e.g. "STRONG is gone at one life".
    // Never consulted for the baseline: it is the fallback, so a baseline that
    // could report itself unavailable would leave the player unable to shine.
    // nullptr = everything always available.
    bool (*isAvailable)(uint8_t projectorId);
};

// ================================================================
// Standard projectors and their artwork.
//
// Globally available to every ruleset with no declaration, exactly as
// LightAir_UICtrl's standard action table is.
// ================================================================
namespace ProjectorDefs {

// ---- 8x8 icons, LSB-first (XBM-compatible), same format as Display_Icons.h ----

// FAST — a forward chevron pair: quick, light, short.
static const uint8_t ICON_PROJ_FAST[8] PROGMEM = {
    0b00100010,
    0b01000100,
    0b10001000,
    0b01000100,
    0b00100010,
    0b00000000,
    0b00000000,
    0b00000000
};

// LONG — a beam narrowing to a distant point.
static const uint8_t ICON_PROJ_LONG[8] PROGMEM = {
    0b00000000,
    0b11000000,
    0b01110000,
    0b00111110,
    0b01110000,
    0b11000000,
    0b00000000,
    0b00000000
};

// STRONG — a filled burst.
static const uint8_t ICON_PROJ_STRONG[8] PROGMEM = {
    0b00011000,
    0b00111100,
    0b01111110,
    0b11111111,
    0b11111111,
    0b01111110,
    0b00111100,
    0b00011000
};

// ---- Shot feedback, installed into the Enlight slot on switch ----
// Step durations are overridden at runtime with the real burst length
// (see LightAir_UICtrl::executeStep), so durations[0] is only a fallback.

constexpr LightAir_UIAction ACTION_FAST = {
    {10,0,0,0}, 1, {3000,0,0,0}, {90,0,0,0},
    { {0,180,255},{0,0,0},{0,0,0},{0,0,0} }, 1
};

constexpr LightAir_UIAction ACTION_LONG = {
    {10,0,0,0}, 1, {1500,0,0,0}, {150,0,0,0},
    { {180,0,255},{0,0,0},{0,0,0},{0,0,0} }, 1
};

constexpr LightAir_UIAction ACTION_STRONG = {
    {10,0,0,0}, 1, {900,0,0,0}, {255,0,0,0},
    { {255,80,0},{0,0,0},{0,0,0},{0,0,0} }, 1
};

// ---- The table ----
//
// BASE reproduces today's behaviour: cycles 10 is the current Enlight member
// initialiser, strength 1 is the implicit lives--, rangeM 0 leaves the
// classifier comparison exactly as it is, and REFILL 50 / 10000 ms is the
// startEnergy = 50 / rechargeSecs = 10 that five rulesets already declare.
// cooldownMs = 10 is the one deliberate change (it was 0).
//
// FAST's short reach is not an arbitrary nerf: with 4 cycles the integration
// gain is genuinely lower, so gating it keeps the profile honest instead of
// letting it produce unreliable long-range hits.

constexpr Projector kStandard[ProjectorId::STD_COUNT] = {
    // id                  name      cyc  cool  rng | recharge          cost pool  delay  ramp | str role immun | ready action        icon
    { ProjectorId::BASE,   "BASE",    10,   10,   0,  Recharge::REFILL,   1,  50, 10000,     0,   1,  0, 3000,      0, nullptr,       nullptr },
    { ProjectorId::FAST,   "FAST",     4,   60,   8,  Recharge::RAMP,     1,  30,  1500,  3000,   1,  0, 1200,    150, &ACTION_FAST,  ICON_PROJ_FAST },
    { ProjectorId::LONG,   "LONG",    30,  400,   0,  Recharge::REFILL,   1,  20,  4000,     0,   1,  0, 3000,    400, &ACTION_LONG,  ICON_PROJ_LONG },
    { ProjectorId::STRONG, "STRONG",  15,  900,  15,  Recharge::REFILL,   2,   8,  6000,     0,   3,  0, 6000,    600, &ACTION_STRONG,ICON_PROJ_STRONG },
};

// The standard table must already satisfy the limits: if a clamp would bite
// here it is a mistake in the table, not a value to silently correct.
static_assert(kStandard[ProjectorId::BASE].id   == ProjectorId::BASE,   "BASE id");
static_assert(kStandard[ProjectorId::FAST].id   == ProjectorId::FAST,   "FAST id");
static_assert(kStandard[ProjectorId::LONG].id   == ProjectorId::LONG,   "LONG id");
static_assert(kStandard[ProjectorId::STRONG].id == ProjectorId::STRONG, "STRONG id");
static_assert(kStandard[ProjectorId::BASE].recharge != Recharge::CONSUMED,
              "the baseline is undroppable; CONSUMED would delete it at zero");
static_assert(kStandard[ProjectorId::BASE].cycles <= ProjectorLimits::MAX_CYCLES &&
              kStandard[ProjectorId::FAST].cycles <= ProjectorLimits::MAX_CYCLES &&
              kStandard[ProjectorId::LONG].cycles <= ProjectorLimits::MAX_CYCLES &&
              kStandard[ProjectorId::STRONG].cycles <= ProjectorLimits::MAX_CYCLES,
              "a standard projector exceeds MAX_CYCLES");
static_assert(kStandard[ProjectorId::STRONG].strength <= ProjectorLimits::MAX_STRENGTH,
              "a standard projector exceeds MAX_STRENGTH");

} // namespace ProjectorDefs

#endif // LIGHTAIR_PROJECTOR_H
