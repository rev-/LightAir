#pragma once
#include <stdint.h>

// ================================================================
// GameTypeIds — central registry of all assigned game typeId values.
//
// Rules:
//   - Values are permanent. Never reuse a retired value; bump to the
//     next available slot instead (this acts as an implicit schema
//     version: a device on old firmware rejects config blobs from a
//     new schema because the typeId won't match).
//   - Values must be unique. GameManager::registerGame() enforces
//     this at runtime; this header makes collisions visible at a
//     glance.
//   - RadioTypeId::UNIVERSAL (0x0000) is reserved by the radio layer
//     for infrastructure messages and must never appear here.
// ================================================================

namespace GameTypeId {
    constexpr uint16_t FREE_FOR_ALL = 0x0001;
    constexpr uint16_t TEAMS        = 0x0002;
    constexpr uint16_t FLAG         = 0x0003;
    constexpr uint16_t OUTFLOW      = 0x0004;
    constexpr uint16_t UPKEEP       = 0x0005;
    constexpr uint16_t KING_OF_HILL = 0x0006;
    // Next available: 0x0007
}
