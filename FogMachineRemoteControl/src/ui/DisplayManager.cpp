#include "ui/DisplayManager.h"

#include <Wire.h>
#include "Defaults.h"
#include "Pins.h"
#include "ui/Screens.h"

bool DisplayManager::begin() {
    // Adafruit_SSD1306::begin() reports success even without a display, so probe the I2C address ourselves.
    auto present = [](int sda, int scl) {
        Wire.begin(sda, scl);
        Wire.setClock(400000);
        Wire.beginTransmission(0x3C);
        return Wire.endTransmission() == 0;
    };
    bool ok = present(OLED_SDA_PIN, OLED_SCL_PIN);
    if (!ok) ok = present(OLED_SDA_PIN_ALT, OLED_SCL_PIN_ALT);        // some boards are wired to the alternate pair
    if (!ok) { inited_ = false; return false; }
    ok = display_.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
    if (!ok) { inited_ = false; return false; }
    inited_ = true;
    display_.setRotation(Defaults::OLED_ROTATION);
    display_.clearDisplay();
    screens::drawSplash(display_);
    display_.display();
    lastWakeMs_ = millis();
    haveHash_ = false;
    return true;
}

void DisplayManager::setContrast(uint8_t level) {
    if (level == contrast_) return;
    contrast_ = level;
    display_.ssd1306_command(SSD1306_SETCONTRAST);
    display_.ssd1306_command(level);
}

void DisplayManager::blankNow() {
    if (!inited_ || blanked_) return;
    blanked_ = true;
    display_.ssd1306_command(SSD1306_DISPLAYOFF);
}

void DisplayManager::wake() {
    lastWakeMs_ = millis();
    if (!inited_ || !blanked_) return;
    blanked_ = false;
    display_.ssd1306_command(SSD1306_DISPLAYON);
    haveHash_ = false;
}

void DisplayManager::flushIfChanged(bool force) {
    // FNV-1a over the frame buffer: ~1 ms, versus ~25 ms to push a frame over I2C.
    const uint8_t* buf = display_.getBuffer();
    uint32_t h = 2166136261u;
    for (int i = 0; i < 1024; ++i) { h ^= buf[i]; h *= 16777619u; }
    if (!force && haveHash_ && h == lastHash_) { ++skipped_; return; }
    lastHash_ = h;
    haveHash_ = true;
    const uint32_t t0 = micros();
    display_.display();
    lastFlushUs_ = micros() - t0;
    if (lastFlushUs_ > maxFlushUs_) maxFlushUs_ = lastFlushUs_;
    ++frames_;
}

void DisplayManager::render(const UiModel& model, const MenuState& menu, uint8_t brightness, uint16_t autoOffSeconds, bool activity, bool keepAwake) {
    if (!inited_) return;
    const uint32_t now = millis();
    if (activity || keepAwake) { lastWakeMs_ = now; if (blanked_) wake(); }
    if (autoOffSeconds > 0 && !keepAwake && !blanked_ && (now - lastWakeMs_) >= static_cast<uint32_t>(autoOffSeconds) * 1000UL) blankNow();
    if (blanked_) return;

    setContrast(menu.mode == MenuState::Mode::EDIT_BRIGHTNESS ? menu.editBrightness : brightness);   // live preview while editing

    display_.clearDisplay();
    if (menu.inMenu) screens::drawMenu(display_, model, menu);
    else screens::drawMain(display_, model);
    flushIfChanged(false);
}

void DisplayManager::showBootStatus(const char* msg) {
    if (!inited_) return;
    screens::drawBootStatus(display_, msg);
    display_.display();
}

void DisplayManager::showUpdateCountdown(uint8_t secondsRemaining) {
    if (!inited_) return;
    display_.clearDisplay();
    screens::drawUpdateCountdown(display_, secondsRemaining);
    display_.display();
}
