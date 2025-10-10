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
    int innerW = w - 2;
    int innerH = h - 2;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    bool charging=false, powered=false;
    if (CHARGER_CHG_PIN >= 0) {
        int lvl = digitalRead(CHARGER_CHG_PIN);
        charging = Defaults::CHARGER_CHG_ACTIVE_HIGH ? (lvl == HIGH) : (lvl == LOW);
    }
    if (CHARGER_PWR_PIN >= 0) {
        int lvl = digitalRead(CHARGER_PWR_PIN);
        powered = Defaults::CHARGER_PWR_ACTIVE_HIGH ? (lvl == HIGH) : (lvl == LOW);
    }

    // Default percent fill
    int fillW = (innerW * percent) / 100;
    if (fillW < 0) fillW = 0; if (fillW > innerW) fillW = innerW;

    if (powered && !charging) {
        // Replace battery icon with a compact USB/plug glyph to avoid TIME overlap
        // Shift left slightly so it doesn't collide with TIME digits
        int px = x + 1;
        int py = y + 1;
        display.fillRect(x, y, w + termW + 1, h, SSD1306_BLACK);
        // Draw a simple plug: two prongs and a body
        // Body
        display.drawRect(px+2, py+1, 9, h-2, SSD1306_WHITE);
        // Cable
        display.drawLine(px, py+3, px+2, py+3, SSD1306_WHITE);
        display.drawLine(px+11, py+3, px+14, py+3, SSD1306_WHITE);
        // Prongs
        display.drawLine(px+4, py,   px+4, py+1, SSD1306_WHITE);
        display.drawLine(px+8, py,   px+8, py+1, SSD1306_WHITE);
    } else {
        // Draw classic battery outline
        display.drawRect(x, y, w, h, SSD1306_WHITE);
        display.fillRect(x + w, y + (h - termH)/2, termW, termH, SSD1306_WHITE);
        if (charging) {
            // Blink the entire battery fill to indicate charging (ignore percent)
            bool blinkOn = ((millis() / 350) % 2) == 0;
            if (blinkOn) {
                display.fillRect(x + 1, y + 1, innerW, innerH, SSD1306_WHITE);
            } else {
                // empty interior during blink-off
                display.fillRect(x + 1, y + 1, innerW, innerH, SSD1306_BLACK);
            }
        } else {
            // Normal: draw percent fill
            if (fillW > 0) display.fillRect(x + 1, y + 1, fillW, innerH, SSD1306_WHITE);
        }
    }
}
