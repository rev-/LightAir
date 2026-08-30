#ifndef LIGHTAIR_DISPLAYCTRL_H
#define LIGHTAIR_DISPLAYCTRL_H

#include <Arduino.h>
#include "LightAir_Display.h"
#include "LightAir_Display_Icons.h"
#include "../../../config.h"

class LightAir_DisplayCtrl {
public:

    /* ================================
     *       CONSTRUCTOR
     *    =================================== */
    LightAir_DisplayCtrl(LightAir_Display& display);
    void begin();

    /* ================================
     *       BINDING SET MANAGEMENT
     *    =================================== */
    uint8_t createBindingSet();
    void selectBindingSet(uint8_t setId);
    void activateBindingSet(uint8_t setId);

    /* ================================
     *       VARIABLE BINDING
     *    =================================== */
    bool bindIntVariable(
        int* variable,
        IconType icon,
        uint8_t x,
        uint8_t y
    );

    /* ================================
     *       BAR (value → filling gauge)
     *
     * Renders *variable* as a number, except while it sits at
     * triggerValue: then the slot becomes a bar that fills over
     * *fillSecs seconds, and reverts to the number as soon as the
     * variable leaves the trigger.
     *
     * fillSecs is a POINTER into the owning config slot, not a copy:
     * bindings are built once at game start while the duration
     * (recharge / respawn seconds) stays menu-editable, so the bar must
     * read it live.  It may be null, in which case nothing is drawn
     * while at the trigger.
     *
     * By default the fill restarts whenever the variable arrives at the
     * trigger, and whenever this binding set is activated — entering a
     * state is what starts a respawn wait, and a bar bound to an
     * always-at-trigger variable has no value change to key off.
     *
     * startMs overrides that when the owner knows better.  It is a
     * pointer to a millisecond timestamp: the instant the wait actually
     * began, or 0 for "not waiting", which draws an empty bar.  A
     * projector's energy needs this — with a refill recharge the wait
     * starts when the TRIGGER IS RELEASED, not when the pool hit zero, so
     * a self-started bar would fill while a player held a dead trigger
     * and nothing came back.  Null = self-start, as above.
     *
     * Three shapes cover what the game needs:
     *   energy  : variable = energy, trigger = 0, fill = rechargeSecs,
     *             start = the projector's reload anchor
     *   respawn : variable = a const 0, trigger = 0, fill = respawnSecs
     *   simple  : as respawn, with start = null to self-start
     *    =================================== */
    bool bindBarVariable(
        int* variable,
        IconType icon,
        uint8_t x,
        uint8_t y,
        int triggerValue,
        const int* fillSecs,
        uint8_t barWidth = DisplayDefaults::BAR_WIDTH,
        const int* startMs = nullptr
    );

    bool bindStringVariable(
        const char* str,
        IconType icon,
        uint8_t x,
        uint8_t y
    );

    /* ================================
     *       TRAY MESSAGES
     *
     * Messages stack top-to-bottom. Each new message
     * pushes existing ones down by one line. Messages
     * that fall below the tray area are discarded.
     * durationMs = 0 means the message persists until
     * replaced or clearTray() is called.
     *    =================================== */
    void showMessage(const char* text, uint32_t durationMs = 0);
    void clearTray();

    /* ================================
     *       MAIN UPDATE
     *    =================================== */
    void update();

private:

    enum BindingType {
        TYPE_INT,
        TYPE_BAR,
        TYPE_STRING
    };

    struct VariableBinding {
        union {
            int*        variable;    // TYPE_INT, TYPE_BAR
            const char* strVariable; // TYPE_STRING
        };
        IconType icon;
        BindingType type;
        uint8_t  x;
        uint8_t  y;
        int        trigger;     // TYPE_BAR: value at which the bar takes over
        const int* fillSecs;    // TYPE_BAR: live pointer to the fill duration
        const int* startMs;     // TYPE_BAR: owner's start instant; null = self-start
        uint32_t   fillStart;   // TYPE_BAR: millis() when the fill began
        uint8_t    barWidth;
        union {
            int  lastValue;       // TYPE_INT, TYPE_BAR
            char lastText[32];    // TYPE_STRING
        };
        bool     filling;       // TYPE_BAR: currently drawing the bar
    };

    struct BindingSet {
        VariableBinding bindings[DisplayDefaults::MAX_BINDINGS];
        uint8_t count;
        bool    locked;
    };

    /* ================================
     *       INTERNAL
     *    =================================== */
    void renderBinding(VariableBinding& b);
    void renderInt(VariableBinding& b);
    void renderBar(VariableBinding& b);
    void renderString(VariableBinding& b);
    void renderTray();

    void drawIcon(IconType icon, uint8_t x, uint8_t y);
    const uint8_t* getIconBitmap(IconType icon);
    void drawBar(uint8_t x, uint8_t y, uint8_t width, uint8_t height, float ratio);

    LightAir_Display& _display;

    BindingSet _sets[DisplayDefaults::MAX_SETS];
    uint8_t _setCount;
    uint8_t _selectedSet;
    uint8_t _activeSet;

    struct TrayMessage {
        char     text[32];
        uint32_t expireAt;  // millis() deadline; 0 = no expiry
        bool     active;
        bool     dirty;
    };

    TrayMessage _tray[DisplayDefaults::TRAY_MAX_MESSAGES];
    bool _pendingClear;
};

#endif
