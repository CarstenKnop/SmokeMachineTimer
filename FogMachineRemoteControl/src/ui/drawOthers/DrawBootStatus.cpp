// DrawBootStatus.cpp
#include "ui/DisplayManager.h"
#include <cstring>

void DisplayManager::drawBootStatus(const char* msg) const {
    if (!inited) return;
    const int y = 54;
    display.fillRect(0, y-1, 128, 11, SSD1306_BLACK);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    int maxChars = 21;
    char buf[32];
    if (!msg) msg = "";
    strncpy(buf, msg, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;
    if ((int)strlen(buf) > maxChars) buf[maxChars] = 0;
    display.setCursor(0, y);
    display.print(buf);
    display.display();
}
