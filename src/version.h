#pragma once
#include <stdint.h>

// ================================================================
// version.h — LightAir firmware version.
//
// Single source of truth for the firmware's human-facing version.
// Bump the three numbers below; the string and the packed number are
// derived from them, so they cannot drift.
//
// Release checklist
// ─────────────────
// 1. Bump MAJOR / MINOR / PATCH here.
// 2. Copy the same value into library.properties (version=).
//    Nothing enforces this: arduino-cli reads that file, the firmware
//    reads this one.  They are currently the only two places a
//    LightAir version number appears.
//
// This is NOT a wire contract.  Two separate constants version the
// protocol surfaces and bump only when those break:
//   LuaDefaults::API_VERSION (config.h) — game-file binding contract
//   TotemVMDefs::VERSION     (config.h) — totem program format
// Both are advertised in the totem beacon (LightAir_TotemDriver.cpp).
// Keep all three independent; a firmware release does not imply a
// protocol break, and vice versa.
// ================================================================

#define LIGHTAIR_VERSION_MAJOR 1
#define LIGHTAIR_VERSION_MINOR 0
#define LIGHTAIR_VERSION_PATCH 0

// Two-level expansion: the inner macro applies #, the outer one forces
// its argument to expand first.  Stringifying directly would yield
// "LIGHTAIR_VERSION_MAJOR" instead of "1".
#define LIGHTAIR_STR_(x) #x
#define LIGHTAIR_STR(x)  LIGHTAIR_STR_(x)

#define LIGHTAIR_VERSION_STR                     \
    LIGHTAIR_STR(LIGHTAIR_VERSION_MAJOR) "."     \
    LIGHTAIR_STR(LIGHTAIR_VERSION_MINOR) "."     \
    LIGHTAIR_STR(LIGHTAIR_VERSION_PATCH)

// Build identifier — which build, not which release.  Injected via the
// Makefile's ADDITIONAL_DEFINES hook:
//   make ADDITIONAL_DEFINES='-DLIGHTAIR_BUILD_ID=\"$(shell git describe --always --dirty)\"'
// Empty for an Arduino IDE build, which cannot pass extra defines.
#ifndef LIGHTAIR_BUILD_ID
#define LIGHTAIR_BUILD_ID ""
#endif

// C++ view of the same values, for code that prefers typed constants.
namespace VersionDefs {
    static constexpr uint8_t MAJOR = LIGHTAIR_VERSION_MAJOR;
    static constexpr uint8_t MINOR = LIGHTAIR_VERSION_MINOR;
    static constexpr uint8_t PATCH = LIGHTAIR_VERSION_PATCH;

    // Packed 0x00MMmmpp — compare two firmware versions with a single
    // relational operator instead of nested field comparisons.
    static constexpr uint32_t NUM = ((uint32_t)MAJOR << 16) |
                                    ((uint32_t)MINOR <<  8) |
                                     (uint32_t)PATCH;

    static constexpr const char* STR      = LIGHTAIR_VERSION_STR;  // "1.0.0"
    static constexpr const char* BUILD_ID = LIGHTAIR_BUILD_ID;
}