#pragma once
#include <stdint.h>
#include "../game/LightAir_GameOutput.h"
#include "../input/LightAir_InputTypes.h"

// ----------------------------------------------------------------
// Internal glue shared by the LightAir_LuaGame translation units.
//
// The binding is one class (LightAir_LuaGame) split across three
// .cpp files along its natural seams:
//
//   LightAir_LuaGame.cpp      loader / descriptor synthesis, the
//                             trampolines and runtime dispatch,
//                             fault accounting
//   LightAir_LuaKernel.cpp    the `la` verb table and the `vars` /
//                             `pkt` proxies (everything game files
//                             can call)
//   LightAir_TotemEncoder.cpp `totems` table → TotemVM program
//                             serializer (wire format:
//                             docs/totem-behavior-handshake.md)
//
// Nothing in this header is public API — include it only from those
// three files.
// ----------------------------------------------------------------

class LightAir_LuaGame;
class LightAir_DisplayCtrl;
class LightAir_UICtrl;
class LightAir_Radio;
class LightAir_GameRunner;

// ---- Callback context ------------------------------------------
// GameRunner drives games through plain function pointers, which
// cannot carry a `this`.  The trampolines (LightAir_LuaGame.cpp)
// stash the references the runner hands them here before dispatching
// into Lua, and the la.* verbs read them back.  Exactly one Lua game
// is active at a time, so a single context is enough.  Every pointer
// is valid only for the duration of one callback (`radio`, `ui` and
// `runner` live from doBegin() to the end of the match).
struct LuaGameContext {
    LightAir_LuaGame*          active;   // game whose callbacks may run
    GameOutput*                out;      // queued outputs, flushed by the runner
                                         // in its OUTPUT phase; null in callbacks
                                         // that must write directly (begin/score/end)
    LightAir_DisplayCtrl*      disp;
    LightAir_UICtrl*           ui;       // may be null (runner started without UI)
    LightAir_Radio*            radio;
    const LightAir_GameRunner* runner;
    const InputReport*         inputs;   // current READ-phase snapshot

    // Backing packets of the reusable `pkt` proxies.
    // Index: 0 = incoming request, 1 = reply, 2 = original request.
    // Null outside the handler call — the proxy __index turns a
    // stale access into a Lua error instead of a wild read.
    const RadioPacket*         pkts[3];
    int8_t                     pktRssi[3];
};
extern LuaGameContext g_luaCtx;   // defined in LightAir_LuaGame.cpp

// ---- Small name→byte lookup tables ------------------------------
struct NamedU8 { const char* name; uint8_t val; };
int lookupName(const NamedU8* tab, uint8_t n, const char* name);  // -1 = not found
#define LOOKUP(tab, name) lookupName(tab, sizeof(tab) / sizeof(*tab), name)

// Totem role name ("BASE", "CP", …) → TotemRoleId constant; -1 if
// unknown.  Shared by the loader (totem_slots / totems keys) and the
// la.totem_for_role verb.
int lookupTotemRole(const char* name);
