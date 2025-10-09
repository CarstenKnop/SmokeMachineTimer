// DrawMenu.cpp
#include "ui/DisplayManager.h"
#include "Defaults.h"
#include "comm/CommManager.h"
#include <cstring>
#include <cstdio>

void DisplayManager::drawMenu(const MenuSystem& menu, const DeviceManager& deviceMgr) const {
    display.setTextSize(1);
    if (menu.isEditingBlanking()) {
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
        display.setCursor(0, 54);
        int applied = menu.getAppliedBlankingSeconds();
        display.print("Active: "); if (applied==0) display.print("OFF"); else { display.print(applied); display.print("s"); }
        return;
    } else if (menu.isEditingTxPower()) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("WiFi TX Power"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        display.setCursor(0,16); display.print("Level: "); display.print((int)menu.getEditingTxPowerQdbm()); display.println(" qdBm");
        display.setCursor(0,28); display.println("Up/Down change");
        display.setCursor(0,40); display.println("#=Save  *=Back");
        return;
    } else if (menu.isEditingBrightness()) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("OLED Brightness"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        uint8_t lvl = menu.getEditingOledBrightness();
        display.ssd1306_command(SSD1306_SETCONTRAST);
        display.ssd1306_command(lvl);
        display.setCursor(0,16); display.print("Level: "); display.print((int)lvl);
        display.setCursor(0,28); display.println("Up/Down change");
        display.setCursor(0,40); display.println("#=Save  *=Back");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::EDIT_TIMERS) {
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
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Pair Timer"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        auto *comm = CommManager::get();
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
            bool already=false; for(int p=0;p<deviceMgr.getDeviceCount();++p){ if (memcmp(deviceMgr.getDevice(p).mac,d.mac,6)==0){already=true;break;} }
            char line[32]; snprintf(line,sizeof(line),"%c%s %s",already?'*':' ', macBuf, d.name[0]?d.name:"(noname)");
            display.print(line);
        }
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
        if (count>0) {
            const auto &d = comm->getDiscovered(sel);
            bool already=false; for(int p=0;p<deviceMgr.getDeviceCount();++p){ if (memcmp(deviceMgr.getDevice(p).mac,d.mac,6)==0){already=true;break;} }
            display.setCursor(0,54); display.print("#="); display.print(already?"Unpair":"Pair"); display.print(" *=Back");
        } else {
            display.setCursor(0,14); display.println("Scanning...");
            display.setCursor(0,26); display.println("*=Back");
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
            display.setTextSize(2);
            const char* buf = menu.getRenameBuffer();
            int pos = menu.getRenamePos();
            const int charW = 12; const int charH = 16; const int y = 14;
            display.fillRect(0, y, 128, charH, SSD1306_BLACK);
            for (int i = 0; buf[i] != '\0'; ++i) {
                int x = i * charW;
                bool inv = (i == pos);
                if (inv) { display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); display.fillRect(x, y, charW, charH, SSD1306_WHITE); }
                else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); }
                display.setCursor(x, y);
                display.print(buf[i]);
            }
            display.setTextSize(1);
            display.setCursor(0,48); display.setTextColor(SSD1306_WHITE); display.print("Up/Down change  #=Next  *=Back");
            return;
        }
    } else if (menu.getMode() == MenuSystem::Mode::EDIT_NAME) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Edit Name"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        display.setTextSize(2);
        {
            const char* buf = menu.getRenameBuffer();
            int pos = menu.getRenamePos();
            const int charW = 12; const int charH = 16; const int y = 14;
            display.fillRect(0, y, 128, charH, SSD1306_BLACK);
            for (int i = 0; buf[i] != '\0'; ++i) {
                int x = i * charW;
                bool inv = (i == pos);
                if (inv) { display.setTextColor(SSD1306_BLACK, SSD1306_WHITE); display.fillRect(x, y, charW, charH, SSD1306_WHITE); }
                else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK); }
                display.setCursor(x, y);
                display.print(buf[i]);
            }
        }
        display.setTextSize(1);
        display.setCursor(0,48); display.setTextColor(SSD1306_WHITE); display.print("Up/Down change  #=Next  *=Back");
        return;
    } else if (menu.getMode() == MenuSystem::Mode::SELECT_ACTIVE) {
        display.setCursor(0,0); display.setTextColor(SSD1306_WHITE); display.println("Active Timer"); display.drawLine(0,9,127,9,SSD1306_WHITE);
        int count = deviceMgr.getDeviceCount();
        if (count == 0) { display.setCursor(0,14); display.println("No devices"); display.setCursor(0,26); display.println("*=Back"); return; }
        int sel = menu.getActiveSelectIndex();
        if (sel >= count) sel = count-1; if (sel < 0) sel = 0;
        int activeIdx = deviceMgr.getActiveIndex();
        int first = 0; if (sel >= 4) first = sel-3;
        for (int i=0;i<4 && first+i<count;i++) {
            int idx = first + i;
            const auto &d = deviceMgr.getDevice(idx);
            bool highlight = (idx==sel);
            int y = 12 + i*12;
            if (highlight) { display.fillRect(0,y,128,10,SSD1306_WHITE); display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);} else { display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);}                
            display.setCursor(2,y);
            char line[32];
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
    } else {
        int start = menu.getVisibleStart();
        int lines = menu.getVisibleCount(5);
        const int rowH = 10;
        int yBase = 10;
        bool scrollAnim = menu.isScrollAnimating();
        bool selAnim = menu.isSelectionAnimating() && !scrollAnim;
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
        if (start > 0) display.drawTriangle(120, 8, 125, 8, 122, 3, SSD1306_WHITE);
        if (start + lines < menu.getItemCount()) display.drawTriangle(120, 57, 125, 57, 122, 62, SSD1306_WHITE);
    }
}
