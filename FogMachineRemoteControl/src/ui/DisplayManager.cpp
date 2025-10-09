// DisplayManager.cpp
// Handles OLED rendering, UI, and battery indicator.
#include "Pins.h"
#include "DisplayManager.h"
#include "ui/ButtonInput.h"
#include "debug/DebugMetrics.h"
#include "Defaults.h"
#include "comm/CommManager.h"

DisplayManager::DisplayManager() : display(128, 64, &Wire, -1) {}

void DisplayManager::begin() {
    if (inited) return;
    auto tryInit = [&](int sda, int scl)->bool {
        Serial.printf("[DISPLAY] Trying I2C (raw GPIO) SDA=%d SCL=%d\n", sda, scl);
        Wire.begin(sda, scl);
        Wire.setClock(100000);
        Wire.beginTransmission(0x3C);
        if (Wire.endTransmission() != 0) {
            Serial.println("[DISPLAY] Probe fail 0x3C");
            return false;
        }
        if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
            Serial.println("[DISPLAY] display.begin fail");
            return false;
        }
        return true;
    };
    if (!tryInit(OLED_SDA_GPIO_PRIMARY, OLED_SCL_GPIO_PRIMARY)) {
        Serial.println("[DISPLAY] Primary pins failed, scanning bus then trying alternate...");
        for (uint8_t a=1;a<127;++a){ Wire.beginTransmission(a); if(Wire.endTransmission()==0) Serial.printf("[I2C] dev 0x%02X\n",a); }
        if (!tryInit(OLED_SDA_GPIO_ALT, OLED_SCL_GPIO_ALT)) {
            Serial.println("[DISPLAY][FATAL] Both pin sets failed.");
            return;
        }
    }
    inited = true; splash();
}

void DisplayManager::splash() {
    if (!inited) return;
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("FogMachine");
    display.setTextSize(1);
    display.println("Remote Control");
    display.println("v1");
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.display();
    Serial.println("[DISPLAY] Splash drawn");
    delay(600);
    display.invertDisplay(true);
    delay(150);
    display.invertDisplay(false);
}

void DisplayManager::render(const DeviceManager& deviceMgr, const BatteryMonitor& battery, const MenuSystem& menu, const ButtonInput& buttons) {
    // If display not initialized successfully, skip
    if (!inited) return;

    static unsigned long lastFrame=0; unsigned long now=millis();
    if (now - lastFrame < 50) return; // cap ~20 FPS
    lastFrame = now;
    unsigned long tStart = now;
    display.clearDisplay();

    if (menu.isInMenu()) {
        drawMenu(menu, deviceMgr);
    } else {
        // Battery indicator only on non-menu screens
    uint8_t batt = battery.getPercent();
    drawBatteryIndicator(batt);
    drawMainScreen(deviceMgr, battery);
        // If editing timers mode, draw an overlay similar to original with size-2 digits and labels
        if (menu.editingTimers()) {
            display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
            auto drawTimerRowEdit = [&](int tenths, int y, const char* label, int startDigit, bool highlight){
                char buf[8]; int integerPart = tenths/10; int frac = tenths%10; snprintf(buf,sizeof(buf),"%04d%01d", integerPart, frac);
                display.setTextSize(2);
                int startX=26; int digitW=11;
                int x=startX;
                for (int i=0;i<5;i++) {
                    bool inv = highlight && (i+startDigit)==menu.getEditDigitIndex();
                    if (inv) { display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); display.fillRect(x,y,digitW,16,SSD1306_WHITE);} else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.fillRect(x,y,digitW,16,SSD1306_BLACK);} display.setCursor(x,y); display.print(buf[i]); if (i==3) { display.print('.'); x+=digitW;} x+=digitW;
                }
                int labelX = startX + digitW*(5+1) + 10;
                display.setTextSize(1); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.setCursor(labelX,y+7); display.print(label);
            };
            // First row: TOFF (digits 0..4)
            drawTimerRowEdit(menu.getEditToffTenths(), 0, "Toff", 0, true);
            // Second row: TON (digits 5..9)
            drawTimerRowEdit(menu.getEditTonTenths(), 24, "Ton", 5, true);
            display.setTextSize(1); display.setCursor(0,54); display.print("#=Next *=Cancel");
        }
        // Show progress bar for long-press only after a grace period
    unsigned long holdMs = buttons.hashHoldDuration();
        unsigned long pressStart = buttons.hashPressStartTime();
    unsigned long exitTime = menu.getMenuExitTime();
    bool neverExited = (exitTime == 0);
    // Require a post-exit release only if we've actually exited the menu before
    bool releasedAfterExit = neverExited ? true : (buttons.hashLastReleaseTime() > exitTime);
    // Hold must have started after exit (or simply started if never exited)
    bool holdStartedAfterExit = (pressStart != 0) && (neverExited || (pressStart >= exitTime));
    bool allowBar = releasedAfterExit && holdStartedAfterExit;
    // Suppress for a grace period after exiting the menu to avoid visual blips
    bool recentlyExitedMenu = (!neverExited) && ((millis() - exitTime) < Defaults::MENU_HOLD_GRACE_MS);
        if (allowBar && !recentlyExitedMenu && holdMs >= Defaults::MENU_HOLD_GRACE_MS && holdMs < ButtonInput::LONG_PRESS_MS) {
            // Reset progress origin to 0 at grace, so bar starts empty at that moment
            unsigned long adjHold = holdMs - Defaults::MENU_HOLD_GRACE_MS;
            unsigned long adjLong = ButtonInput::LONG_PRESS_MS - Defaults::MENU_HOLD_GRACE_MS;
            drawProgressBar(adjHold, adjLong);
            float pct = (float)adjHold / (float)adjLong;
            DebugMetrics::instance().recordProgress(pct);
        }
    }

    unsigned long tPre = millis();
    display.display();
    unsigned long tEnd = millis();
    unsigned long flush = tEnd - tPre;
    // Aggregate metrics instead of per-frame spam
    DebugMetrics::instance().recordDisplayFrame((uint32_t)(tPre - tStart), (uint32_t)flush, (uint32_t)(tEnd - tStart));
    if (flush > 50) { // unexpected large delay, attempt soft recovery (silent apart from metric)
        DebugMetrics::instance().recordSlowFlush();
        static unsigned long lastReinit = 0;
        if (millis() - lastReinit > 1000) {
            lastReinit = millis();
            Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
            Wire.setClock(400000);
            display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
        }
    }
}

void DisplayManager::drawBatteryIndicator(uint8_t percent) const {
    int x = Defaults::UI_BATT_X;
    int y = Defaults::UI_BATT_Y;
    int w = Defaults::UI_BATT_W;
    int h = Defaults::UI_BATT_H;
    int termW = Defaults::UI_BATT_TERM_W;
    int termH = Defaults::UI_BATT_TERM_H;
    // Clear area
    display.fillRect(x, y, w + termW + 1, h, SSD1306_BLACK);
    // Body
    display.drawRect(x, y, w, h, SSD1306_WHITE);
    // Terminal (to the right, vertically centered)
    display.fillRect(x + w, y + (h - termH)/2, termW, termH, SSD1306_WHITE);
    // Fill level (1px inset)
    int innerW = w - 2;
    int innerH = h - 2;
    if (percent > 100) percent = 100;
    int fillW = (innerW * percent) / 100;
    if (fillW < 0) fillW = 0; if (fillW > innerW) fillW = innerW;
    if (fillW > 0) display.fillRect(x + 1, y + 1, fillW, innerH, SSD1306_WHITE);
}

void DisplayManager::drawMenu(const MenuSystem& menu, const DeviceManager& deviceMgr) const {
    display.setTextSize(1);
    if (menu.isEditingBlanking()) {
        // Editing UI for Display Blanking
        display.setCursor(0, 0);
        display.setTextColor(SSD1306_WHITE);
        display.println("Display Blanking");
        display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
        int val = menu.getEditingBlankingSeconds();
        display.setCursor(0, 16);
        if (val == 0) display.println("Current: OFF");
        else { display.print("Current: "); display.print(val); display.println("s"); }
        display.setCursor(0, 28);
        display.println("Up/Down change");
        display.setCursor(0, 40);
        display.println("#=Save  *=Back");
        // Show applied (active) for context
        display.setCursor(0, 54);
        int applied = menu.getAppliedBlankingSeconds();
        display.print("Active: "); if (applied==0) display.print("OFF"); else { display.print(applied); display.print("s"); }
        return;
    } else if (menu.getMode() == MenuSystem::Mode::EDIT_TIMERS) {
        // Edit Timers modal: draw same style as main overlay but inside menu path
        auto drawTimerRowEdit = [&](int tenths, int y, const char* label, int startDigit){
            char buf[8]; int integerPart = tenths/10; int frac = tenths%10; snprintf(buf,sizeof(buf),"%04d%01d", integerPart, frac);
            display.setTextSize(2);
            int startX = Defaults::UI_TIMER_START_X; int digitW = Defaults::UI_DIGIT_WIDTH; int x=startX;
            for (int i=0;i<5;i++) {
                bool inv = ((i+startDigit)==menu.getEditDigitIndex());
                if (inv) { display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); display.fillRect(x,y,digitW,16,SSD1306_WHITE);} else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.fillRect(x,y,digitW,16,SSD1306_BLACK);} display.setCursor(x,y); display.print(buf[i]); if (i==3) { display.print('.'); x+=digitW;} x+=digitW;
            }
            int labelX = startX + digitW*(5+1) + Defaults::UI_LABEL_GAP_X;
            display.setTextSize(1); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.setCursor(labelX,y+7); display.print(label);
        };
        drawTimerRowEdit(menu.getEditToffTenths(), Defaults::UI_TIMER_ROW_Y_OFF, "Toff", 0);
        drawTimerRowEdit(menu.getEditTonTenths(),  Defaults::UI_TIMER_ROW_Y_ON,  "Ton",  5);
        display.setTextSize(1); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.setCursor(0,54); display.print("#=Next *=Cancel");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::PAIRING) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Pair Device"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        auto *comm = CommManager::get();
        bool discovering = comm && comm->isDiscovering();
        int count = (comm? comm->getDiscoveredCount():0);
        int sel = menu.getPairingSelection(); if (sel >= count) sel = count>0?count-1:0;
        int first = 0; if (sel >= 4) first = sel-3;
        for (int i=0;i<4 && first+i<count;i++) {
            const auto &d = comm->getDiscovered(first+i);
            bool highlight = (first+i)==sel;
            int y = 12 + i*12;
            if (highlight) { display.fillRect(0,y,128,10,SSD1306_WHITE); display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);} else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);}                
            display.setCursor(2,y);
            char macBuf[18]; snprintf(macBuf,sizeof(macBuf),"%02X%02X%02X", d.mac[3],d.mac[4],d.mac[5]);
            // Mark if already paired
            bool already=false; for(int p=0;p<deviceMgr.getDeviceCount();++p){ if (memcmp(deviceMgr.getDevice(p).mac,d.mac,6)==0){already=true;break;} }
            char line[32]; snprintf(line,sizeof(line),"%c%s %s",already?'*':' ', macBuf, d.name[0]?d.name:"(noname)");
            display.print(line);
        }
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        if (discovering) {
            display.setCursor(0,54); display.print("#=Stop "); display.print("F:"); display.print(count); display.print(" *=Back");
        } else if (count>0) {
            display.setCursor(0,54); display.println("#=Pair *=Back");
        } else {
            display.setCursor(0,14); display.println("Idle");
            display.setCursor(0,26); display.println("#=Scan *=Back");
        }
        return;
    } else if (menu.getMode() == MenuSystem::Mode::MANAGE_DEVICES) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Manage Devices"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        int count = deviceMgr.getDeviceCount();
        if (count == 0) { display.setCursor(0,14); display.println("None"); display.setCursor(0,26); display.println("*=Back"); return; }
        int sel = menu.getManageSelection(); if (sel >= count) sel = count-1;
        int activeIdx = deviceMgr.getActiveIndex();
        int first=0; if (sel>=4) first=sel-3;
        for (int i=0;i<4 && first+i<count;i++) {
            int idx=first+i; const auto &d = deviceMgr.getDevice(idx);
            bool highlight = (idx==sel);
            int y=12 + i*12;
            if (highlight) { display.fillRect(0,y,128,10,SSD1306_WHITE); display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);} else display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
            display.setCursor(2,y);
            char line[32]; snprintf(line,sizeof(line),"%c %s", idx==activeIdx?'*':' ', d.name[0]?d.name:"(noname)");
            display.print(line);
        }
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        display.setCursor(0,54); display.println("#=Activate  #L=Del *=Back"); return;
    } else if (menu.getMode() == MenuSystem::Mode::RENAME_DEVICE) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Rename Device"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        if (!menu.renameEditing()) {
            display.setCursor(0,14); display.println("Press # to edit");
            display.setCursor(0,26); display.println("*=Back");
            return;
        } else {
            // Show buffer with cursor underline at current pos
            display.setCursor(0,14);
            display.setTextSize(2);
            const char* buf = menu.getRenameBuffer();
            display.print(buf);
            display.setTextSize(1);
            // Draw caret under current position
            int pos = menu.getRenamePos();
            int x = pos * 12; int y = 32; display.drawLine(x, y, x+10, y, SSD1306_WHITE);
            display.setCursor(0,48); display.setTextColor(SSD1306_WHITE); display.print("Up/Down change  #=Next  *=Back");
            return;
        }
    } else if (menu.getMode() == MenuSystem::Mode::EDIT_NAME) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Edit Name"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        display.setCursor(0,14); display.setTextSize(2);
        display.print(menu.getRenameBuffer());
        display.setTextSize(1);
        int pos = menu.getRenamePos(); int x = pos * 12; int y = 32; display.drawLine(x, y, x+10, y, SSD1306_WHITE);
        display.setCursor(0,48); display.setTextColor(SSD1306_WHITE); display.print("Up/Down change  #=Next  *=Back");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::SELECT_ACTIVE) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Select Active"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        int count = deviceMgr.getDeviceCount();
        if (count == 0) { display.setCursor(0,14); display.println("No devices"); display.setCursor(0,26); display.println("*=Back"); return; }
        int sel = menu.getActiveSelectIndex();
        if (sel >= count) sel = count-1; if (sel < 0) sel = 0; // clamp
        int activeIdx = deviceMgr.getActiveIndex();
        // show up to 4
        int first = 0; if (sel >= 4) first = sel-3;
        for (int i=0;i<4 && first+i<count;i++) {
            int idx = first + i;
            const auto &d = deviceMgr.getDevice(idx);
            bool highlight = (idx==sel);
            int y = 12 + i*12;
            if (highlight) { display.fillRect(0,y,128,10,SSD1306_WHITE); display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);} else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);}                
            display.setCursor(2,y);
            char line[32];
            // Prefix '*' for currently active device (even if not selection highlight)
            snprintf(line,sizeof(line),"%c %s", (idx==activeIdx?'*':' '), d.name[0]?d.name:"(noname)");
            display.print(line);
        }
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        display.setCursor(0,54); display.println("#=Set *=Back"); return;
    } else if (menu.getMode() == MenuSystem::Mode::CONFIRM) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Confirm"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        const char* what = "";
        auto act = menu.getConfirmAction();
    if (act == MenuSystem::ConfirmAction::RESET_SLAVE) what = "Reset Timer?";
        else if (act == MenuSystem::ConfirmAction::RESET_REMOTE) what = "Reset Remote?";
        display.setCursor(0,24); display.println(what);
        display.setCursor(0,54); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.println("#=Yes *=No");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::SHOW_RSSI) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("RSSI"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        // Header row: columns for Remote and Slave RSSI (no labels on rows to save space)
        display.setCursor(2,10); display.setTextSize(1); display.print("Name  R  S");
        int count = 0; int activeIdx = -1; if (auto *cm=CommManager::get()) { count = cm->getPairedCount(); if (cm->getActiveDevice()) activeIdx = cm->getActiveDevice() - &cm->getPaired(0); }
        int first = menu.getRssiFirst(); if (first < 0) first = 0; if (first > count-1) first = count>0?count-1:0;
        int maxRows = 4; // rows under header
        for (int i=0;i<maxRows;i++) {
            int idx = first + i; if (idx >= count) break;
            const auto &d = CommManager::get()->getPaired(idx);
            int y = 20 + i*11;
            // Highlight active device with '*'
            char name[10]; strncpy(name, d.name[0]?d.name:"(noname)", sizeof(name)-1); name[sizeof(name)-1]=0;
            display.setCursor(2,y);
            display.print((idx==activeIdx)?'*':' ');
            display.print(name);
            // Right side: Remote and Slave RSSI as integers
            char rbuf[12]; snprintf(rbuf,sizeof(rbuf)," %d %d", (int)d.rssiRemote, (int)d.rssiSlave);
            int x = 120 - (int)strlen(rbuf)*6; if (x < 64) x = 64; display.setCursor(x, y); display.print(rbuf);
        }
        display.setCursor(0,57); display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); display.print("Up/Down scroll  *=Back");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::BATTERY_CALIB) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Battery Cal"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        display.setCursor(0,14);
        if (!menu.batteryCalActive()) { display.println("Press # to start"); display.setCursor(0,26); display.println("*=Back"); }
        else {
            // Simple three-point editor: A0 (0%), A50 (50%), A100 (100%)
            int idx = menu.getEditCalibIndex();
            display.print("A0:"); display.print(menu.getEditCalib(0)); display.print("  ");
            display.print("A50:"); display.print(menu.getEditCalib(1)); display.print("  ");
            display.print("A100:"); display.println(menu.getEditCalib(2));
            // underline current
            int underlineY = 22; int startX = idx==0?0:(idx==1?36:80); int endX = startX + 28;
            display.drawLine(startX, underlineY, endX, underlineY, SSD1306_WHITE);
            display.setCursor(0,32);
            display.println("Up/Down chg  *=Next  #=Save");
        }
        return;
    } else {
        int start = menu.getVisibleStart();
        int lines = menu.getVisibleCount(5);
        const int rowH = 10;
        int yBase = 10;
        bool scrollAnim = menu.isScrollAnimating();
        bool selAnim = menu.isSelectionAnimating() && !scrollAnim; // suppress selection anim during scroll anim
        float scrollProgress = 1.0f;
        int prevScroll = menu.getPrevScrollOffset();
        int dir = menu.getScrollAnimDir();
        if (scrollAnim) {
            unsigned long dt = millis() - menu.getScrollAnimStart();
            if (dt > MenuSystem::SCROLL_ANIM_MS) dt = MenuSystem::SCROLL_ANIM_MS;
            scrollProgress = (float)dt / (float)MenuSystem::SCROLL_ANIM_MS;
        }
        auto drawList = [&](int baseOffset, int yShift){
            for (int i=0;i<lines;++i) {
                int idx = baseOffset + i;
                if (idx >= menu.getItemCount()) break;
                const MenuItem& it = menu.getItem(idx);
                int y = yBase + i*rowH + yShift;
                display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
                display.setCursor(2,y);
                display.print(it.label);
            }
        };
        if (scrollAnim) {
            int prevShift = (int)(-dir * rowH * scrollProgress + 0.5f);
            int newShift = (int)(dir * rowH * (1.0f - scrollProgress) + 0.5f);
            drawList(prevScroll, prevShift);
            drawList(start, newShift);
        } else {
            drawList(start, 0);
        }
        // Highlight selected row (new list coords)
        int selIdx = menu.getSelectedIndex();
        int rel = selIdx - start;
        if (rel >=0 && rel < lines) {
            int yTarget = yBase + rel*rowH;
            if (selAnim) {
                unsigned long dt = millis() - menu.getLastSelectionChangeTime();
                if (dt > MenuSystem::SELECTION_ANIM_MS) dt = MenuSystem::SELECTION_ANIM_MS;
                float p = (float)dt / (float)MenuSystem::SELECTION_ANIM_MS;
                int prevSel = menu.getPrevSelectedIndex();
                int prevRel = prevSel - start;
                if (prevRel>=0 && prevRel<lines) {
                    int yPrev = yBase + prevRel*rowH;
                    int yAnim = (int)( (float)yPrev + (float)(yTarget - yPrev)*p + 0.5f );
                    display.fillRect(0, yAnim-1, 128, rowH, SSD1306_WHITE);
                    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
                    display.setCursor(2,yAnim);
                    display.print(menu.getItem(selIdx).label);
                } else {
                    display.fillRect(0, yTarget-1, 128, rowH, SSD1306_WHITE);
                    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
                    display.setCursor(2,yTarget);
                    display.print(menu.getItem(selIdx).label);
                }
            } else {
                display.fillRect(0, yTarget-1, 128, rowH, SSD1306_WHITE);
                display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
                display.setCursor(2,yTarget);
                display.print(menu.getItem(selIdx).label);
            }
        }
        // Scroll indicators (up & down carets)
        if (start > 0) display.drawTriangle(120, 8, 125, 8, 122, 3, SSD1306_WHITE); // up caret (apex up)
        if (start + lines < menu.getItemCount()) display.drawTriangle(120, 57, 125, 57, 122, 62, SSD1306_WHITE); // down caret (apex down)
    }
}

void DisplayManager::drawMainScreen(const DeviceManager& deviceMgr, const BatteryMonitor& battery) const {
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    if (deviceMgr.getDeviceCount() == 0) {
        display.print("No devices");
        return;
    }
    const SlaveDevice* act = deviceMgr.getActive();
    if (!act) {
        display.print("No active");
        return;
    }
    unsigned long now = millis();
    bool fresh = (act->lastStatusMs != 0) && (now - act->lastStatusMs < 5000UL);
    if (!fresh) {
        display.setTextSize(1);
        display.setCursor(0, 12);
        display.print("Waiting for status...");
        display.setCursor(0, 24);
        display.print(act->name);
        return;
    }
    // Timers positioned like original using Defaults
    drawTimerRow((int)(act->toff*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_OFF, "OFF", Defaults::UI_TIMER_START_X);
    drawTimerRow((int)(act->ton*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_ON,  "ON",  Defaults::UI_TIMER_START_X);
    // Third line: elapsed time in current state (TIME), smoothed locally until next STATUS
    {
        unsigned long nowMs = millis();
        float since = (act->lastStatusMs > 0) ? ((nowMs - act->lastStatusMs) / 1000.0f) : 0.0f;
        float e = act->elapsed + since;
        float cap = act->outputState ? act->ton : act->toff;
        if (e > cap) e = cap;
        drawTimerRow((int)(e*10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_TIME,  "TIME",  Defaults::UI_TIMER_START_X);
    }
    // State indicator: show '*' at bottom-left when ON (matches original feel)
    display.setTextSize(2); if (act->outputState) { display.setCursor(0, Defaults::UI_STATE_CHAR_Y); display.print('*'); }
}

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
