#pragma once

// Abstract interface for a single button input.
// Concrete implementations must report the instantaneous physical
// state of the button. State machine logic (PRESSED/HELD/RELEASED)
// is handled entirely by LightAir_InputCtrl.
class LightAir_Button {
public:
    // Returns true while the button is physically held down.
    virtual bool isPressed() const = 0;

    virtual ~LightAir_Button() = default;
};
