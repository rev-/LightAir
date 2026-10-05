#include "LightAir_LuaGameInternal.h"

// ----------------------------------------------------------------
// stripLuaDebug — see LightAir_LuaGameInternal.h.
//
// Needs the core's own view of a function prototype, which the public
// API does not expose; the freeing mirrors luaF_freeproto.  A file of
// its own because the core's internal headers define short global
// macros (G, cast, UNUSED, ...) that must not leak into the binding.
// ----------------------------------------------------------------
extern "C" {
#include "../libs/lua-5.5.0/src/lstate.h"
#include "../libs/lua-5.5.0/src/lfunc.h"
#include "../libs/lua-5.5.0/src/lmem.h"
}

static void stripProto(lua_State* L, Proto* f) {
    // Line tables in fixed memory belong to an undumped chunk, which
    // nothing here loads; luaF_freeproto leaves them alone too.
    if (!(f->flag & PF_FIXED)) {
        luaM_freearray(L, f->lineinfo,    (size_t)f->sizelineinfo);
        luaM_freearray(L, f->abslineinfo, (size_t)f->sizeabslineinfo);
    }
    f->lineinfo    = nullptr;  f->sizelineinfo    = 0;
    f->abslineinfo = nullptr;  f->sizeabslineinfo = 0;
    luaM_freearray(L, f->locvars, (size_t)f->sizelocvars);
    f->locvars = nullptr;  f->sizelocvars = 0;
    // The name strings themselves become garbage once nothing points
    // at them; the descriptors stay, as the closures need them.
    for (int i = 0; i < f->sizeupvalues; i++) f->upvalues[i].name = nullptr;
    for (int i = 0; i < f->sizep; i++) stripProto(L, f->p[i]);
}

void stripLuaDebug(lua_State* L) {
    const TValue* top = s2v(L->top.p - 1);
    if (ttisLclosure(top)) stripProto(L, clLvalue(top)->p);
}
