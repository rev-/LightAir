#pragma once
#include <stdint.h>
#include "../ui/display/LightAir_Display_Icons.h"

// ----------------------------------------------------------------
// VarType — discriminates the two kinds of game variable.
//
//   INT   : int* (32-bit signed on ESP32-S3).
//           Compatible with DisplayCtrl::bindIntVariable.
//           Can be marked isConfig to appear in the config menu.
//
//   CHARS : char* (mutable null-terminated buffer).
//           Compatible with DisplayCtrl::bindStringVariable.
//           Not configurable via the config menu.
// ----------------------------------------------------------------
enum class VarType : uint8_t { INT, CHARS };

// ----------------------------------------------------------------
// GameVar — one tracked variable owned by a game.
//
// A game typically declares its variables as file-scope statics
// and lists them in a GameVar array inside the LightAir_Game struct.
//
// Display binding:
//   When stateMask != 0, GameRunner::begin() automatically creates
//   the necessary DisplayCtrl binding sets and binds this variable
//   to them.  Bit N of stateMask means "show in state N".
//
// Config:
//   INT vars with isConfig=true are shown in the config menu.
//   cfgStep = 0 is treated as 1 by the config menu.
//   CHARS vars cannot be config (isConfig is ignored for CHARS).
//
// Construction:
//   Use the static factory helpers to avoid union init ambiguity:
//
//     static int  lives = 3;
//     static char role[16] = "Attacker";
//
//     static GameVar vars[] = {
//         GameVar::Int ("Lives", &lives, 1<<IN_GAME, ICON_LIFE,  0,0, true, 1,10,1),
//         GameVar::Str ("Role",  role,   1<<IN_GAME, ICON_ROLE,  1,0),
//         GameVar::Int ("Score", &score, 1<<IN_GAME, ICON_SCORE, 0,1),
//     };
// ----------------------------------------------------------------
struct GameVar {
    const char* name;       // ≤12 chars; shown in config menu header
    VarType     type;
    int*        asInt;      // non-null when type == INT
    char*       asChars;    // non-null when type == CHARS
    uint32_t    stateMask;  // bit N → display in state N;  0 = not monitored
    IconType    icon;
    uint8_t     col, row;   // display grid position (passed to bind*Variable)
    bool        isConfig;   // INT only: show in config menu?
    int         cfgMin;
    int         cfgMax;
    int         cfgStep;    // 0 treated as 1

    // ---- factory helpers ----

    static GameVar Int(const char* name, int* value,
                       uint32_t stateMask, IconType icon, uint8_t col, uint8_t row,
                       bool isConfig = false,
                       int cfgMin = 0, int cfgMax = 0, int cfgStep = 1) {
        GameVar v = {};
        v.name      = name;
        v.type      = VarType::INT;
        v.asInt     = value;
        v.asChars   = nullptr;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;   v.row = row;
        v.isConfig  = isConfig;
        v.cfgMin    = cfgMin; v.cfgMax = cfgMax;
        v.cfgStep   = cfgStep ? cfgStep : 1;
        return v;
    }

    static GameVar Str(const char* name, char* buf,
                       uint32_t stateMask, IconType icon, uint8_t col, uint8_t row) {
        GameVar v = {};
        v.name      = name;
        v.type      = VarType::CHARS;
        v.asInt     = nullptr;
        v.asChars   = buf;
        v.stateMask = stateMask;
        v.icon      = icon;
        v.col       = col;   v.row = row;
        v.isConfig  = false;
        return v;
    }
};
