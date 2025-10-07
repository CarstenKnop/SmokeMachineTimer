#include "DisplayManager.h"
#include "UI.h"
#include <Arduino.h>
#include <Wire.h>
#include "Defaults.h"

void DisplayManager::begin() {
  Wire.begin(Defaults::OLED_SDA, Defaults::OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { while(true) delay(1000); }
  display.clearDisplay();
  display.setRotation(Defaults::OLED_ROTATION);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("Remote Booting...");
  display.display();
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

void DisplayManager::render(const ESPNowMaster& master, const UI& ui) {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE);
  int y = 0; int i=0;

  if (ui.getState() == UI::State::PAIRING) {
    display.setCursor(0,0);
    display.println("Pair New Device");
    y += 10;
    // show simple countdown if available (ms left / 1000)
    display.setCursor(100,0);
    // Can't access ui->master directly; rely on discovered list present
    for (auto &p : master.discoveredPeers) {
      display.setCursor(0,y);
      display.printf("%02X:%02X:%02X:%02X:%02X:%02X", p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
      // show RSSI bars at right
      drawAntenna(120,y+5, (p.rssi + 100) / 20);
      if (i == ui.getSelectedIndex()) {
        display.drawRect(0, y - 1, 128, 10, WHITE);
      }
      y += 10; i++; if (y > 54) break;
    }
  } else if (ui.getState() == UI::State::EDIT_NAME) {
    display.setCursor(0,0);
    display.println("Name device:");
    display.setCursor(0,12);
    display.print(ui.getEditName());
    display.setCursor(0,24);
    display.println("Up/Down change first char");
    display.println("# to confirm");
  } else {
    for (auto &p : master.peers()) {
      display.setCursor(0,y); display.print(p.name);
      drawAntenna(96,y+6, (p.rssi + 100) / 20);
      drawBattery(104,y+10,p.battery);
      display.setCursor(0,y+8); display.printf("off:%u on:%u", p.offTime, p.onTime);
      if (i == ui.getSelectedIndex()) {
        display.drawRect(0, y - 1, 128, 18, WHITE);
      }
      y += 18; i++; if (y > 46) break;
    }
  }

  // show calibration of selected peer (if any) in top-right
  if (!master.peerList.empty() && ui.getSelectedIndex() < (int)master.peerList.size()) {
    auto &sp = master.peerList[ui.getSelectedIndex()];
    display.setCursor(80,0);
    display.setTextSize(1);
    display.printf("C:%u %u %u", sp.calibAdc[0], sp.calibAdc[1], sp.calibAdc[2]);
  }
  // Debug overlay: show button pressed indicators at bottom
  auto bs = ui.getLastButtons();
  int by = 56;
  display.fillRect(0, by, 6, 6, bs.up ? WHITE : BLACK);     // UP
  display.drawRect(0, by, 6, 6, WHITE);
  display.fillRect(10, by, 6, 6, bs.down ? WHITE : BLACK);  // DOWN
  display.drawRect(10, by, 6, 6, WHITE);
  display.fillRect(20, by, 6, 6, bs.hash ? WHITE : BLACK);  // #
  display.drawRect(20, by, 6, 6, WHITE);
  display.fillRect(30, by, 6, 6, bs.star ? WHITE : BLACK);  // *
  display.drawRect(30, by, 6, 6, WHITE);
  display.display();
}
