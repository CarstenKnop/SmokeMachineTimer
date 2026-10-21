// FogMachineTimer - robust OFF/ON cycle timer for one smoke machine (Seeed XIAO ESP32-C3).
//
// Designed for a supply that disappears while the machine heats up and comes back later:
//  * the output pin is driven to its safe level before anything else happens;
//  * settings live in NVS (power-fail safe, CRC protected) and are written only when they change;
//  * the radio comes up on the stored channel within a few hundred ms and announces the (re)boot;
//  * the radio is serviced from one place (this loop) so protocol code never runs inside Wi-Fi callbacks;
//  * a watchdog resets the chip if the loop ever stalls (the output is off after any reset).
#include <Arduino.h>
#include <esp_task_wdt.h>

#include "EspTimerPlatform.h"
#include "FogJsonLine.h"
#include "FogLog.h"
#include "Pins.h"
#include "UsbConsole.h"

using namespace Fog;

static EspTimerPlatform platform;
static TimerNode node(platform);
static UsbConsole console(node, platform);
static bool outputState = false;
static BootRecord bootRecord;

static void driveOutput(bool on) {
    outputState = on;
    digitalWrite(FOG_OUTPUT_GPIO, (on == (FOG_OUTPUT_ACTIVE_HIGH != 0)) ? HIGH : LOW);
}

void setup() {
    // 1. Safe output level first - before USB, flash or radio.
    pinMode(FOG_OUTPUT_GPIO, OUTPUT);
    driveOutput(false);

    JsonLine::prepareSerial(Serial);
    Serial.begin(115200);

    // 2. Persistent state.
    bootRecord = beginBoot("fogtimer");              // counted in RAM now, stored once the supply has proven stable
    platform.life = bootRecord.life;
    const uint8_t reason = bootRecord.reason;
    platform.resetReason = reason;
    TimerSettings settings;
    if (!platform.loadSettings(settings)) {
        settings = TimerSettings();                     // first boot or damaged record: defaults
        platform.saveSettings(settings);
    }

    // 3. Radio + protocol.
    platform.begin(settings, 80);
    platform.tap = [](bool tx, const uint8_t* mac, int8_t rssi, const uint8_t* d, size_t n) { console.onFrame(tx, mac, rssi, d, n, millis()); };
    node.begin(millis(), settings, platform.life.boots);
    console.begin();

    // 4. Watchdog on the loop task: a stalled loop resets the chip (the output is off after any reset).
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 8000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
    esp_task_wdt_add(nullptr);

    FLOGI("boot", "FogTimer %s boot #%u reset=%s ch=%u on=%.1fs off=%.1fs", FOG_FW_STRING, (unsigned)platform.life.boots,
          resetReasonName(reason), settings.channel, settings.onTenths / 10.0, settings.offTenths / 10.0);
}

void loop() {
    const uint32_t now = millis();
    esp_task_wdt_reset();
    platform.loop.mark();

    platform.service(now, node);          // radio <-> protocol
    node.tick(now);                       // cycle engine, heartbeats, delayed channel change

    const bool want = node.outputOn(now);
    if (want != outputState) driveOutput(want);

    platform.updateLed(now, node, node.settings().led != 0);
    console.loop(now);
    if (bootRecord.dirty && now > 3000) commitBoot("fogtimer", bootRecord);   // not while a returning supply may still be chattering
    if (platform.rebootDue()) { driveOutput(false); delay(20); ESP.restart(); }
    delay(1);
}
