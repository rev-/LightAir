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
    // 0x0002–0x0005 retired: Teams/Flag/Outflow/Upkeep pre-RadioMessages registry.
    // Bumped because the 0xA0/0x10/0x50 message layout changes wire format.
    constexpr uint16_t TEAMS        = 0x0007;
    constexpr uint16_t FLAG         = 0x0008;
    constexpr uint16_t OUTFLOW      = 0x0006;
    constexpr uint16_t UPKEEP       = 0x0009;
    // Next available: 0x000A
}
