#ifndef LIGHTAIR_TEST_LUA_H
#define LIGHTAIR_TEST_LUA_H

#include <ArduinoLog.h>
#include <AUnit.h>
#include <lua.hpp>

static lua_State *L;

#define SETUP_LUA() do {\
    Log.infoln("Initializing Lua VM...");\
    L = luaL_newstate();\
    if (!L){\
        Log.errorln("Failed to create Lua state");\
        failTestNow();\
    }\
    luaL_openlibs(L);\
    Log.infoln("Lua VM initialized (version %s)", LUA_VERSION);\
} while (0)

#define CLOSE_LUA() do {\
    if (L) {\
        lua_close(L);\
        L = NULL;\
    }\
} while (0)

#define DO_WITH_LUA(test_body) do {\
    SETUP_LUA();\
    test_body;\
    CLOSE_LUA();\
} while (0)


test(test_lua_version) {
    // extract lua version from lua script and see if it matches
    // he expected version "
    
    DO_WITH_LUA(
        const char *expected = LUA_VERSION;

        lua_getglobal(L, "_VERSION");
        assertTrue(lua_isstring(L, lua_gettop(L)));

        const char *actual = lua_tostring(L, lua_gettop(L));
        lua_pop(L, lua_gettop(L));
        assertEqual(expected, actual);
    );
}

// TODO: test Lua code call from C
// TODO: test C code call from Lua

#endif // LIGHTAIR_TEST_LUA_H