// DrawErrorScreen.cpp
#include "ui/DisplayManager.h"

void DisplayManager::drawErrorScreen() const {
    // Re-draw minimal error panel periodically in case previous content was overwritten
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0); display.println("OLED/I2C not found");
    display.setCursor(0, 12); display.println("Check wiring:");
    display.setCursor(0, 22); display.println("SDA=D4 (GPIO6)");
    display.setCursor(0, 32); display.println("SCL=D5 (GPIO7)");
    display.setCursor(0, 44); display.println("Addr 0x3C");
    display.display();
}
