// DrawTimerRow.cpp
#include "Defaults.h"
#include <cstdio>
#include "ui/DisplayManager.h"

void DisplayManager::drawTimerRow(int tenths, int y, const char* label, int startX) const {
    char buf[8]; int integerPart = tenths/10; int frac = tenths%10; snprintf(buf,sizeof(buf),"%04d%01d", integerPart, frac);
    display.setTextSize(2);
    int x = startX; int digitW = Defaults::UI_DIGIT_WIDTH;
    for (int i=0;i<5;i++) {
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        display.fillRect(x,y,digitW,16,SSD1306_BLACK);
        display.setCursor(x,y);
        display.print(buf[i]);
        if (i==3) { display.print('.'); x+=digitW; }
        x += digitW;
    }
    int labelX = startX + digitW*(5+1) + Defaults::UI_LABEL_GAP_X;
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.setCursor(labelX,y+7); display.print(label);
}
