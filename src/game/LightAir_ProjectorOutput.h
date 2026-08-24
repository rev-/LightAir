#ifndef LIGHTAIR_PROJECTOROUTPUT_H
#define LIGHTAIR_PROJECTOROUTPUT_H

#include <stdint.h>

// ----------------------------------------------------------------
// ProjectorOutput — queued projector requests, the third member of
// GameOutput alongside RadioOutput and UIOutput.
//
// Rules and behaviors call these during the LOGIC phase; GameRunner
// applies them to LightAir_ProjectorCtrl in the OUTPUT phase, once all
// game logic for the cycle is done.  Besides matching the three-phase
// loop, deferring the switch fixes a real hazard: reconfiguring Enlight
// while a measurement is in flight would corrupt it.
//
// Note the asymmetry with LightAir_ProjectorCtrl::trigger(), which is a
// DIRECT call: a shot must start on this tick, exactly as the existing
// enlightPtr->run() does, so queueing it would only add a loop of latency.
// Selection is queued; shining is immediate.
//
//   out.proj.grant(ProjectorId::STRONG);   // totem reward / quest unlock
//   out.proj.next();                       // keypad cycling
// ----------------------------------------------------------------

constexpr uint8_t PROJ_OUT_MAX = 4;

struct ProjOutMsg {
    enum Op : uint8_t {
        SELECT,   // switch to one already held
        GIVE,     // add at full energy (refill if already held), don't switch
        GRANT,    // GIVE + SELECT
        DROP,     // remove from the inventory
        NEXT,     // cycle forward through held + available
        PREV,     // cycle backward
    };
    Op      op;
    uint8_t id;   // unused for NEXT / PREV
};

struct ProjectorOutput {
    ProjOutMsg msgs[PROJ_OUT_MAX];
    uint8_t    count = 0;

    void select(uint8_t id) { push(ProjOutMsg::SELECT, id); }
    void give  (uint8_t id) { push(ProjOutMsg::GIVE,   id); }
    void grant (uint8_t id) { push(ProjOutMsg::GRANT,  id); }
    void drop  (uint8_t id) { push(ProjOutMsg::DROP,   id); }
    void next  ()           { push(ProjOutMsg::NEXT,   0);  }
    void prev  ()           { push(ProjOutMsg::PREV,   0);  }

private:
    void push(ProjOutMsg::Op op, uint8_t id) {
        if (count >= PROJ_OUT_MAX) return;
        msgs[count].op = op;
        msgs[count].id = id;
        count++;
    }
};

#endif // LIGHTAIR_PROJECTOROUTPUT_H
