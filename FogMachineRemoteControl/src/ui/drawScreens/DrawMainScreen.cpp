// DrawMainScreen.cpp
#include "Defaults.h"
#include "comm/CommManager.h"
#include "device/DeviceManager.h"
#include "battery/BatteryMonitor.h"
#include "ui/DisplayManager.h"
#include "menu/MenuSystem.h"
#include <cstdio>

void DisplayManager::drawMainScreen(const DeviceManager& deviceMgr, const BatteryMonitor& battery, const MenuSystem& menu) const {
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
    unsigned long now = millis();
    bool fresh = (act->lastStatusMs != 0) && (now - act->lastStatusMs < Defaults::RSSI_STALE_MS);
    {
        if (!fresh) {
            display.setTextSize(1);
            display.setCursor(64, 0);
            display.print("Stale");
            display.setTextSize(2);
        }
        auto drawRssiBars = [&](int8_t rssi, int x, int y){
            // Treat invalid/default readings (>=0) and very low sentinel (<=-120) as 0 levels
            int8_t v = rssi;
            if (v >= 0 || v <= -120) v = -127;
            // Map calibrated range [low..high] (negative dBm) to [0..6] bars, with clamping
            int8_t low = menu.getAppliedRssiLowDbm();
            int8_t high = menu.getAppliedRssiHighDbm();
            if (high <= low) { high = (int8_t)(low + 5); }
            if (v <= low) {
                v = low;
            } else if (v >= high) {
                v = high;
            }
            int level = 0;
            if (high > low) {
                float frac = (float)(v - low) / (float)(high - low); // 0..1
                int mapped = (int)(frac * 6.0f + 0.5f);
                if (mapped < 0) mapped = 0; if (mapped > 6) mapped = 6;
                level = mapped;
            }
            const int bars = 6;
            const int barW = 3; const int gap = 1; const int areaH = 12; // width=3 makes hollow (outlined) bars visibly different
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
        // Use Timer-side RSSI value but keep the existing RSSI icon and placement
        drawRssiBars(act->rssiSlave, 0, rssiY);
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
