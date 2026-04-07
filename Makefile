INCLUDES=-I{build.source.path}/src
FLAGS=-Wall -Wextra
SKETCH_NAME=LightAir
# Leave blank for default profile
PROFILE=

SRCS = $(wildcard *.ino) $(wildcard src/**/*.cpp) $(wildcard src/**/*.h) $(wildcard src/**/*.c)

build/debug/$(SKETCH_NAME).ino.bin: $(SRCS)
	arduino-cli compile --optimize-for-debug --profile "$(PROFILE)" -v --jobs 0 --build-path ./build/debug . --build-property "compiler.cpp.extra_flags=$(INCLUDES) $(FLAGS)"

clean:
	rm -rf build 