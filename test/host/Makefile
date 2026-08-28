# Host test suite — runs on a PC, no ESP32 toolchain needed.
#
#   make -C test/host          # build + run everything
#
# Requires: g++, lua5.4 (apt install lua5.4).
#
# What runs:
#   games      — loads every games/*.lua against a stubbed `la` kernel,
#                exercises handlers/rules/totem tables, and size-checks
#                every TotemVM program with the reference encoder.
#   totemvm    — feeds reference-encoder programs into the real C++
#                LightAir_TotemVM and asserts the five roles' behaviour.
#   radio      — LightAir_Radio's reply bookkeeping: every reply to a
#                broadcast is delivered, a unicast's slot closes on the
#                first answer, timeouts fire only where they mean something.
#   strip      — the totem LED strip's one-shot queue: several animations
#                triggered together play in order instead of replacing
#                one another.
#   luagame    — loads every real game file through the real
#                LightAir_LuaGame binding (real Lua 5.5 core) and drives
#                a scripted FFA session through the synthesized
#                LightAir_Game descriptor exactly as GameRunner would.

ROOT  := ../..
BUILD := build
LUA   ?= lua5.4

CXX      := g++
CXXFLAGS := -std=gnu++11 -Wall -Wextra -Wno-unused-parameter -g \
            -I stubs -I $(ROOT)/src -I $(ROOT)/src/libs/lua-5.5.0/src
CC       := gcc
CFLAGS   := -O1 -I $(ROOT)/src/libs/lua-5.5.0/src

LUA_SRC  := $(wildcard $(ROOT)/src/libs/lua-5.5.0/src/*.c)
LUA_OBJ  := $(patsubst $(ROOT)/src/libs/lua-5.5.0/src/%.c,$(BUILD)/lua/%.o,$(LUA_SRC))

FW_SRC := $(ROOT)/src/lua/LightAir_LuaGame.cpp \
          $(ROOT)/src/lua/LightAir_LuaKernel.cpp \
          $(ROOT)/src/lua/LightAir_TotemEncoder.cpp \
          $(ROOT)/src/lua/LightAir_LuaEngine.cpp \
          $(ROOT)/src/game/LightAir_GameRunner.cpp \
          $(ROOT)/src/radio/LightAir_Radio.cpp \
          $(ROOT)/src/input/LightAir_InputCtrl.cpp \
          $(ROOT)/src/input/SpiAdcSensor.cpp \
          $(ROOT)/src/enlight/SpiAdcBus.cpp \
          $(ROOT)/src/ui/player/display/LightAir_DisplayCtrl.cpp \
          $(ROOT)/src/ui/player/LightAir_UICtrl.cpp

PROGS := $(BUILD)/prog_base0.bin

.PHONY: all games totemvm radio strip luagame clean
all: games totemvm radio strip luagame
	@echo "ALL HOST TESTS PASS"

games:
	cd $(ROOT) && $(LUA) test/host/test_games.lua

$(BUILD)/lua/%.o: $(ROOT)/src/libs/lua-5.5.0/src/%.c
	@mkdir -p $(BUILD)/lua
	$(CC) $(CFLAGS) -c $< -o $@

$(PROGS): totemvm.lua gen_programs.lua $(ROOT)/games/lib/std.lua
	@mkdir -p $(BUILD)
	cd $(ROOT) && $(LUA) test/host/gen_programs.lua test/host test/host/$(BUILD)

$(BUILD)/test_totemvm: test_totemvm.cpp $(ROOT)/src/totem/LightAir_TotemVM.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $^

totemvm: $(BUILD)/test_totemvm $(PROGS)
	$(BUILD)/test_totemvm $(BUILD)

$(BUILD)/test_radio: test_radio.cpp $(ROOT)/src/radio/LightAir_Radio.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $^

radio: $(BUILD)/test_radio
	$(BUILD)/test_radio

$(BUILD)/test_strip: test_strip.cpp $(ROOT)/src/ui/totem/strip/LightAir_LEDStrip_HW.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $^

strip: $(BUILD)/test_strip
	$(BUILD)/test_strip

$(BUILD)/test_luagame: test_luagame.cpp $(FW_SRC) $(LUA_OBJ)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ test_luagame.cpp $(FW_SRC) $(LUA_OBJ) -lm

luagame: $(BUILD)/test_luagame
	cd $(ROOT) && test/host/$(BUILD)/test_luagame

clean:
	rm -rf $(BUILD)
