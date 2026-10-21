// DisplayManager.h - owns the SSD1306 OLED: init, blanking, contrast and efficient flushing.
// What is drawn comes from ui/Screens.*; a frame is only sent to the display when its pixels changed,
// which keeps the I2C bus (and the loop) quiet while a screen is static.
#pragma once

#include <Adafruit_SSD1306.h>
#include "menu/MenuState.h"
#include "ui/UiModel.h"

class DisplayManager {
public:
    DisplayManager() : display_(128, 64, &Wire, -1) {}

    bool begin();
    // Draws the current screen. activity = a button is down / was pressed (wakes a blanked display).
    void render(const UiModel& model, const MenuState& menu, uint8_t brightness, uint16_t autoOffSeconds, bool activity, bool keepAwake);
    bool isOk() const { return inited_; }
    bool isBlank() const { return blanked_; }
    void blankNow();
    void wake();
    void showBootStatus(const char* msg);
    void showUpdateCountdown(uint8_t secondsRemaining);

    // frame statistics for the diagnostics
    uint32_t lastFlushUs() const { return lastFlushUs_; }
    uint32_t maxFlushUs() const { return maxFlushUs_; }
    uint32_t frames() const { return frames_; }
    uint32_t skipped() const { return skipped_; }

private:
    void setContrast(uint8_t level);
    void flushIfChanged(bool force);

    Adafruit_SSD1306 display_;
    bool inited_ = false;
    bool blanked_ = false;
    uint32_t lastWakeMs_ = 0;
    uint8_t contrast_ = 0;
    uint32_t lastHash_ = 0;
    bool haveHash_ = false;
    uint32_t lastFlushUs_ = 0, maxFlushUs_ = 0, frames_ = 0, skipped_ = 0;
};
