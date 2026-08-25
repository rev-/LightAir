# No -I flags: every in-tree #include is written relative to the file that
# issues it, so the same sources also compile in the Arduino IDE, which
# has no way to pass extra include paths.  Keep new includes relative.
FLAGS=-Wall -Wextra
DEFINES=-DLUA_32BITS
ADDITIONAL_DEFINES?=
SKETCH_NAME?=LightAir
# Leave blank for default profile
PROFILE?=

# GNU make wildcards don't recurse ('**' behaves like '*'); list depths
# explicitly so edits everywhere (incl. the vendored Lua core and the
# embedded games) trigger a rebuild.
SRCS = $(wildcard *.ino) \
       $(wildcard src/*.cpp) $(wildcard src/*.h) \
       $(wildcard src/*/*.cpp) $(wildcard src/*/*.h) $(wildcard src/*/*.c) \
       $(wildcard src/*/*/*.cpp) $(wildcard src/*/*/*.h) \
       $(wildcard src/libs/lua-5.5.0/src/*.c) $(wildcard src/libs/lua-5.5.0/src/*.h)

# The vendored C core needs the same defines as the C++ binding
# (LUA_32BITS is additionally forced inside luaconf.h as a belt).
CPROPS = --build-property "compiler.cpp.extra_flags=$(FLAGS) $(DEFINES) $(ADDITIONAL_DEFINES)" \
         --build-property "compiler.c.extra_flags=$(DEFINES) $(ADDITIONAL_DEFINES)"

build/debug/$(SKETCH_NAME).ino.bin: $(SRCS) src/lua/LightAir_GamesBundle.h
	arduino-cli compile --optimize-for-debug --profile "$(PROFILE)" -v --jobs 0 --build-path ./build/debug . $(CPROPS)

CPROPS_TEST = --build-property "compiler.cpp.extra_flags=$(FLAGS) $(DEFINES) $(ADDITIONAL_DEFINES) -DTEST_UNIT" \
              --build-property "compiler.c.extra_flags=$(DEFINES) $(ADDITIONAL_DEFINES) -DTEST_UNIT"

build/test/unit/$(SKETCH_NAME).ino.bin: $(SRCS) src/lua/LightAir_GamesBundle.h
	arduino-cli compile --optimize-for-debug --profile "$(PROFILE)" -v --jobs 0 --build-path ./build/test/unit . $(CPROPS_TEST)

# Regenerate the embedded stock-games header whenever a game file changes.
src/lua/LightAir_GamesBundle.h: $(wildcard games/*.lua) $(wildcard games/lib/*.lua) tools/embed_games.py
	python3 tools/embed_games.py

clean:
	rm -rf build 