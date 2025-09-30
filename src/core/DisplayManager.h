#pragma once
#include <Adafruit_SSD1306.h>
#include "Defaults.h"
#include "TimerController.h"
#include "MenuSystem.h"
#include "Config.h"
#include "Screensaver.h"

class DisplayManager {
public:
  void begin() {
    // Assumes Wire already begun externally
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      while (true) { delay(1000); }
    }
    display.clearDisplay(); display.display();
    splash();
  }

  Adafruit_SSD1306* get() { return &display; }

  void render(const TimerController& timerCtl, const MenuSystem& menu, const Config& config,
              bool blinkState, bool relayOn, uint32_t currentTimerTenths) {
  if (screensaver && screensaver->isBlanked()) return; // nothing while blanked
  display.clearDisplay();
    // (Removed debug state marker previously displayed in top-right)
    // Decide what to show based on states
    if (menu.getState() == MenuSystem::State::INACTIVE || menu.getState()==MenuSystem::State::PROGRESS || menu.getState()==MenuSystem::State::SELECT) {
      // Show timers unless saver edit or result occupies full screen
      printTimerValue(config.get().offTime, 0, "OFF", timerCtl.inEdit()?timerCtl.getEditDigit():255, timerCtl.inEdit(), blinkState, true);
      printTimerValue(config.get().onTime, 24, "ON", timerCtl.inEdit()? (timerCtl.getEditDigit()>=Defaults::DIGITS? timerCtl.getEditDigit()-Defaults::DIGITS:255):255, timerCtl.inEdit() && timerCtl.getEditDigit()>=Defaults::DIGITS, blinkState, true);
      // Indicators / third line
      if (timerCtl.inEdit()) {
        // EDIT MODE banner similar to legacy implementation
        display.setTextSize(2); display.setTextColor(WHITE,BLACK); display.setCursor(0,48); display.print("EDIT MODE");
      } else {
        // Relay state indicator (*) and runtime timer
        if (relayOn) { display.setTextSize(2); display.setTextColor(WHITE,BLACK); display.setCursor(0,48); display.print('*'); }
        printTimerValue(currentTimerTenths, 48, "TIME", 255, false, false, false);
      }
      // Unsaved changes indicator '!' upper-left when editing & dirty
      // Top-left indicators precedence: '!' (dirty edit) > 'M' (menu) > none
      if (timerCtl.inEdit() && timerCtl.timersDirty) {
        display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('!');
      } else if (!timerCtl.inEdit() && (menu.getState()==MenuSystem::State::PROGRESS || menu.getState()==MenuSystem::State::SELECT || menu.getState()==MenuSystem::State::RESULT || menu.showMenuHint())) {
        display.setTextSize(2); display.setCursor(0,0); display.setTextColor(WHITE,BLACK); display.print('M');
      }
    }
    // Overlays for states
    switch(menu.getState()) {
      case MenuSystem::State::INACTIVE: break;
      case MenuSystem::State::PROGRESS: drawProgress(menu); break;
      case MenuSystem::State::SELECT: drawMenu(menu); break;
      case MenuSystem::State::RESULT: drawResult(menu); break;
      case MenuSystem::State::SAVER_EDIT: drawSaverEdit(menu, blinkState); break;
    }
    display.display();
  }

  void attachScreensaver(Screensaver* s) { screensaver = s; }

private:
  void splash() {
    display.setTextSize(1); display.setTextColor(WHITE); display.setCursor(0,0);
    display.print(Defaults::VERSION()); display.display(); delay(800); display.clearDisplay(); display.display();
  }

  void printTimerValue(uint32_t value, int y, const char* label, int editDigit, bool editMode, bool blinkState, bool showDecimal, int startX=26) {
    char buf[8]; unsigned long integerPart = value/10; unsigned long frac = value%10; snprintf(buf,sizeof(buf),"%04lu%01lu", integerPart, frac);
    display.setTextSize(2); int digitWidth=11; int x=startX;
    for (int i=0;i<Defaults::DIGITS;i++) {
      bool inv = editMode && editDigit==i && blinkState; if (inv) { display.setTextColor(BLACK,WHITE); display.fillRect(x,y,digitWidth,16,WHITE);} else { display.setTextColor(WHITE,BLACK); display.fillRect(x,y,digitWidth,16,BLACK);} display.setCursor(x,y); display.print(buf[i]); if (i==Defaults::DIGITS-2) { display.print('.'); x+=digitWidth;} x+=digitWidth; }
    int labelX = startX + digitWidth*(Defaults::DIGITS+1)+10; display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(labelX,y+7); display.print(label); display.setTextSize(2);
  }

  void drawProgress(const MenuSystem& menu) {
    unsigned long now = millis(); float prog = menu.progressFraction(now); bool full = menu.progressFull(now);
    if (prog > 0.0f || full) {
      int barX=0, barY=48, barW=128, barH=16;
      // Fill background black so area is cleared/solid when bar first appears
      display.fillRect(barX,barY,barW,barH,BLACK);
      display.drawRect(barX,barY,barW,barH,WHITE);
      int fillW = (int)((barW-2)*prog);
      if (fillW>0) display.fillRect(barX+1,barY+1,fillW,barH-2,WHITE);
      if (full) {
        static bool blink=false; static unsigned long lastBlink=0; if (millis()-lastBlink>Defaults::MENU_FULL_BLINK_INTERVAL_MS){ blink=!blink; lastBlink=millis(); }
        if (blink) { const char* txt="MENU"; int txtWidth=4*12; int xTxt=barX+(barW-txtWidth)/2; int yTxt=barY+2; display.setTextColor(BLACK,WHITE); display.setCursor(xTxt,yTxt); display.print(txt);} }
    }
  }
  void drawMenu(const MenuSystem& menu) {
    display.clearDisplay(); display.setTextSize(2);
    float centerY=24.0f; float offset = menu.getScrollPos() - floor(menu.getScrollPos()); int baseIndex = (int)floor(menu.getScrollPos()) % MenuSystem::MENU_COUNT; if (baseIndex<0) baseIndex+=MenuSystem::MENU_COUNT;
    for (int rel=-1; rel<=1; ++rel) {
      int idx = (baseIndex + rel + MenuSystem::MENU_COUNT) % MenuSystem::MENU_COUNT; float logicalRow = (float)rel - offset; float y = centerY + logicalRow*24.0f; int yi=(int)y; bool isSelected = fabs(logicalRow) < 0.5f; const char* name = MenuSystem::MENU_NAMES[idx];
      if (isSelected) { display.fillRect(0,yi,128,20,WHITE); display.setTextColor(BLACK,WHITE); display.setCursor(0,yi); display.print("> "); display.print(name);} else { display.setTextColor(WHITE,BLACK); display.setCursor(0,yi); display.print("  "); display.print(name);} }
  }
  void drawResult(const MenuSystem& menu) {
    display.clearDisplay(); display.setTextSize(2); display.setCursor(0,0); display.print("Selected"); display.setCursor(0,24); display.print("Menu "); display.print(menu.getSelectedMenu()+1);
  }
  void drawSaverEdit(const MenuSystem& menu, bool blinkState) {
    display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0); display.print("Saver Delay s"); display.setTextSize(2); int startX=10; uint16_t val=menu.getEditingSaverValue(); if (val==0) {int boxW=60;int boxH=18; if (blinkState) { display.fillRect(startX,24,boxW,boxH,WHITE); display.setTextColor(BLACK,WHITE);} else { display.fillRect(startX,24,boxW,boxH,BLACK); display.setTextColor(WHITE,BLACK);} display.setCursor(startX+2,24); display.print("OFF");}
    else { char buf[6]; snprintf(buf,sizeof(buf),"%u",val); int len=strlen(buf); int digitWidth=11; int boxW=len*digitWidth+6; if (blinkState) { display.fillRect(startX,24,boxW,18,WHITE); display.setTextColor(BLACK,WHITE);} else { display.fillRect(startX,24,boxW,18,BLACK); display.setTextColor(WHITE,BLACK);} display.setCursor(startX+2,24); display.print(buf); display.setTextColor(WHITE,BLACK); display.setCursor(startX+boxW+2,24); display.print('s'); }
    display.setTextSize(1); display.setTextColor(WHITE,BLACK); display.setCursor(50,46); if (val==0) display.print("OFF"); else display.print("    "); display.setCursor(0,56); display.print("#=Save *=Cancel");
  }

  Adafruit_SSD1306 display{128,64,&Wire,-1}; Screensaver* screensaver=nullptr;
};
