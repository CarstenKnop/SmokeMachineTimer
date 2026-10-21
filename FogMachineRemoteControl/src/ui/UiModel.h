// UiModel.h - a plain snapshot of everything the screens display. Built once per frame by main.cpp from the
// fleet, battery, settings and menu; the screen code only reads it. This keeps the drawing code free of
// hardware so the very same functions render the pictures in the manual (tools/oled-preview).
#pragma once

#include <stdint.h>
#include "FogProto.h"
#include "link/FleetTypes.h"
#include "menu/MenuState.h"

struct ChannelRow {
    uint8_t channel = 0;
    uint8_t score = 0;       // 0..100, lower = quieter
    uint8_t aps = 0;
};

struct LinkTestView {
    bool started = false;
    bool running = false;
    uint16_t sent = 0, macAck = 0, echo = 0, total = 40;
    uint32_t rttAvgMs = 0, rttMaxMs = 0;
};

struct UiModel {
    uint32_t now = 0;

    // battery / power
    uint8_t batPercent = 100;
    uint16_t batMv = 4000;
    uint16_t liveMv = 4000;
    bool charging = false;
    bool usbPower = false;

    // settings the screens show
    int8_t rssiLow = -100, rssiHigh = -80;
    int8_t txPowerQdbm = 80;
    uint8_t brightness = 255;
    uint16_t autoOffSeconds = 60;

    // fleet
    int deviceCount = 0;
    int activeIndex = 0;
    SlaveDevice devices[Fog::kMaxTimers];
    int discoveredCount = 0;
    DiscoveredDevice discovered[16];
    bool discovering = false;
    bool searching = false;          // lost-timer search / recovery in progress

    // channel tools
    uint8_t homeChannel = 1;
    uint8_t surveyProgress = 0;
    ChannelRow channelRows[Fog::kMaxChannel];
    uint8_t migOk = 0, migFail = 0, migTotal = 0;
    int8_t migResult = -1;
    LinkTestView linkTest;

    // transient UI
    const char* toast = nullptr;
    uint32_t hashHoldMs = 0;         // '#' held on the main screen (menu-entry progress bar)
    bool pcConnected = false;
    bool holdingFog = false;         // '*' currently held (dead-man fog)

    const SlaveDevice* active() const { return (activeIndex >= 0 && activeIndex < deviceCount) ? &devices[activeIndex] : nullptr; }
};
