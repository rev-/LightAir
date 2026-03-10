#pragma once
#include <stdint.h>
#include "../ui/display/LightAir_Display_Icons.h"

// ----------------------------------------------------------------
// VarType — discriminates the two kinds of monitored variable.
//
//   INT   : int* (32-bit signed on ESP32-S3).
//           Compatible with DisplayCtrl::bindIntVariable.
//
//   CHARS : char* (mutable null-terminated buffer).
//           Compatible with DisplayCtrl::bindStringVariable.
// ----------------------------------------------------------------
enum class VarType : uint8_t { INT, CHARS };

// ----------------------------------------------------------------
// ConfigVar — one variable shown and edited in the pre-game config menu.
//
// All ConfigVars are integer values.  The menu lets the player
// adjust the value in increments of step within [min, max].
// step = 0 is treated as 1 by the config menu.
//
// Example:
//   static int startLives = 3;
//
//   static const ConfigVar configVars[] = {
//       { "Start", &startLives, 1, 10, 1 },
//   };
// ----------------------------------------------------------------
struct ConfigVar {
    const char* name;   // ≤12 chars; shown in config menu
    int*        value;
    int         min;
    int         max;
    int         step;   // 0 treated as 1
};

// ----------------------------------------------------------------
// MonitorVar — one variable displayed on the LCD during the game.
//
// stateMask encodes which states show this variable: bit N means
// "display in state N".  GameRunner::begin() reads this mask to
// create the necessary DisplayCtrl binding sets automatically.
//
// Construction:
//   Use the static factory helpers to avoid union init ambiguity:
//
//     static int  lives = 3;
//     static char role[16] = "Attacker";
//
//     static const MonitorVar monitorVars[] = {
//         MonitorVar::Int("Lives", &lives, 1u<<IN_GAME, ICON_LIFE,  0, 0),
//         MonitorVar::Str("Role",  role,   1u<<IN_GAME, ICON_ROLE,  1, 0),
//     };
// ----------------------------------------------------------------
struct MonitorVar {
    const char* name;
    VarType     type;
    int*        asInt;      // non-null when type == INT
    char*       asChars;    // non-null when type == CHARS
    uint32_t    stateMask;  // bit N → display in state N
    IconType    icon;
    uint8_t     col, row;

    // ---- factory helpers ----

    static MonitorVar Int(const char* name, int* value,
                          uint32_t stateMask, IconType icon,
                          uint8_t col, uint8_t row) {
        MonitorVar v = {};
        v.name      = name;
        v.type      = VarType::INT;
        v.asInt     = value;
        v.asChars   = nullptr;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;  v.row = row;
        return v;
    }

    static MonitorVar Str(const char* name, char* buf,
                          uint32_t stateMask, IconType icon,
                          uint8_t col, uint8_t row) {
        MonitorVar v = {};
        v.name      = name;
        v.type      = VarType::CHARS;
        v.asInt     = nullptr;
        v.asChars   = buf;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;  v.row = row;
        return v;
    }
};
