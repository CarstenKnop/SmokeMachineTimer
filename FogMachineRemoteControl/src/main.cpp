// FogMachineRemoteControl - battery remote for up to 10 smoke-machine timers (Seeed XIAO ESP32-C3).
//
// Main loop, in this order, every pass:   buttons -> radio/fleet -> input handling -> PC bridge -> screen.
// Nothing blocks: radio callbacks only fill queues, the OLED is only flushed when its pixels change, and
// every command is a short-lived job with a deadline (see lib/FogLink/FogRemoteLink.h).
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

#include "FogIdent.h"
#include "FogLog.h"
#include "FogSys.h"
#include "Pins.h"
#include "battery/BatteryMonitor.h"
#include "config/RemoteSettings.h"
#include "link/Fleet.h"
#include "menu/MenuSystem.h"
#include "pc/PcBridge.h"
#include "ui/ButtonInput.h"
#include "ui/DisplayManager.h"
#include "ui/InputInterpreter.h"
#include "ui/UiModel.h"

using namespace Fog;

FOG_FW_IDENT("remote");                             // lets Fog Console recognise this image before flashing it

static RemoteSettings settings;
static Fleet fleet(settings);
static BatteryMonitor battery(BAT_ADC_PIN, settings);
static ButtonInput buttons(BUTTON_UP_GPIO, BUTTON_DOWN_GPIO, BUTTON_HASH_GPIO, BUTTON_STAR_GPIO);
static MenuSystem menu;
static InputInterpreter inputInterp;
static DisplayManager display;
static PcBridge bridge;
static LifeCounters life;
static BootRecord bootRecord;
static LoopMeter meter;
static uint8_t resetReason = 0;
static UiModel model;

static bool wokeFromSleep = false;
static uint32_t splashUntil = 0;
static bool prevInMenu = false;
static uint32_t sleepRequestedAt = 0;
static bool sleepNow = false;                      // 'Sleep Now' chosen in the menu
static bool starArmed = false, starHolding = false;   // '*' press that started on the main screen

// Survives deep sleep (not a power loss): keeps the command ids running so a new session cannot reuse a recent one.
RTC_DATA_ATTR static uint32_t rtcMagic = 0;
RTC_DATA_ATTR static uint8_t rtcCmdSeq = 0;
static constexpr uint32_t kRtcMagic = 0xF06C0DE5;

// ---------------------------------------------------------------------------------------------- helpers
static void applySettings() {
    fleet.applyTxPower(settings.get().txPowerQdbm);
}

static void commLed(uint32_t now) {
    static uint32_t lastTx = 0, lastRx = 0, offAt = 0;
    const FogRadio::Stats& rs = fleet.radio().stats();
    if (rs.tx != lastTx || rs.rx != lastRx) {
        lastTx = rs.tx; lastRx = rs.rx;
        digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? HIGH : LOW);
        offAt = now + Defaults::COMM_LED_MIN_ON_MS;
    } else if (offAt && static_cast<int32_t>(now - offAt) >= 0) {
        digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? LOW : HIGH);
        offAt = 0;
    }
}

static void buildModel(uint32_t now) {
    UiModel& m = model;
    m.now = now;
    m.batPercent = battery.percent();
    m.batMv = battery.millivolts();
    m.liveMv = menu.mode == MenuState::Mode::BATTERY_CALIB ? battery.readMillivoltsNow() : battery.millivolts();
    m.charging = battery.charging();
    m.usbPower = battery.usbPower();
    const RemoteSettingsData& s = settings.get();
    m.rssiLow = menu.mode == MenuState::Mode::EDIT_RSSI_CALIB ? menu.editRssiLow : s.rssiLowDbm;
    m.rssiHigh = menu.mode == MenuState::Mode::EDIT_RSSI_CALIB ? menu.editRssiHigh : s.rssiHighDbm;
    m.txPowerQdbm = s.txPowerQdbm;
    m.brightness = s.oledBrightness;
    m.autoOffSeconds = s.autoOffSeconds;
    m.deviceCount = fleet.getPairedCount();
    m.activeIndex = fleet.getActiveIndex();
    for (int i = 0; i < m.deviceCount; ++i) m.devices[i] = fleet.getPaired(i);
    m.discovering = fleet.isDiscovering();
    m.discoveredCount = 0;
    if (menu.mode == MenuState::Mode::PAIRING) {
        m.discoveredCount = fleet.getDiscoveredCount();
        if (m.discoveredCount > 16) m.discoveredCount = 16;
        for (int i = 0; i < m.discoveredCount; ++i) m.discovered[i] = fleet.getDiscovered(i);
    }
    m.searching = fleet.link().busyExcursion();
    m.homeChannel = fleet.homeChannel();
    if (menu.mode == MenuState::Mode::CHANNEL) {
        ChannelSurvey& sv = fleet.survey();
        m.surveyProgress = sv.progressPercent();
        for (uint8_t i = 0; i < Fog::kMaxChannel; ++i) {
            m.channelRows[i].channel = i + 1;
            m.channelRows[i].score = sv.channel(i + 1).score;
            m.channelRows[i].aps = sv.channel(i + 1).aps;
        }
        m.migOk = fleet.migrateOk();
        m.migFail = fleet.migrateFail();
        m.migTotal = fleet.migrateTotal();
        m.migResult = fleet.migrateResult();
    }
    if (menu.mode == MenuState::Mode::LINK_TEST) {
        const Fog::LinkTestResult& r = fleet.linkTestResult();
        m.linkTest.running = fleet.link().linkTesting();
        m.linkTest.sent = r.sent;
        m.linkTest.macAck = r.macAck;
        m.linkTest.echo = r.echo;
        m.linkTest.total = 40;
        m.linkTest.rttAvgMs = r.echo ? r.rttSumMs / r.echo : 0;
        m.linkTest.rttMaxMs = r.rttMax;
    }
    m.toast = fleet.toast(now);
    m.hashHoldMs = menu.isInMenu() ? 0 : buttons.hashHoldDuration();
    m.pcConnected = bridge.pcPresent(now);
}

// ---------------------------------------------------------------------------------------------- deep sleep
// ESP32-C3 can only wake from deep sleep on GPIO0..5 - on this board that is UP (GPIO3) and DOWN (GPIO4).
static void enterDeepSleep() {
    commitBoot("fogremote", bootRecord);
    rtcCmdSeq = fleet.link().cmdSeq();
    rtcMagic = kRtcMagic;
    display.blankNow();
    FLOGI("power", "deep sleep");
    gpio_wakeup_enable(static_cast<gpio_num_t>(BUTTON_UP_GPIO), GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(static_cast<gpio_num_t>(BUTTON_DOWN_GPIO), GPIO_INTR_LOW_LEVEL);
    gpio_pullup_en(static_cast<gpio_num_t>(BUTTON_UP_GPIO));
    gpio_pullup_en(static_cast<gpio_num_t>(BUTTON_DOWN_GPIO));
    gpio_deep_sleep_hold_en();
    esp_deep_sleep_enable_gpio_wakeup((1ULL << BUTTON_UP_GPIO) | (1ULL << BUTTON_DOWN_GPIO), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}

// ESP32-C3: GPIO9 ('#') is a strapping pin - a reset while it is held can enter the ROM download mode.
static void restartSafely() {
    const uint32_t t0 = millis();
    while (buttons.hashHeld() && millis() - t0 < 2000) { buttons.update(); delay(10); }
    ESP.restart();
}

// Sleeping happens only when the release of a held fog and every queued command have gone out and no key is down
// (a key still held would wake the chip again at once).
static bool readyToSleep(uint32_t now) {
    if (!sleepRequestedAt) {
        sleepRequestedAt = now ? now : 1;
        fleet.holdFog(false, now);                 // queue the release now; the loop keeps running until it is sent
    }
    const bool quiet = fleet.link().queuedJobs() == 0 && !buttons.anyHeld();
    return quiet || (now - sleepRequestedAt) > 3000;
}

// ---------------------------------------------------------------------------------------------- setup / loop
void setup() {
    gpio_deep_sleep_hold_dis();                       // release pins held during deep sleep
    JsonLine::prepareSerial(Serial);
    Serial.begin(115200);
    pinMode(COMM_OUT_GPIO, OUTPUT);
    digitalWrite(COMM_OUT_GPIO, Defaults::COMM_LED_ACTIVE_HIGH ? LOW : HIGH);

    wokeFromSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
    buttons.begin();
    display.begin();
    if (!wokeFromSleep) display.showBootStatus("Hold UP = flash mode");

    bootRecord = beginBoot("fogremote");
    life = bootRecord.life;
    resetReason = bootRecord.reason;
    settings.begin();
    battery.begin();
    display.wake();

    // Firmware-update window: UP held through power-up/reset (not when UP merely woke us from sleep).
    if (!wokeFromSleep && buttons.upHeld()) {
        const uint32_t t0 = millis();
        while (buttons.upHeld() && millis() - t0 < 1200) { buttons.update(); delay(10); }
        if (buttons.upHeld()) {
            const uint32_t end = millis() + 60000UL;
            while (static_cast<int32_t>(end - millis()) > 0) {
                display.showUpdateCountdown(static_cast<uint8_t>((end - millis() + 999) / 1000));
                buttons.update();
                if (buttons.starPressed()) break;
                delay(100);
            }
        }
    }

    fleet.begin(millis());
    if (wokeFromSleep && rtcMagic == kRtcMagic) fleet.link().setCmdSeq(static_cast<uint8_t>(rtcCmdSeq + 8));
    menu.begin(&fleet, &settings, &battery);

    PcBridge::Context ctx;
    ctx.fleet = &fleet;
    ctx.settings = &settings;
    ctx.battery = &battery;
    ctx.life = &life;
    ctx.meter = &meter;
    ctx.resetReason = resetReason;
    bridge.begin(ctx);
    bridge.onSettingsChanged = [] { applySettings(); menu.begin(&fleet, &settings, &battery); };
    applySettings();

    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 8000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
    esp_task_wdt_add(nullptr);
    splashUntil = wokeFromSleep ? 0 : millis() + 900;
    FLOGI("boot", "FogRemote %s boot #%u reset=%s", FOG_FW_STRING, (unsigned)life.boots, resetReasonName(resetReason));
}

void loop() {
    const uint32_t now = millis();
    esp_task_wdt_reset();
    meter.mark();

    buttons.update();
    if (sleepNow && (buttons.upPressed() || buttons.downPressed() || buttons.hashPressed() || buttons.starPressed())) sleepNow = false;   // any new key press cancels 'Sleep Now'
    fleet.loop(now);
    battery.update(now);
    fleet.setAwake(!display.isBlank());

    // ---- input -------------------------------------------------------------------------------------
    const bool inMenu = menu.isInMenu();
    const auto ev = inputInterp.update(buttons, inMenu);
    if (ev.longHash) menu.enterMenu();
    if (ev.shortHash) menu.enterEditTimers();

    if (!inMenu) {
        if (buttons.upPressed() || buttons.downPressed()) menu.enterSelectActive(true);

        // '*': click = toggle phase, hold = fog while held (dead-man lease, released = back to auto)
        if (buttons.starPressed()) { starArmed = true; starHolding = false; }
        if (starArmed && buttons.starHeld()) {
            if (buttons.starHoldDuration() >= Defaults::STAR_HOLD_THRESHOLD_MS) { starHolding = true; fleet.holdFog(true, now); }
        }
        if (starArmed && !buttons.starHeld()) {
            if (starHolding) fleet.holdFog(false, now);
            else fleet.toggleActive();
            starArmed = starHolding = false;
        }
    } else {
        fleet.holdFog(false, now);                    // never leave a forced output behind when the menu opens
        starArmed = starHolding = false;              // a '*' that started on the main screen must not act after the menu
    }

    if (inMenu) menu.update(buttons, now);       // a menu opened by this very key press must not also receive it
    if (prevInMenu && !menu.isInMenu()) inputInterp.resetOnMenuExit();
    prevInMenu = menu.isInMenu();

    if (menu.consumeSettingsApplied()) applySettings();
    if (menu.consumeRemoteReset()) {
        FLOGW("power", "factory reset of the remote");
        fleet.wipeLocalData();
        settings.resetToDefaults();
        delay(100);
        restartSafely();
    }
    if (menu.consumeReboot()) { delay(100); restartSafely(); }
    if (menu.consumeSleep()) { sleepNow = true; display.blankNow(); }

    // ---- PC link -------------------------------------------------------------------------------------
    bridge.setMenuName(menu.modeName());
    bridge.setBlank(display.isBlank());
    bridge.loop(now);
    const bool pc = bridge.pcPresent(now);

    // ---- screen --------------------------------------------------------------------------------------
    static uint32_t lastFrame = 0;
    if (now - lastFrame >= 33) {
        lastFrame = now;
        buildModel(now);
        if (static_cast<int32_t>(splashUntil - now) > 0) {
            // splash is already on the glass
        } else {
            display.render(model, menu, settings.get().oledBrightness, settings.get().autoOffSeconds,
                           buttons.anyHeld() || buttons.upPressed() || buttons.downPressed() || buttons.hashPressed() || buttons.starPressed(), pc);
        }
    }

    commLed(now);

    // ---- power -----------------------------------------------------------------------------------------
    const bool wantSleep = (sleepNow || display.isBlank()) && !pc;
    if (!wantSleep) { sleepRequestedAt = 0; sleepNow = false; }       // a key press (display woke) cancels a pending sleep
    else if (readyToSleep(now)) enterDeepSleep();
    if (bootRecord.dirty && now > 3000) commitBoot("fogremote", bootRecord);

    delay(1);
}
