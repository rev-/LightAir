# Vendored Lua core

- **Version**: Lua 5.5.1 (`LUA_VERSION_RELEASE_NUM` 50501), unmodified core
  sources. The directory keeps the `lua-5.5.0` name the Makefile include
  path (`-Isrc/libs/lua-5.5.0/src`) already used.
- **Origin**: the `third-party/lua55` tree bundled in the `lupa` 2.8 source
  distribution on PyPI (lua.org was unreachable from the build environment;
  the tree is the pristine upstream source).
- **Removed**: `lua.c` (standalone interpreter `main`), `onelua.c`
  (single-file build wrapper), `ltests.{c,h}` (internal test scaffolding).
  One functional patch: `luaconf.h` force-enables `LUA_32BITS` so the C
  core and the C++ binding can never disagree on the number ABI regardless
  of per-language compiler flags.  Nothing else was touched.
- **Build**: compiled as plain C by the Arduino build alongside the library
  sources, with `-DLUA_32BITS` (32-bit `lua_Integer`/float — matches the
  game var slots). Whole core compiles clean with `-Wall`.
