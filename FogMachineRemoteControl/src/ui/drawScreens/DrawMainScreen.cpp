// DrawMainScreen.cpp
#include "Defaults.h"
#include "comm/CommManager.h"
#include "device/DeviceManager.h"
#include "battery/BatteryMonitor.h"
#include "ui/DisplayManager.h"
#include <cstdio>

void DisplayManager::drawMainScreen(const DeviceManager& deviceMgr, const BatteryMonitor& battery) const {
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    if (deviceMgr.getDeviceCount() == 0) {
        display.setCursor(0, 12);
        display.print("No paired");
        display.setCursor(0, 24);
        display.print("timers.");
        return;
    }
    const SlaveDevice* act = deviceMgr.getActive();
    if (!act) {
        display.setCursor(0, 12);
        display.print("No active");
        return;
    }
    {
        auto drawRssiBars = [&](int8_t rssi, int x, int y){
            int level = 0;
            if      (rssi > -45) level = 6;
            else if (rssi > -55) level = 5;
            else if (rssi > -65) level = 4;
            else if (rssi > -75) level = 3;
            else if (rssi > -85) level = 2;
            else if (rssi > -95) level = 1;
            else level = 0;
            const int bars = 6;
            const int barW = 2; const int gap = 1; const int areaH = 12;
            for (int i=0;i<bars;i++) {
                int h = 2 + i*2;
                int bx = x + i*(barW+gap);
                int baseY = y + (areaH - 1);
                int by = baseY - (h - 1);
                display.fillRect(bx, y, barW, areaH, SSD1306_BLACK);
                display.fillRect(bx, baseY, barW, 1, SSD1306_WHITE);
                if (i < level) {
                    if (h > 1) display.fillRect(bx, by, barW, h-1, SSD1306_WHITE);
                } else {
                    display.drawRect(bx, by, barW, h-1, SSD1306_WHITE);
                }
            }
        };
        int rssiY = Defaults::UI_BATT_Y + Defaults::UI_BATT_H + 4;
        drawRssiBars(act->rssiSlave, 0, rssiY);
    }
    unsigned long now = millis();
    bool fresh = (act->lastStatusMs != 0) && (now - act->lastStatusMs < 5000UL);
    if (!fresh) {
        display.setTextSize(1);
        display.setCursor(0, 36);
        display.print("Timer disconnected...");
        return;
    }
    drawTimerRow((int)(act->toff*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_OFF, "OFF", Defaults::UI_TIMER_START_X);
    drawTimerRow((int)(act->ton*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_ON,  "ON",  Defaults::UI_TIMER_START_X);
    {
        unsigned long nowMs = millis();
        float since = (act->lastStatusMs > 0) ? ((nowMs - act->lastStatusMs) / 1000.0f) : 0.0f;
        float e = act->elapsed + since;
        float cap = act->outputState ? act->ton : act->toff;
        if (e > cap) e = cap;
        drawTimerRow((int)(e*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_TIME,  "TIME",  Defaults::UI_TIMER_START_X);
    }
    display.setTextSize(2); if (act->outputState) { display.setCursor(0, Defaults::UI_STATE_CHAR_Y); display.print('*'); }
}
