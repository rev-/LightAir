#include "DisplayCtrl.h"
#include <cstring>

DisplayCtrl::DisplayCtrl(SSD1306Wire& displayRef,
                         const uint8_t* fontArg,
                         TextAlignment alignmentArg,
                         uint8_t charWidthArg)
: display(displayRef),
itemCount(0),
font(fontArg),
alignment(alignmentArg),
charWidth(charWidthArg),
warningActive(false),
warningCount(0)
{
    memset(out, 0, sizeof(out));
    display.setFont(font);
    display.setTextAlignment(alignment);

    timedText.active = false;
    barGraph.active = false;
}

// ------------------- Monitoring -------------------
void DisplayCtrl::addToMonitor(int* var, int x, int y, char prefix) {
    if (itemCount < MAX_ITEMS) {
        auto& it = items[itemCount++];
        it.ptr = var; it.type = TYPE_INT;
        it.x = x; it.y = y; it.prefix = prefix;
        memset(it.lastRendered, 0, sizeof(it.lastRendered));
    }
}
void DisplayCtrl::addToMonitor(float* var, int x, int y, char prefix) {
    if (itemCount < MAX_ITEMS) {
        auto& it = items[itemCount++];
        it.ptr = var; it.type = TYPE_FLOAT;
        it.x = x; it.y = y; it.prefix = prefix;
        memset(it.lastRendered, 0, sizeof(it.lastRendered));
    }
}
void DisplayCtrl::addToMonitor(bool* var, int x, int y, char prefix) {
    if (itemCount < MAX_ITEMS) {
        auto& it = items[itemCount++];
        it.ptr = var; it.type = TYPE_BOOL;
        it.x = x; it.y = y; it.prefix = prefix;
        memset(it.lastRendered, 0, sizeof(it.lastRendered));
    }
}
void DisplayCtrl::addToMonitor(String* var, int x, int y, char prefix) {
    if (itemCount < MAX_ITEMS) {
        auto& it = items[itemCount++];
        it.ptr = var; it.type = TYPE_STRING;
        it.x = x; it.y = y; it.prefix = prefix;
        memset(it.lastRendered, 0, sizeof(it.lastRendered));
    }
}

void DisplayCtrl::composeItemString(MonitorItem& item) {
    memset(out, 0, sizeof(out));
    switch (item.type) {
        case TYPE_INT: snprintf(out, sizeof(out), "%d", *((int*)item.ptr)); break;
        case TYPE_FLOAT: snprintf(out, sizeof(out), "%.2f", *((float*)item.ptr)); break;
        case TYPE_BOOL: snprintf(out, sizeof(out), "%s", *((bool*)item.ptr) ? "true" : "false"); break;
        case TYPE_STRING: snprintf(out, sizeof(out), "%s", ((String*)item.ptr)->c_str()); break;
    }
}

void DisplayCtrl::eraseItemZone(const MonitorItem& item) {
    int eraseX = item.x + 2 * charWidth;
    int eraseWidth = 11 * charWidth;
    display.setColor(BLACK);
    display.fillRect(eraseX, item.y, eraseWidth, 10);
    display.setColor(WHITE);
}

void DisplayCtrl::drawItem(const MonitorItem& item) {
    char prefixBuf[3] = { item.prefix, ' ', 0 };
    display.drawString(item.x, item.y, prefixBuf);
    display.drawString(item.x + 2 * charWidth, item.y, item.lastRendered);
}

void DisplayCtrl::monitor() {
    if (warningActive)
        return; // suppress during warnings

        // Update monitor items
        for (int i = 0; i < itemCount; i++) {
            auto& it = items[i];
            composeItemString(it);
            if (strcmp(out, it.lastRendered) == 0) continue;
            strncpy(it.lastRendered, out, sizeof(it.lastRendered));
            it.lastRendered[sizeof(it.lastRendered)-1] = 0;
            eraseItemZone(it);
            drawItem(it);
        }

        // Timed text
        if (timedText.active) {
            display.drawString(timedText.x, timedText.y, timedText.text);
            if (millis() - timedText.startTime >= timedText.duration) {
                display.fillRect(timedText.x, timedText.y, strlen(timedText.text) * charWidth, 10, BLACK);
                timedText.active = false;
            }
        }

        // Bar graph
        if (barGraph.active) {
            unsigned long elapsed = millis() - barGraph.startTime;
            int blocksLeft = 10 - (elapsed / (barGraph.totalMs / 10));
            if (blocksLeft <= 0) {
                display.fillRect(barGraph.x, barGraph.y, (strlen(barGraph.prefix) + 10) * charWidth, 10, BLACK);
                barGraph.active = false;
            } else {
                char line[19]; // 8 prefix + 10 blocks + null
                int n = snprintf(line, sizeof(line), "%s%.*s", barGraph.prefix, blocksLeft, "██████████");
                display.fillRect(barGraph.x, barGraph.y, (strlen(barGraph.prefix) + 10) * charWidth, 10, BLACK);
                display.drawString(barGraph.x, barGraph.y, line);
            }
        }

        display.display();
}

void DisplayCtrl::clearAllItems() {
    for (int i = 0; i < itemCount; i++)
        memset(items[i].lastRendered, 0, sizeof(items[i].lastRendered));
    itemCount = 0;
    display.clear();
    display.display();
}

// ------------------- Warning System -------------------
void DisplayCtrl::refreshWarningScreen() {
    display.clear();
    for (int i = 0; i < warningCount; i++) {
        display.drawString(0, warningLines[i].y, warningLines[i].text);
    }
    display.display();
}

void DisplayCtrl::warning(const char* text) {
    warningActive = true;

    int newY = 20;
    if (warningCount > 0) newY = warningLines[warningCount - 1].y + 10;

    if (newY > 60) {
        int newCount = 0;
        for (int i = 0; i < warningCount; i++) {
            warningLines[i].y -= 10;
            if (warningLines[i].y >= 0) warningLines[newCount++] = warningLines[i];
        }
        warningCount = newCount;
        newY = 60;
    }

    if (warningCount < MAX_WARNINGS) {
        strncpy(warningLines[warningCount].text, text, 31);
        warningLines[warningCount].text[31] = 0;
        warningLines[warningCount].y = newY;
        warningCount++;
    }

    refreshWarningScreen();
    warningTimer.detach();
    warningTimer.once(2.0f, [this]() { this->onWarningTimeout(); });
}

void DisplayCtrl::onWarningTimeout() {
    warningActive = false;
    warningCount = 0;
    display.clear();

    for (int i = 0; i < itemCount; i++) {
        auto& it = items[i];
        composeItemString(it);
        strncpy(it.lastRendered, out, sizeof(it.lastRendered));
        it.lastRendered[sizeof(it.lastRendered)-1] = 0;
        drawItem(it);
    }

    display.display();
}

// ------------------- Temporary Displays -------------------
void DisplayCtrl::showText(const char* text, int x, int y, unsigned long durationMs) {
    strncpy(timedText.text, text, 31);
    timedText.text[31] = 0;
    timedText.x = x;
    timedText.y = y;
    timedText.startTime = millis();
    timedText.duration = durationMs;
    timedText.active = true;
}

void DisplayCtrl::showBarGraph(const char* prefix, int x, int y, unsigned long totalMs) {
    strncpy(barGraph.prefix, prefix, 8);
    barGraph.prefix[8] = 0;
    barGraph.x = x;
    barGraph.y = y;
    barGraph.startTime = millis();
    barGraph.totalMs = totalMs;
    barGraph.active = true;

    // Draw initial full bar
    char line[19];
    snprintf(line, sizeof(line), "%s%s", barGraph.prefix, "██████████");
    display.drawString(x, y, line);
    display.display();
}
