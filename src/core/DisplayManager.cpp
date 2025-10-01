#ifndef ARDUINO
#define F(x) x
#endif

// WiFi QR implementation notes:
// We generate a standards-compatible Wi-Fi join payload of the form
//   WIFI:T:<WPA|nopass>;S:<ssid>;P:<password>;;
// (hidden flag omitted). For now password not exposed by WiFiService, so
// open/nopass or WPA assumption placeholder. The trimmed qrcodegen encoder
// included under lib/qrcodegen is a minimized adaptation providing byte-mode
// encoding for small versions (1..10) with EC level selection. We choose
// ECC=MEDIUM and allow automatic version growth. Quiet zone is reduced to
// 2 modules due to 64px display height constraints; empirically phones are
// tolerant. If scanning proves unreliable, increase quiet to 3 and/or reduce
// title/header space.
#include "DisplayManager.h"
#include <cstring>

void DisplayManager::begin() {
    #if defined(QR_DUMP_SERIAL)
    Serial.begin(115200);
    unsigned long startWait = millis();
    while(!Serial && (millis()-startWait)<1500) { /* wait up to 1.5s */ }
    #endif
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { while (true) { delay(1000); } }
    // Contrast API not available in this library version; rely on default.
    display.clearDisplay(); display.display(); splash();
}

void DisplayManager::render(const TimerController& timerCtl, const MenuSystem& menu, const Config& config,
              bool blinkState, bool relayOn, uint32_t currentTimerTenths) {
    if (screensaver && screensaver->isBlanked()) return;
    display.clearDisplay();
    if (menu.getState() == MenuSystem::State::INACTIVE || menu.getState()==MenuSystem::State::PROGRESS || menu.getState()==MenuSystem::State::SELECT) {
        printTimerValue(config.get().offTime, 0, "OFF", timerCtl.inEdit()?timerCtl.getEditDigit():255, timerCtl.inEdit(), blinkState, true);
        printTimerValue(config.get().onTime, 24, "ON", timerCtl.inEdit()? (timerCtl.getEditDigit()>=Defaults::DIGITS? timerCtl.getEditDigit()-Defaults::DIGITS:255):255, timerCtl.inEdit() && timerCtl.getEditDigit()>=Defaults::DIGITS, blinkState, true);
        if (timerCtl.inEdit()) {
            display.setTextSize(2); display.setTextColor(WHITE,BLACK); display.setCursor(0,48); display.print("EDIT MODE");
        } else {
            if (relayOn) { display.setTextSize(2); display.setTextColor(WHITE,BLACK); display.setCursor(0,48); display.print('*'); }
            printTimerValue(currentTimerTenths, 48, "TIME", 255, false, false, false);
        }
        if (timerCtl.inEdit() && timerCtl.timersDirty) {
            display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('!');
        } else if (!timerCtl.inEdit() && (menu.getState()==MenuSystem::State::PROGRESS || menu.getState()==MenuSystem::State::SELECT || menu.getState()==MenuSystem::State::RESULT || menu.showMenuHint())) {
            display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('M');
        }

        // Draw bottom 1px progress bar during normal run (only when base screen visible and not editing)
        if (!timerCtl.inEdit() && menu.getState()==MenuSystem::State::INACTIVE) {
            // Determine phase length (on or off) in tenths; currentTimerTenths is tenths elapsed in current phase
            uint32_t phaseLen = relayOn ? config.get().onTime : config.get().offTime; // both stored in tenths
            if (phaseLen == 0) phaseLen = 1; // avoid div by zero
            float frac = (float)currentTimerTenths / (float)phaseLen;
            if (frac < 0) frac = 0; if (frac > 1) frac = 1;
            int filled = (int)(frac * 128.0f + 0.5f);
            if (filled>128) filled=128; if (filled<0) filled=0;
            // Clear bottom row then draw filled segment
            display.fillRect(0,63,128,1,BLACK);
            if (filled>0) display.fillRect(0,63,filled,1,WHITE);
        }

        // Transient clamp indicator (flash 'MIN' for ~1s after clamping zeros)
        if (timerCtl.recentlyClamped(millis())) {
            display.setTextSize(1); display.setTextColor(WHITE,BLACK);
            // Place near top-right but avoid overwriting menu hint / dirty flags; use bottom-left small box above progress bar
            int y = 54; int w=24; int h=8; int x=0;
            display.fillRect(x,y,w,h,WHITE);
            display.setTextColor(BLACK,WHITE);
            display.setCursor(x+2,y);
            display.print("MIN");
            display.setTextColor(WHITE,BLACK); // restore default
        }
    }
        switch(menu.getState()) {
            case MenuSystem::State::INACTIVE: break;
            case MenuSystem::State::PROGRESS: drawProgress(menu); break;
            case MenuSystem::State::SELECT: drawMenu(menu); break;
            case MenuSystem::State::RESULT: drawResult(menu); break;
            case MenuSystem::State::SAVER_EDIT: drawSaverEdit(menu, blinkState); break;
            case MenuSystem::State::WIFI_INFO: drawWiFiInfo(menu); break;
            case MenuSystem::State::QR_DYN: drawDynQR(menu); break;
            case MenuSystem::State::RICK: drawRick(menu); break;
            case MenuSystem::State::HELP: drawHelp(menu); break;
        }
    display.display();
}

void DisplayManager::splash() {
    display.setTextSize(1); display.setTextColor(WHITE); display.setCursor(0,0);
    display.print(Defaults::VERSION()); display.display(); delay(800); display.clearDisplay(); display.display();
}

void DisplayManager::printTimerValue(uint32_t value, int y, const char* label, int editDigit, bool editMode, bool blinkState, bool showDecimal, int startX) {
    char buf[8]; unsigned long integerPart = value/10; unsigned long frac = value%10; snprintf(buf,sizeof(buf),"%04lu%01lu", integerPart, frac);
    display.setTextSize(2); int digitWidth=11; int x=startX;
    for (int i=0;i<Defaults::DIGITS;i++) {
        bool inv = editMode && editDigit==i && blinkState; if (inv) { display.setTextColor(BLACK,WHITE); display.fillRect(x,y,digitWidth,16,WHITE);} else { display.setTextColor(WHITE,BLACK); display.fillRect(x,y,digitWidth,16,BLACK);} display.setCursor(x,y); display.print(buf[i]); if (i==Defaults::DIGITS-2) { display.print('.'); x+=digitWidth;} x+=digitWidth; }
    int labelX = startX + digitWidth*(Defaults::DIGITS+1)+10; display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(labelX,y+7); display.print(label); display.setTextSize(2);
}

void DisplayManager::drawProgress(const MenuSystem& menu) {
    unsigned long now = millis(); float prog = menu.progressFraction(now); bool full = menu.progressFull(now);
    if (prog > 0.0f || full) {
        int barX=0, barY=48, barW=128, barH=16;
        display.fillRect(barX,barY,barW,barH,BLACK);
        display.drawRect(barX,barY,barW,barH,WHITE);
        int fillW = (int)((barW-2)*prog);
        if (fillW>0) display.fillRect(barX+1,barY+1,fillW,barH-2,WHITE);
        if (full) {
            static bool blink=false; static unsigned long lastBlink=0; if (millis()-lastBlink>Defaults::MENU_FULL_BLINK_INTERVAL_MS){ blink=!blink; lastBlink=millis(); }
            if (blink) { const char* txt="MENU"; int txtWidth=4*12; int xTxt=barX+(barW-txtWidth)/2; int yTxt=barY+2; display.setTextColor(BLACK,WHITE); display.setCursor(xTxt,yTxt); display.print(txt);} }
    }
}

void DisplayManager::drawMenu(const MenuSystem& menu) {
    display.clearDisplay(); display.setTextSize(2);
    int count = menu.getMenuCount();
    if (count <= 0) return;
    float centerY=24.0f; float offset = menu.getScrollPos() - floor(menu.getScrollPos());
    int baseIndex = (int)floor(menu.getScrollPos());
    if (baseIndex < 0) baseIndex = 0; if (baseIndex >= count) baseIndex = count-1;
    for (int rel=-1; rel<=1; ++rel) {
    int idx = baseIndex + rel; if (idx < 0 || idx >= count) continue; float logicalRow = (float)rel - offset; float y = centerY + logicalRow*24.0f; int yi=(int)y; bool isSelected = (idx == menu.getMenuIndex()); const char* name = menu.getMenuName(idx);
        if (yi < -20 || yi > 64) continue; // cull offscreen
        if (isSelected) { display.fillRect(0,yi,128,20,WHITE); display.setTextColor(BLACK,WHITE); display.setCursor(0,yi); display.print("> "); display.print(name);} else { display.setTextColor(WHITE,BLACK); display.setCursor(0,yi); display.print("  "); display.print(name);} }
}

void DisplayManager::drawResult(const MenuSystem& menu) {
    display.clearDisplay(); display.setTextSize(2); display.setCursor(0,0); display.print("Selected"); display.setCursor(0,24); display.print("Menu "); display.print(menu.getSelectedMenu()+1);
}

void DisplayManager::drawSaverEdit(const MenuSystem& menu, bool blinkState) {
    display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0); display.print("Saver Delay s"); display.setTextSize(2); int startX=10; uint16_t val=menu.getEditingSaverValue(); if (val==0) {int boxW=60;int boxH=18; if (blinkState) { display.fillRect(startX,24,boxW,boxH,WHITE); display.setTextColor(BLACK,WHITE);} else { display.fillRect(startX,24,boxW,boxH,BLACK); display.setTextColor(WHITE,BLACK);} display.setCursor(startX+2,24); display.print("OFF");}
    else { char buf[6]; snprintf(buf,sizeof(buf),"%u",val); int len=strlen(buf); int digitWidth=11; int boxW=len*digitWidth+6; if (blinkState) { display.fillRect(startX,24,boxW,18,WHITE); display.setTextColor(BLACK,WHITE);} else { display.fillRect(startX,24,boxW,18,BLACK); display.setTextColor(WHITE,BLACK);} display.setCursor(startX+2,24); display.print(buf); display.setTextColor(WHITE,BLACK); display.setCursor(startX+boxW+2,24); display.print('s'); }
    display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(50,46); if (val==0) display.print("OFF"); else display.print("    "); display.setCursor(0,56); display.print("#=Save *=Cancel");
}

void DisplayManager::drawHelp(const MenuSystem& menu) {
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
    float basePos = menu.getHelpScrollPos();
    // Only draw lines that have their baseline within visible region ( -15 .. 63 )
    for (int line=0; line<menu.getHelpLines(); ++line) {
        float logicalY = (line - basePos) * 16.0f; // baseline
        if (logicalY < -15 || logicalY > 63) continue; // outside view for baseline
        int y = (int)logicalY;
        // clear a band for this line (height 16) to avoid remnants when overlapping during scroll
        display.fillRect(0, y, 126, 16, BLACK); // leave last 2 px for scrollbar only
        display.setCursor(0, y);
        display.print(menu.getHelpLine(line));
    }
    // Scroll indicator: rightmost 2 pixels column, proportional position
    int total = menu.getHelpLines();
    int visible = 4;
    float scrollF = menu.getHelpScrollPos();
    if (total > visible) {
        int trackX = 126; // columns 126-127
        int trackY = 0;
        int trackH = 64; // full height
        // compute thumb height (at least 4 px)
        int thumbH = (trackH * visible) / total;
        if (thumbH < 4) thumbH = 4;
        int maxScroll = total - visible;
        int thumbY = (maxScroll>0) ? (int)(((trackH - thumbH) * scrollF) / (float)maxScroll) : 0;
        // draw track background (optional left blank) -> just draw thumb
        display.fillRect(trackX, thumbY, 2, thumbH, WHITE);
    }
}

void DisplayManager::drawRick(const MenuSystem& menu) {
    (void)menu;
    display.clearDisplay();
    // QR-only layout (full screen for QR code)
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
#ifdef QR_STATIC_TEST_MODE
    // In static test mode we bypass WiFi service and render a fixed known URL (rickroll) to validate real-world scanning.
    #ifdef QR_TEST_V1
    const char* testPayload = "HELLO"; // Force very short payload to stay at version 1
    #else
    const char* testPayload = "https://youtu.be/dQw4w9WgXcQ"; // version 2 expected
    #endif
    if (strcmp(testPayload,lastQrPayload)!=0) {
        strncpy(lastQrPayload,testPayload,sizeof(lastQrPayload));
        // Prefer auto mask (-1); fallback to mask 0 if that somehow fails
    #ifdef QR_TEST_V1
        qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 1, -1, false);
        if (!qrValid) qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 1, 0, false);
    #else
        qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 2, -1, false);
        if (!qrValid) qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 2, 0, false);
    #endif
        lastQrSize = qrValid ? qrcodegen_getSize(qrBuffer) : 0;
        lastScale = 2;
        #if defined(QR_DUMP_ASCII)
        if (qrValid) {
            Serial.println(F("[QR-DUMP-BEGIN]"));
            Serial.print(F("VER=")); Serial.print(lastQrSize==21?1:(lastQrSize==25?2:lastQrSize));
            Serial.print(F(" SIZE=")); Serial.println(lastQrSize);
            for(int y=0;y<lastQrSize;y++){
                for(int x=0;x<lastQrSize;x++) Serial.print(qrcodegen_getModule(qrBuffer,x,y)?'#':'.');
                Serial.println();
            }
            Serial.println(F("[QR-DUMP-END]"));
        }
        #endif
    }
#else
    if (!wifi || !wifi->isStarted()) {
        display.setCursor(0,0); display.print(F("Starting AP..."));
        qrValid = false;
        return;
    }

    // Rebuild QR only if payload changed
    char payload[sizeof(lastQrPayload)];
    buildWifiQrString(payload,sizeof(payload));
    if (strcmp(payload,lastQrPayload)!=0) {
        strncpy(lastQrPayload,payload,sizeof(lastQrPayload));
        // Try ECC Medium first (more robust)
        qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_MEDIUM, 1, 2, -1, false);
        if (!qrValid) {
            // Fallback to ECC Low for extra capacity
            qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 2, -1, false);
        }
        lastQrSize = qrValid ? qrcodegen_getSize(qrBuffer) : 0;
        lastScale = 2; // fixed module size
    }
#endif
    if (!qrValid || lastQrSize==0) {
        display.setCursor(0,0); display.print(F("QR too big"));
        return;
    }

    const int scale = 2;
    const int maxW = 128;
    const int maxH = 64;
    // Compute maximum quiet zone that fits vertically for given size/scale.
    int maxQuietPossible = (maxH/scale - lastQrSize) / 2; // integer division
    #ifdef QR_TEST_V1
    // Try to use full quiet zone 4 if it fits for version 1 test
    if ((lastQrSize + 8) * scale <= maxH) maxQuietPossible = 4;
    #endif
    if (maxQuietPossible > 4) maxQuietPossible = 4; // no spec need beyond 4
    if (maxQuietPossible < 1) maxQuietPossible = 1; // at least 1 to separate edges
    int quiet = maxQuietPossible; // pick largest that fits to maximize finder isolation
    int totalModules = lastQrSize + 2*quiet;
    int qrPix = totalModules * scale;
    int fullLeft = (maxW - qrPix)/2; if (fullLeft<0) fullLeft=0;
    int fullTop = (maxH - qrPix)/2; if (fullTop<0) fullTop=0;
    int offX = fullLeft + quiet*scale;
    int offY = fullTop + quiet*scale;
    // Background: pure black outside, white square for quiet zone + code.
    display.fillRect(0,0,128,64,BLACK);
    display.fillRect(fullLeft, fullTop, qrPix, qrPix, WHITE);
    for(int y=0;y<lastQrSize;++y){
        for(int x=0;x<lastQrSize;++x){
            if (qrcodegen_getModule(qrBuffer,x,y))
                display.fillRect(offX + x*scale, offY + y*scale, scale, scale, BLACK);
        }
    }
    #ifdef QR_FORCE_FINDERS
    // Redraw canonical finder patterns to correct any accidental format-bit overwrite (diagnostic fix)
    auto drawFinderScaled = [&](int fx,int fy){
        for(int dy=0; dy<7; ++dy){
            for(int dx=0; dx<7; ++dx){
                bool dark = (dx==0||dx==6||dy==0||dy==6|| (dx>=2&&dx<=4&&dy>=2&&dy<=4));
                int gx = fx+dx; int gy = fy+dy; if (gx<0||gy<0||gx>=lastQrSize||gy>=lastQrSize) continue;
                int px = offX + gx*scale; int py = offY + gy*scale;
                // Paint background module first to remove any mask artifacts
                display.fillRect(px,py,scale,scale, dark?BLACK:WHITE);
            }
        }
    };
    drawFinderScaled(0,0);
    drawFinderScaled(lastQrSize-7,0);
    drawFinderScaled(0,lastQrSize-7);
    #endif
    #ifdef QR_DEBUG_OUTLINE
    // Draw a 1px outline just outside quiet zone for diagnostic framing
    display.drawRect(fullLeft-1, fullTop-1, qrPix+2, qrPix+2, WHITE);
    #endif
    // (Removed border rectangle to preserve clean quiet zone)
}

void DisplayManager::drawDynQR(const MenuSystem& menu) {
    (void)menu;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
    if (!wifi || !wifi->isStarted()) {
        display.setCursor(0,0); display.print(F("Starting AP..."));
        return;
    }
    char payload[sizeof(lastQrPayload)];
    buildWifiQrString(payload,sizeof(payload));
    if (strcmp(payload,lastQrPayload)!=0) {
        strncpy(lastQrPayload,payload,sizeof(lastQrPayload));
        qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_MEDIUM, 1, 2, -1, false);
        if (!qrValid) qrValid = qrcodegen_encodeText(lastQrPayload, qrTemp, qrBuffer, QR_ECC_LOW, 1, 2, -1, false);
        lastQrSize = qrValid ? qrcodegen_getSize(qrBuffer) : 0;
        lastScale = 2;
    }
    if (!qrValid || lastQrSize==0) { display.setCursor(0,0); display.print(F("QR err")); return; }
    int scale=2; int maxW=128; int maxH=64; int maxQuietPossible = (maxH/scale - lastQrSize) / 2; if (maxQuietPossible>4) maxQuietPossible=4; if (maxQuietPossible<1) maxQuietPossible=1; int quiet=maxQuietPossible; int totalModules= lastQrSize + 2*quiet; int qrPix= totalModules*scale; int fullLeft=(maxW-qrPix)/2; if(fullLeft<0) fullLeft=0; int fullTop=(maxH-qrPix)/2; if(fullTop<0) fullTop=0; int offX=fullLeft+quiet*scale; int offY=fullTop+quiet*scale; display.fillRect(0,0,128,64,BLACK); display.fillRect(fullLeft,fullTop,qrPix,qrPix,WHITE); for(int y=0;y<lastQrSize;++y){ for(int x=0;x<lastQrSize;++x){ if(qrcodegen_getModule(qrBuffer,x,y)) display.fillRect(offX+x*scale, offY+y*scale, scale, scale, BLACK);} }
}

void DisplayManager::drawWiFiInfo(const MenuSystem& menu) {
    (void)menu;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
    if (!wifi || !wifi->isStarted()) { display.setCursor(0,0); display.print(F("WiFi not started")); return; }
    const char* ssid = wifi->getSSID();
    const char* pass = wifi->getPass();
    display.setCursor(0,0); display.print(F("SSID:")); display.setCursor(0,8); display.print(ssid);
    display.setCursor(0,24); display.print(F("PASS:")); display.setCursor(0,32); if(pass && *pass) display.print(pass); else display.print(F("<open>"));
    // Removed back hint per request (navigation still via # or * )
}
void DisplayManager::escapeAppend(char c, char *&w, size_t &remain) {
    if (c=='\\' || c==';' || c==',' || c==':' || c=='"') {
        if (remain >= 2) { *w++='\\'; *w++=c; remain-=2; }
    } else if (remain >=1) { *w++=c; remain--; }
}

void DisplayManager::buildWifiQrString(char *out, size_t cap) const {
    if (!wifi) { if (cap>0) out[0]='\0'; return; }
    const char* ssid = wifi->getSSID();
    const char* pass = wifi->getPass();
    char *w = out; size_t remain = cap;
    auto put=[&](const char *s){ while(*s && remain>0){ *w++=*s++; remain--; } };
    put("WIFI:T:");
    if (pass && *pass) put("WPA"); else put("nopass");
    put(";S:");
    // Escape SSID
    for (const char* p=ssid; *p && remain>1; ++p) escapeAppend(*p,w,remain);
    if (pass && *pass) {
        put(";P:");
        for (const char* p=pass; *p && remain>1; ++p) escapeAppend(*p,w,remain);
    }
    put(";;");
    if (remain>0) *w='\0';
}
