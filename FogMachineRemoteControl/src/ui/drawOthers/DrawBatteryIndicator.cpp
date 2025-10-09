// DrawBatteryIndicator.cpp
#include "Pins.h"
#include "Defaults.h"
#include "ui/DisplayManager.h"

void DisplayManager::drawBatteryIndicator(uint8_t percent) const {
    int x = Defaults::UI_BATT_X;
    int y = Defaults::UI_BATT_Y;
    int w = Defaults::UI_BATT_W;
    int h = Defaults::UI_BATT_H;
    int termW = Defaults::UI_BATT_TERM_W;
    int termH = Defaults::UI_BATT_TERM_H;
    display.fillRect(x, y, w + termW + 1, h, SSD1306_BLACK);
    display.drawRect(x, y, w, h, SSD1306_WHITE);
    display.fillRect(x + w, y + (h - termH)/2, termW, termH, SSD1306_WHITE);
    int innerW = w - 2;
    int innerH = h - 2;
    if (percent > 100) percent = 100;
    int fillW = (innerW * percent) / 100;
    if (fillW < 0) fillW = 0; if (fillW > innerW) fillW = innerW;
    if (fillW > 0) display.fillRect(x + 1, y + 1, fillW, innerH, SSD1306_WHITE);
    bool charging=false, plugged=false;
    if (CHARGER_CHG_PIN >= 0) charging = (digitalRead(CHARGER_CHG_PIN) == HIGH);
    if (CHARGER_PWR_PIN >= 0) plugged  = (digitalRead(CHARGER_PWR_PIN) == HIGH);
    if (charging) {
        int bx = x + w/2 - 2;
        int by = y + 1;
        display.drawLine(bx+1, by+0, bx+3, by+3, SSD1306_BLACK);
        display.drawLine(bx+3, by+3, bx+2, by+3, SSD1306_BLACK);
        display.drawLine(bx+2, by+3, bx+4, by+6, SSD1306_BLACK);
        display.drawLine(bx+0, by+3, bx+2, by+3, SSD1306_BLACK);
        display.drawLine(bx+2, by+3, bx+0, by+6, SSD1306_BLACK);
    } else if (plugged) {
        int px = x + w/2 - 3;
        int py = y + 2;
        display.fillRect(px, py, 5, 3, SSD1306_BLACK);
        display.drawPixel(px+1, py-1, SSD1306_BLACK);
        display.drawPixel(px+3, py-1, SSD1306_BLACK);
        display.drawLine(px+4, py+1, px+6, py+1, SSD1306_BLACK);
    }
}
