// DrawUpdateCountdown.cpp
#include "ui/DisplayManager.h"
#include <cstring>
#include <cstdio>

void DisplayManager::drawUpdateCountdown(uint8_t secondsRemaining) const {
    if (!inited) return;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    // Header
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Firmware Update Mode");
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
    // Short instruction without any hold/release hints
    display.setCursor(0, 16);
    display.println("Connect USB and flash");
    // Big centered countdown in seconds; shift down by only 2 pixels as requested
    {
        char buf[8]; snprintf(buf, sizeof(buf), "%us", (unsigned)secondsRemaining);
        int len = strlen(buf);
    int size = 3;
        display.setTextSize(size);
        int charW = 6 * size; int charH = 8 * size;
        int textW = len * charW;
        int x = (128 - textW) / 2; if (x < 0) x = 0;
    // Vertically center and nudge +7px down (2px further down from the prior +5px state)
    int y = ((64 - charH) / 2) + 7; if (y < 10) y = 10;
        display.fillRect(0, y-2, 128, charH+4, SSD1306_BLACK);
        display.setCursor(x, y);
        display.print(buf);
    }
    display.display();
}
