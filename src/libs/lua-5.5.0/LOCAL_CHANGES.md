# Divergences from upstream Lua

This tree is Lua **5.5.1** (kept under the directory name `lua-5.5.0`
that the Makefile include path already used).  Provenance and build
notes are in `README.md` next to this file.

Divergences from the pristine upstream source, in full:

1. **Removed files** (never referenced by the firmware build):
   `lua.c` (standalone interpreter `main`), `onelua.c` (single-file
   build wrapper), `ltests.c` / `ltests.h` (internal test
   scaffolding).

2. **`src/luaconf.h`** — one functional patch: `LUA_32BITS` is
   force-defined (`#define LUA_32BITS 1` when not already defined),
   so the C core can never disagree with the C++ binding about the
   32-bit integer/float ABI, regardless of per-language compiler
   flags.  The Makefile and test/host Makefile still pass
   `-DLUA_32BITS` explicitly; the patch is the belt to that brace.

Nothing else was modified.  When upgrading the vendored copy, re-apply
item 2 and re-remove the files in item 1, then update both this file
and `README.md`.
