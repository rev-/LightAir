INCLUDES=-I{build.source.path}/src -I{build.source.path}/src/libs/lua-5.5.0/src
FLAGS=-Wall -Wextra
DEFINES=-DLUA_32BITS
ADDITIONAL_DEFINES?=
SKETCH_NAME?=LightAir
# Leave blank for default profile
PROFILE?=

SRCS = $(wildcard *.ino) $(wildcard src/**/*.cpp) $(wildcard src/**/*.h) $(wildcard src/**/*.c)

build/debug/$(SKETCH_NAME).ino.bin: $(SRCS)
	arduino-cli compile --optimize-for-debug --profile "$(PROFILE)" -v --jobs 0 --build-path ./build/debug . --build-property "compiler.cpp.extra_flags=$(INCLUDES) $(FLAGS) $(DEFINES) $(ADDITIONAL_DEFINES)"

build/test/unit/$(SKETCH_NAME).ino.bin: $(SRCS)
	arduino-cli compile --optimize-for-debug --profile "$(PROFILE)" -v --jobs 0 --build-path ./build/test/unit . --build-property "compiler.cpp.extra_flags=$(INCLUDES) $(FLAGS) $(DEFINES) $(ADDITIONAL_DEFINES) -DTEST_UNIT"

clean:
	rm -rf build 