#ifndef ARDUINO
#define F(x) x
#endif
// Ensure Arduino types before using uint8_t/int16_t
#include <Arduino.h>
#include <pgmspace.h>
#include "DisplayManager.h"
#include "RickRollQrBitmap.h"
// Simple 12x8 monochrome icons (packed 1 byte per row, 12 LSB used) rough symbolic representations
const uint8_t DisplayManager::ICON_WIFI_AP[] PROGMEM         = { 0x3F,0x21,0x21,0x3F,0x04,0x0E,0x0E,0x04 }; // AP box + antenna
const uint8_t DisplayManager::ICON_WIFI_STA[] PROGMEM        = { 0x00,0x0E,0x11,0x00,0x04,0x0E,0x1F,0x04 }; // WiFi arcs + dot
const uint8_t DisplayManager::ICON_WIFI_DUAL[] PROGMEM       = { 0x3F,0x21,0x21,0x3F,0x0E,0x11,0x0E,0x04 }; // AP + STA combo
const uint8_t DisplayManager::ICON_WIFI_SUPPRESSED[] PROGMEM = { 0x0E,0x11,0x15,0x15,0x11,0x0E,0x04,0x1F }; // STA with bar
const uint8_t DisplayManager::ICON_WIFI_HOSTED[] PROGMEM     = { 0x3F,0x21,0x21,0x3F,0x1F,0x04,0x0E,0x04 }; // AP with host marker

void DisplayManager::drawIcon(int16_t x, int16_t y, const uint8_t *bitmap, uint8_t w, uint8_t h) {
    for (uint8_t row=0; row<h; ++row) {
        uint8_t bits = pgm_read_byte(bitmap + row);
        for (uint8_t col=0; col<w; ++col) {
            if (bits & (1 << (w-1-col))) {
                display.drawPixel(x+col, y+row, SSD1306_WHITE);
            }
        }
    }
}

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
// (includes moved earlier)
#include <cstring>
extern unsigned long netSetFlashUntil; // defined in main.cpp
extern unsigned long staFlashUntil; // station connect indicator window

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
        if (!timerCtl.inEdit()) {
            unsigned long nowMs = millis();
            if (nowMs < netSetFlashUntil) {
                display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(100,0); display.print(F("NET"));
            } else if (nowMs < staFlashUntil) {
                display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(100,0); display.print(F("STA"));
            }
        }
        bool drewStatusChar=false;
        if (timerCtl.inEdit() && timerCtl.timersDirty) {
            display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('!'); drewStatusChar=true;
        } else if (!timerCtl.inEdit() && (menu.getState()==MenuSystem::State::PROGRESS || menu.getState()==MenuSystem::State::SELECT || menu.getState()==MenuSystem::State::RESULT || menu.showMenuHint())) {
            display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('M'); drewStatusChar=true;
        }
        // Connectivity indicator (single char) if space not used
        if (!drewStatusChar) {
            // Advanced connectivity glyph logic using provided ConnectivityStatus snapshot.
            // Legend:
            //  W = STA connected & AP active (dual mode)
            //  S = STA connected & AP suppressed (STA-only)
            //  P = AP active only (no STA)
            //  X = AP suppressed, STA not connected (trying / down)
            //  A = AP forced always-on (override) & STA may/may not be connected (if connected shows as W)
            if (conn.wifiEnabled) {
                const uint8_t *icon = nullptr;
                if (conn.staConnected && conn.apActive) icon = ICON_WIFI_DUAL;
                else if (conn.staConnected && conn.apSuppressed) icon = ICON_WIFI_SUPPRESSED;
                else if (!conn.staConnected && conn.apActive) icon = (config.get().apAlwaysOn? ICON_WIFI_HOSTED : ICON_WIFI_AP);
                else if (!conn.staConnected && conn.apSuppressed) icon = ICON_WIFI_SUPPRESSED;
                if (icon) {
                    drawIcon(0,0, icon, 12,8);
                    // Overlay small activity marker (2x2 square) at top-right if clients present or recent auth
                    if (conn.apClients>0 || conn.recentAuth) {
                        // Blink only when recentAuth; steady if only clients
                        bool drawMarker=true;
                        if (conn.recentAuth) {
                            static unsigned long lastBlinkMs=0; static bool blinkOn=true;
                            unsigned long nowB = millis();
                            if (nowB - lastBlinkMs > 300) { blinkOn = !blinkOn; lastBlinkMs = nowB; }
                            drawMarker = blinkOn;
                        }
                        if (drawMarker) display.fillRect(10,0,2,2,SSD1306_WHITE);
                    }
                }
            }
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
            case MenuSystem::State::INFO: drawInfo(menu); break;
            case MenuSystem::State::WIFI_ENABLE_EDIT: {
                display.clearDisplay();
                display.setTextSize(1); display.setTextColor(WHITE,BLACK);
                display.setCursor(0,0); display.print(F("WiFi Enable"));
                display.setCursor(0,14); display.setTextSize(2);
                display.print(menu.wifiEnableTempValue()?F("ON "):F("OFF"));
                display.setTextSize(1); display.setCursor(0,48); display.print(F("Up/Down toggle"));
                display.setCursor(0,56); display.print(F("#=save *=cancel"));
                break; }
            case MenuSystem::State::WIFI_AP_ALWAYS_EDIT: {
                display.clearDisplay();
                display.setTextSize(1); display.setTextColor(WHITE,BLACK);
                display.setCursor(0,0); display.print(F("AP Always"));
                display.setCursor(0,14); display.setTextSize(2);
                display.print(menu.apAlwaysTempValue()?F("ON "):F("OFF"));
                display.setTextSize(1); display.setCursor(0,48); display.print(F("Up/Down toggle"));
                display.setCursor(0,56); display.print(F("#=save *=cancel"));
                break; }
            case MenuSystem::State::WIFI_RESET_CONFIRM: {
                display.clearDisplay();
                display.setTextSize(1); display.setTextColor(WHITE,BLACK);
                display.setCursor(0,0);
                display.print(F("Reset WiFi?"));
                display.setCursor(0,12);
                display.print(F("#=confirm"));
                display.setCursor(0,24);
                display.print(F("*=cancel"));
                if (menu.wifiResetActionDone()) {
                    display.setCursor(0,40); display.print(F("Done."));
                }
                break; }
            case MenuSystem::State::WIFI_FORGET_CONFIRM: {
                display.clearDisplay();
                display.setTextSize(1); display.setTextColor(WHITE,BLACK);
                display.setCursor(0,0);
                display.print(F("Forget STA?"));
                display.setCursor(0,12);
                display.print(F("#=confirm"));
                display.setCursor(0,24);
                display.print(F("*=cancel"));
                if (menu.wifiForgetActionDone()) {
                    display.setCursor(0,40); display.print(F("Done."));
                }
                break; }
            case MenuSystem::State::WIFI_ENABLE_TOGGLE: {
                display.clearDisplay(); display.setTextSize(1);
                display.setCursor(0,0); display.print(F("WiFi Toggled"));
                break; }
            case MenuSystem::State::WIFI_AP_ALWAYS_TOGGLE: {
                display.clearDisplay(); display.setTextSize(1);
                display.setCursor(0,0); display.print(F("AP Always Tgl"));
                break; }
        }
    display.display();
}

void DisplayManager::drawInfo(const MenuSystem& menu) {
    (void)menu;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
    int y=0;
    display.setCursor(0,y);   display.print(F("WiFi:")); display.print(conn.wifiEnabled?F("EN"):F("DIS"));
    y+=8; display.setCursor(0,y); display.print(F("AP:")); display.print(conn.apActive?F("UP"):F("--")); display.print('/'); display.print(conn.apSuppressed?F("S"):F("A"));
    y+=8; display.setCursor(0,y); display.print(F("STA:")); display.print(conn.staConnected?F("OK"):F("--")); display.print(' '); display.print(conn.staRssi);
    y+=8; display.setCursor(0,y); display.print(F("Cli:")); display.print(conn.apClients); display.print(' '); display.print(conn.recentAuth?F("A"):F("-"));
    // Uptime in seconds (approx)
    unsigned long up = millis()/1000UL; unsigned long um=up/60UL; unsigned long uh=um/60UL; unsigned long ud=uh/24UL;
    y+=8; display.setCursor(0,y); display.print(F("Up:")); if (ud>0){display.print(ud);display.print('d');} display.print((uh%24)); display.print('h');
    // Free heap (ESP specific) guarded by ifdef
    #ifdef ESP32
    y+=8; display.setCursor(0,y); display.print(F("Heap:")); display.print(ESP.getFreeHeap()/1024); display.print('K');
    #endif
    // Version on last line if space
    if (y<=48) { y+=8; display.setCursor(0,y); display.print(F("Ver:")); display.print(Defaults::VERSION()); }
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
    display.setTextSize(1); display.setTextColor(WHITE,BLACK);
    // Use static pre-generated bitmap for Rick-roll URL (Version 2, 25x25) with standard dark=black modules on white background.
    const int modules = RICK_QR_SIZE; // 25
    const int scale = 2; // each module 2x2 pixels
    const int qrPix = modules * scale; // 50 pixels
    const int quiet = 2 * scale; // simple fixed quiet zone (4px) around code
    display.fillRect(0,0,128,64,BLACK); // full screen black
    int fullW = qrPix + quiet*2;
    int fullH = qrPix + quiet*2;
    int left = (128 - fullW)/2; if (left<0) left=0;
    int top  = (64 - fullH)/2; if (top<0) top=0;
    // White background (quiet zone + code area)
    display.fillRect(left, top, fullW, fullH, WHITE);
    int offX = left + quiet;
    int offY = top + quiet;
    for (int y=0; y<modules; ++y) {
        for (int x=0; x<modules; ++x) {
            uint8_t v = pgm_read_byte(&RICK_QR_BITMAP[y * modules + x]);
            if (v) {
                display.fillRect(offX + x*scale, offY + y*scale, scale, scale, BLACK);
            }
        }
    }
    // Optional outline for diagnostics (commented)
    // display.drawRect(left-1, top-1, fullW+2, fullH+2, WHITE);
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
    if (!wifi || !wifi->isStarted()) { display.setCursor(0,0); display.print(F("WiFi off (toggle)")); return; }
    const char* ssid = wifi->getSSID();
    const char* pass = wifi->getPass();
    display.setCursor(0,0); display.print(F("AP SSID:")); display.setCursor(0,8); display.print(ssid);
    display.setCursor(0,18); display.print(F("AP PASS:")); display.setCursor(0,26); if(pass && *pass) display.print(pass); else display.print(F("<open>"));
    // Space for STA info if available via future injection (placeholder lines)
    display.setCursor(0,40); display.print(F("#/* back"));
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
