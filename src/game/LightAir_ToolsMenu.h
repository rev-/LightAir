#pragma once
#include <stdint.h>
#include "LightAir_GameHold.h"
#include "../ui/player/display/LightAir_Display.h"
#include "../input/LightAir_InputCtrl.h"

// ----------------------------------------------------------------
// LightAir_ToolsMenu — the in-game tools menu, opened with A+B held.
//
// A hold tool whose job is to run another one: it lists the tools
// registered with addTool(), and the one picked runs under the same hold.
// When that tool returns, so does the menu — straight back to the game.
//
// Keys follow the setup menu:  ^ / V  move,  A (O)  select,  B (X)  back.
// The menu first waits for A and B to be released, so the chord that
// opened it cannot also pick an entry.
//
// Adding a tool: implement LightAir_HoldTool and addTool() it in the
// sketch.  Everything about the hold itself is the runner's.
// ----------------------------------------------------------------
class LightAir_ToolsMenu : public LightAir_HoldTool {
public:
    static constexpr uint8_t MAX_TOOLS = 4;

    LightAir_ToolsMenu(LightAir_Display& disp, LightAir_InputCtrl& input,
                       uint8_t keypadId)
        : _disp(disp), _input(input), _keypadId(keypadId) {}

    bool addTool(LightAir_HoldTool& tool);   // false when full
    uint8_t toolCount() const { return _count; }

    const char* holdName() const override { return "Tools"; }
    void runHeld(LightAir_HoldHost& host) override;

private:
    void draw(uint8_t sel);
    bool keyDown(const InputReport& rep, char key) const;

    LightAir_Display&   _disp;
    LightAir_InputCtrl& _input;
    uint8_t             _keypadId;
    LightAir_HoldTool*  _tools[MAX_TOOLS] = {};
    uint8_t             _count = 0;
};
