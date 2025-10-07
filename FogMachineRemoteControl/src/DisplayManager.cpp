#include "DisplayManager.h"
#include <Arduino.h>

void DisplayManager::begin() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { while(true) delay(1000); }
  display.clearDisplay(); display.display();
}

void DisplayManager::drawAntenna(int x,int y,int strength) {
  for (int i=0;i<4;i++) {
    int h = (i+1)*3;
    int w = 3 + i*3;
    if (i < strength) display.drawRect(x - w/2, y - h, w, h, WHITE);
  }
}

void DisplayManager::drawBattery(int x,int y,int pct) {
  display.drawRect(x,y-8,22,10,WHITE);
  display.fillRect(x+20,y-6,2,6,WHITE);
  int w = (int)((pct/100.0f)*18.0f + 0.5f);
  if (w>0) display.fillRect(x+2,y-6,w,6,WHITE);
}

void DisplayManager::render(const ESPNowMaster& master) {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE);
  int y = 0; int i=0;
  for (auto &p : master.peers()) {
    display.setCursor(0,y); display.print(p.name);
    drawAntenna(96,y+6, (p.rssi + 100) / 20);
    drawBattery(104,y+10,p.battery);
    display.setCursor(0,y+8); display.printf("off:%u on:%u", p.offTime, p.onTime);
    y += 18; i++; if (y > 46) break;
  }
    // show calibration of selected peer (if any) in top-right
    if (!master.peerList.empty()) {
      auto &sp = master.peerList[0]; // simple: show first peer's calib for now
      display.setCursor(80,0);
      display.setTextSize(1);
      display.printf("C:%u %u %u", sp.calibAdc[0], sp.calibAdc[1], sp.calibAdc[2]);
    }
  display.display();
}
