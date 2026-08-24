#ifndef LIGHTAIR_TEST_PROJECTOR_H
#define LIGHTAIR_TEST_PROJECTOR_H

#include <Arduino.h>
#include <AUnit.h>
#include <game/LightAir_Projector.h>
#include <game/LightAir_ProjectorCtrl.h>

// ----------------------------------------------------------------
// Projector tests.
//
// LightAir_ProjectorCtrl guards every Enlight and UICtrl call, so the
// inventory and energy rules can be exercised with begin(nullptr, nullptr)
// on a bare board.  Time-dependent behaviour (readyMs, the refill tick)
// is deliberately left to the host harness rather than padded out with
// real delays here.
// ----------------------------------------------------------------

namespace ProjectorTest {

static int  maxOwned = 2;
static int  lives    = 3;

static bool available(uint8_t id) {
    return !(id == ProjectorId::STRONG && lives <= 1);
}

static const Projector oneShot = {
    ProjectorId::CUSTOM1, "ONESHOT", 12, 300, 10,
    Recharge::CONSUMED, 1, 2, 0, 0,
    3, 7, 500, 200, nullptr, nullptr
};

static const ProjectorSet set = {
    &oneShot, 1,
    (1u << ProjectorId::FAST) | (1u << ProjectorId::STRONG) | (1u << ProjectorId::CUSTOM1),
    nullptr, &maxOwned, available
};

} // namespace ProjectorTest

// ---- clamping ----

test(projector_clamp_bounds_every_field) {
    Projector wild = { ProjectorId::CUSTOM2, "WILD", 9999, 60000, 250,
                       Recharge::RAMP, 99, 250, 65000, 65000, 99, 0, 60000, 60000,
                       nullptr, nullptr };
    const Projector c = projectorClamp(wild);
    assertEqual((int)c.cycles,    (int)ProjectorLimits::MAX_CYCLES);
    assertEqual((int)c.strength,  (int)ProjectorLimits::MAX_STRENGTH);
    assertEqual((int)c.rangeM,    (int)ProjectorLimits::MAX_RANGE_M);
    assertEqual((int)c.readyMs,   (int)ProjectorLimits::MAX_READY_MS);
    assertEqual((int)c.maxEnergy, (int)ProjectorLimits::MAX_ENERGY);
}

test(projector_clamp_keeps_zero_range_as_device_max) {
    Projector p = ProjectorDefs::kStandard[ProjectorId::BASE];
    p.rangeM = 0;
    assertEqual((int)projectorClamp(p).rangeM, 0);
}

// ---- the standard table reproduces today's behaviour ----

test(projector_base_is_todays_behaviour) {
    const Projector& b = ProjectorDefs::kStandard[ProjectorId::BASE];
    assertEqual((int)b.cycles, 10);      // the old Enlight member initialiser
    assertEqual((int)b.strength, 1);     // the implicit lives--
    assertEqual((int)b.rangeM, 0);       // classifier untouched
    assertEqual((int)b.cooldownMs, 10);  // the one deliberate change (was 0)
}

// ---- inventory ----

test(projector_defaults_to_baseline_alone) {
    projector.begin(nullptr, nullptr);
    projector.setGame(nullptr);
    assertEqual((int)projector.activeId(), (int)ProjectorId::BASE);
    assertEqual((int)projector.ownedCount(), 1);
    // Nothing is grantable when a ruleset declares no ProjectorSet.
    assertFalse(projector.give(ProjectorId::FAST));
}

test(projector_catalogue_gates_what_can_be_given) {
    projector.begin(nullptr, nullptr);
    projector.setGame(&ProjectorTest::set);
    assertTrue (projector.give(ProjectorId::FAST));
    assertFalse(projector.give(ProjectorId::LONG));   // not in the catalogue
}

test(projector_fifo_evicts_oldest_powered_never_the_baseline) {
    ProjectorTest::maxOwned = 2;
    projector.begin(nullptr, nullptr);
    projector.setGame(&ProjectorTest::set);

    projector.give(ProjectorId::FAST);   delay(5);
    projector.give(ProjectorId::STRONG); delay(5);
    assertEqual((int)projector.ownedCount(), 3);      // baseline + 2

    projector.give(ProjectorId::CUSTOM1);
    assertEqual((int)projector.ownedCount(), 3);      // still baseline + 2
    assertFalse(projector.owns(ProjectorId::FAST));   // oldest powered went
    assertTrue (projector.owns(ProjectorId::BASE));   // baseline never does
    assertTrue (projector.consumeEvicted());
}

test(projector_each_profile_keeps_its_own_energy) {
    ProjectorTest::maxOwned = 3;
    projector.begin(nullptr, nullptr);
    projector.setGame(&ProjectorTest::set);
    projector.give(ProjectorId::STRONG);
    projector.give(ProjectorId::CUSTOM1);

    projector.select(ProjectorId::STRONG);
    const int strongFull = projectorEnergy;
    projectorEnergy -= 3;                              // as a shot would

    projector.select(ProjectorId::CUSTOM1);
    assertEqual(projectorEnergy, (int)ProjectorTest::oneShot.maxEnergy);

    projector.select(ProjectorId::STRONG);
    assertEqual(projectorEnergy, strongFull - 3);      // banked and restored
}

test(projector_unavailable_profile_hands_back_to_baseline) {
    ProjectorTest::maxOwned = 3;
    ProjectorTest::lives    = 3;
    projector.begin(nullptr, nullptr);
    projector.setGame(&ProjectorTest::set);
    projector.grant(ProjectorId::STRONG);
    assertEqual((int)projector.activeId(), (int)ProjectorId::STRONG);

    ProjectorTest::lives = 1;
    projector.update();
    assertEqual((int)projector.activeId(), (int)ProjectorId::BASE);
    assertFalse(projector.select(ProjectorId::STRONG));

    ProjectorTest::lives = 3;
    assertTrue(projector.select(ProjectorId::STRONG));
}

test(projector_setpool_overrides_the_baseline_pool) {
    projector.begin(nullptr, nullptr);
    projector.setGame(nullptr);
    projector.setPool(150, 0);
    assertEqual(projectorEnergy, 150);
}

// ---- baseOverride ----

test(projector_rejects_an_invalid_base_override) {
    // Wrong id: the override is ignored rather than corrupting the baseline.
    static const Projector badId = { ProjectorId::CUSTOM3, "BAD", 5, 5, 0,
                                     Recharge::REFILL, 1, 1, 0, 0, 1, 0, 0, 0,
                                     nullptr, nullptr };
    static const ProjectorSet badIdSet = { nullptr, 0, 0, &badId, nullptr, nullptr };
    projector.begin(nullptr, nullptr);
    projector.setGame(&badIdSet);
    assertEqual((int)projector.activeId(), (int)ProjectorId::BASE);
    assertEqual((int)projector.active().cycles, 10);   // still the standard BASE

    // CONSUMED is a contradiction for an undroppable baseline.
    static const Projector badMode = { ProjectorId::BASE, "BAD", 5, 5, 0,
                                       Recharge::CONSUMED, 1, 1, 0, 0, 1, 0, 0, 0,
                                       nullptr, nullptr };
    static const ProjectorSet badModeSet = { nullptr, 0, 0, &badMode, nullptr, nullptr };
    projector.setGame(&badModeSet);
    assertEqual((int)projector.active().cycles, 10);
}

test(projector_valid_base_override_is_applied) {
    static const Projector tuned = { ProjectorId::BASE, "BASE", 20, 20, 0,
                                     Recharge::NONE, 1, 100, 0, 0, 1, 0, 0, 0,
                                     nullptr, nullptr };
    static const ProjectorSet tunedSet = { nullptr, 0, 0, &tuned, nullptr, nullptr };
    projector.begin(nullptr, nullptr);
    projector.setGame(&tunedSet);
    assertEqual((int)projector.active().cycles, 20);
    assertEqual((int)projector.active().cooldownMs, 20);
    assertEqual((int)projector.activeId(), (int)ProjectorId::BASE);
}

// ---- anti-spam ----

test(projector_target_immunity_is_per_target) {
    projector.begin(nullptr, nullptr);
    projector.setGame(nullptr);          // BASE: 3000 ms window
    assertTrue(projector.mayLight(4));
    projector.noteLit(4);
    assertFalse(projector.mayLight(4));
    assertTrue (projector.mayLight(5));  // a different target is unaffected
}

#endif // LIGHTAIR_TEST_PROJECTOR_H
