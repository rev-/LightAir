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

    bool bindCooldownVariable(
        int* variable,
        IconType icon,
        uint8_t x,
        uint8_t y,
        uint32_t cooldownTimeMs,
        uint8_t barWidth = 16
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
        TYPE_COOLDOWN,
        TYPE_STRING
    };

    struct VariableBinding {
        union {
            int*        variable;    // TYPE_INT, TYPE_COOLDOWN
            const char* strVariable; // TYPE_STRING
        };
        IconType icon;
        BindingType type;
        uint8_t  x;
        uint8_t  y;
        uint32_t cooldownTime;
        uint32_t cooldownStart;
        uint8_t  barWidth;
        union {
            int  lastValue;       // TYPE_INT, TYPE_COOLDOWN
            char lastText[32];    // TYPE_STRING
        };
        bool     cooldownActive;
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
    void renderCooldown(VariableBinding& b);
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
};

#endif
