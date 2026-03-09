#ifndef DISPLAYCTRL_H
#define DISPLAYCTRL_H

#include <Arduino.h>
#include <Ticker.h>
#include <SSD1306Wire.h>

class DisplayCtrl {
public:

    enum VarType { TYPE_INT, TYPE_FLOAT, TYPE_BOOL, TYPE_STRING };

    struct MonitorItem {
        void* ptr;
        VarType type;
        int x, y;
        char prefix;
        char lastRendered[12];   // max 11 chars + null
    };

    DisplayCtrl(SSD1306Wire& display,
                const uint8_t* font = ArialMT_Plain_10,
                TextAlignment alignment = TEXT_ALIGN_LEFT,
                uint8_t charWidth = 6);

    // ---------------- Monitoring ----------------
    void addToMonitor(int* var, int x, int y, char prefix = '\0');
    void addToMonitor(float* var, int x, int y, char prefix = '\0');
    void addToMonitor(bool* var, int x, int y, char prefix = '\0');
    void addToMonitor(String* var, int x, int y, char prefix = '\0');
    void monitor();
    void clearAllItems();

    // ---------------- Warnings ----------------
    void warning(const char* text);

    // ---------------- Temporary Displays ----------------
    void showText(const char* text, int x, int y, unsigned long durationMs);
    void showBarGraph(const char* prefix, int x, int y, unsigned long totalMs);

private:
    static const int MAX_ITEMS = 32;
    static const int MAX_WARNINGS = 6;

    MonitorItem items[MAX_ITEMS];
    int itemCount;

    SSD1306Wire& display;
    const uint8_t* font;
    TextAlignment alignment;
    uint8_t charWidth;
    char out[12];  // buffer for monitor items

    // -------- Warning system --------
    struct WarningLine {
        char text[32];
        int y;
    };
    WarningLine warningLines[MAX_WARNINGS];
    int warningCount;
    bool warningActive;
    Ticker warningTimer;
    void refreshWarningScreen();
    void onWarningTimeout();

    // -------- Timed Text --------
    struct TimedText {
        char text[32];
        int x, y;
        unsigned long startTime;
        unsigned long duration;
        bool active;
    } timedText;

    // -------- Bar Graph --------
    struct BarGraph {
        char prefix[9];  // max 8 chars + null
        int x, y;
        unsigned long startTime;
        unsigned long totalMs;
        bool active;
    } barGraph;

    // -------- Helpers --------
    void composeItemString(MonitorItem& item);
    void eraseItemZone(const MonitorItem& item);
    void drawItem(const MonitorItem& item);
};

#endif
