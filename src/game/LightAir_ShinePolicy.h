#ifndef LIGHTAIR_SHINEPOLICY_H
#define LIGHTAIR_SHINEPOLICY_H

#include <stdint.h>

// ================================================================
// ShinePolicy — declares how a game turns the trigger into a beam,
// so LightAir_GameRunner can service shining and the ruleset does not
// have to write the loop at all.
//
// A survey of the six shipped rulesets found their shine loops identical
// in every dimension but one — whether the target is filtered — and found
// two of them subtly wrong for having been hand-written (energy was spent
// on runs Enlight had refused).  This table exists so that loop is written
// once, correctly.
//
// ---- Opting out ----
//
// LightAir_Game::shinePolicy == nullptr means the ruleset drives shining
// itself, exactly as before this existed.  That escape hatch is why the
// policy never needs to grow a field per exotic requirement: a game that
// does not fit simply does not declare one.
//
// ---- Who polls Enlight ----
//
// Enlight::poll() is READ-AND-CLEAR, so two callers would silently eat
// each other's hits.  The rule is mechanical, with no middle state:
//
//     policy declared  -> GameRunner polls; the ruleset must NOT.
//     policy absent    -> the ruleset polls, as it always did.
//
// ---- Ordering ----
//
// GameRunner services shining immediately BEFORE the state behavior runs,
// which is where the hand-written loops sat, so a converted ruleset sees
// the ordering it saw before.
// ================================================================

struct ShinePolicy {
    // Bit N set = shining is serviced while currentState == N.
    // 0 disables the policy entirely (and hands polling back to the ruleset),
    // so this field must be set explicitly — each game numbers its own states.
    uint32_t activeStates;

    // Button that starts a beam.  InputDefaults::TRIG_1_ID is 0, which is
    // also the natural default, so leaving this zero needs no sentinel.
    uint8_t  triggerButton;

    // Even msgType unicast to a detected player.  0 = RadioMsg::MSG_LIT.
    // Payload is [strength, projectorId, roleTag] — see LightAir_Projector.h.
    uint8_t  hitMsgType;

    // Optional tally of accepted beams, e.g. &energySpent.  nullptr = none.
    int*     shineCounter;

    // Optional target filter, e.g. "opponents only unless friendly fire is on".
    // nullptr = anyone detected is signalled.
    //
    // Distinct from LightAir_ProjectorCtrl::mayLight(), which is the active
    // projector's per-target anti-spam window: that one is about timing and is
    // always applied, this one is about who counts as a valid target.
    bool   (*isValidTarget)(uint8_t targetId);
};

#endif // LIGHTAIR_SHINEPOLICY_H
