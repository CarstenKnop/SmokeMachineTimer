#include <Arduino.h>
#include "ESPNowMaster.h"
#include "DisplayManager.h"
#include "UI.h"

ESPNowMaster master;
DisplayManager disp;
UI ui;

void setup() {
  Serial.begin(115200);
  master.begin();
  disp.begin();
  ui.begin(&master, &disp);
}

void loop() {
  master.scanAndPing();
  ui.loop();
  delay(200);
}
