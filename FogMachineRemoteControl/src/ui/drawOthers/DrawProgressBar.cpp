// DrawProgressBar.cpp
#include "Defaults.h"
#include "ui/DisplayManager.h"

void DisplayManager::drawProgressBar(unsigned long holdMs, unsigned long longPressMs) const {
    const int barX = Defaults::UI_PBAR_X, barY = Defaults::UI_PBAR_Y, barW = Defaults::UI_PBAR_W, barH = Defaults::UI_PBAR_H;
    float percent = (float)holdMs / (float)longPressMs;
    if (percent < 0.f) percent = 0.f; if (percent > 1.f) percent = 1.f;
    display.fillRect(barX, barY, barW, barH, SSD1306_BLACK);
    display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    int fillW = (int)((barW-2) * percent + 0.5f);
    if (fillW > 0) display.fillRect(barX+1, barY+1, fillW, barH-2, SSD1306_WHITE);
    display.setTextSize(1);
    if (percent >= 0.99f) {
        static bool blink=false; static unsigned long lastBlink=0; unsigned long now=millis();
        if (now - lastBlink > 350) { blink = !blink; lastBlink = now; }
        if (blink) {
            const char* txt="MENU"; int txtW=4*6; int tx=barX+(barW-txtW)/2; int ty=barY+4;
            display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
            display.setCursor(tx,ty); display.print(txt);
        }
    } else {
        int pctInt = (int)(percent * 100.0f + 0.5f); char buf[8]; snprintf(buf,sizeof(buf),"%3d%%", pctInt);
        int txtW=5*6; int tx=barX+(barW-txtW)/2; int ty=barY+4;
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
        display.setCursor(tx,ty); display.print(buf);
    }
}
