// oled_preview - renders the REAL firmware screens (src/ui/Screens.cpp) on a PC and writes PNG pictures.
// Used for the pictures in docs/MANUAL.md and as a visual regression check of the UI.
//   powershell -File tools/oled-preview/run.ps1
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Adafruit_GFX.h"
#include "menu/MenuState.h"
#include "ui/Screens.h"
#include "ui/UiModel.h"

// ------------------------------------------------------------------------------------------- canvas
class Canvas : public Adafruit_GFX {
public:
    uint8_t px[64][128];
    Canvas() : Adafruit_GFX(128, 64) { clear(); }
    void clear() { memset(px, 0, sizeof(px)); }
    void drawPixel(int16_t x, int16_t y, uint16_t c) override {
        if (x >= 0 && x < 128 && y >= 0 && y < 64) px[y][x] = c ? 1 : 0;
    }
};

// ------------------------------------------------------------------------------------------- PNG writer (no deps)
static uint32_t crcTable[256];
static void initCrc() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}
static uint32_t crc32(uint32_t crc, const uint8_t* d, size_t n) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = crcTable[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
static void put32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = crc32(0, out.data() + 4, out.size() - 4);
    put32(out, crc);
    fwrite(out.data(), 1, out.size(), f);
}
static bool writePng(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
    initCrc();
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, w); put32(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    put32(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

// Draws the 128x64 canvas like a real 0.96" OLED: dark glass, slightly soft cyan-white pixels with gaps.
static void savePicture(const Canvas& c, const std::string& path, bool bezel) {
    const int S = 6, pad = bezel ? 24 : 0;
    const int w = 128 * S + 2 * pad, h = 64 * S + 2 * pad;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) { uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3]; p[0] = r; p[1] = g; p[2] = b; };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) put(x, y, 28, 31, 36);              // PCB / bezel
    for (int y = 0; y < 64 * S; ++y) for (int x = 0; x < 128 * S; ++x) put(pad + x, pad + y, 4, 6, 10);   // glass
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (!c.px[y][x]) continue;
            for (int dy = 0; dy < S - 1; ++dy) for (int dx = 0; dx < S - 1; ++dx) {
                const bool edge = (dx == 0 || dy == 0 || dx == S - 2 || dy == S - 2);
                put(pad + x * S + dx, pad + y * S + dy, edge ? 150 : 205, edge ? 205 : 240, edge ? 235 : 255);
            }
        }
    }
    writePng(path.c_str(), rgb, w, h);
}

// Same picture with numbered call-outs in the margins (used by the manual to explain a screen).
struct Callout { int tx, ty, n; bool right; };

static void savePictureAnnotated(const Canvas& c, const std::string& path, const std::vector<Callout>& calls) {
    const int S = 6, pad = 56;
    const int w = 128 * S + 2 * pad, h = 64 * S + 2 * pad;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &rgb[(static_cast<size_t>(y) * w + x) * 3]; p[0] = r; p[1] = g; p[2] = b;
    };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) put(x, y, 28, 31, 36);
    for (int y = 0; y < 64 * S; ++y) for (int x = 0; x < 128 * S; ++x) put(pad + x, pad + y, 4, 6, 10);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x) {
        if (!c.px[y][x]) continue;
        for (int dy = 0; dy < S - 1; ++dy) for (int dx = 0; dx < S - 1; ++dx) {
            const bool edge = (dx == 0 || dy == 0 || dx == S - 2 || dy == S - 2);
            put(pad + x * S + dx, pad + y * S + dy, edge ? 150 : 205, edge ? 205 : 240, edge ? 235 : 255);
        }
    }
    // 3x5 digits
    static const char* digits[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                                     "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"};
    for (const Callout& k : calls) {
        const int cx = k.right ? w - pad / 2 : pad / 2;
        const int cy = pad + k.ty * S + S / 2;
        const int tx = pad + k.tx * S + S / 2;
        // leader line
        const int steps = std::max(std::abs(tx - cx), std::abs(cy - cy)) + 1;
        for (int i = 0; i <= steps; ++i) {
            const int x = cx + (tx - cx) * i / steps;
            for (int t = -1; t <= 1; ++t) put(x, cy + t, 255, 150, 30);
        }
        for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) put(tx + dx, cy + dy, 255, 150, 30);   // target dot
        const int R = 16;
        for (int y = -R; y <= R; ++y) for (int x = -R; x <= R; ++x) if (x * x + y * y <= R * R) put(cx + x, cy + y, 255, 150, 30);
        const int dscale = 4;
        const char* d = digits[k.n % 10];
        for (int ry = 0; ry < 5; ++ry) for (int rx = 0; rx < 3; ++rx) if (d[ry * 3 + rx] == '1')
            for (int a = 0; a < dscale; ++a) for (int b2 = 0; b2 < dscale; ++b2) put(cx - 6 + rx * dscale + b2, cy - 10 + ry * dscale + a, 20, 20, 20);
    }
    writePng(path.c_str(), rgb, w, h);
}

// ------------------------------------------------------------------------------------------- scenes
using screens::drawMain;
using screens::drawMenu;
using Mode = MenuState::Mode;

static SlaveDevice dev(const char* name, bool online, bool phaseOn, bool out, float ton, float toff, float el, int8_t rT, int8_t rR, int loss, uint8_t mode = 0) {
    SlaveDevice d;
    strncpy(d.name, name, sizeof(d.name) - 1);
    d.online = online; d.phaseOn = phaseOn; d.outputState = out && online; d.ton = ton; d.toff = toff; d.elapsed = el;
    d.rssiSlave = online ? rT : 0; d.rssiRemote = online ? rR : 0; d.lossPct = online ? loss : -1; d.mode = mode; d.channel = 6;
    d.ageMs = online ? 300 : 38000;
    return d;
}

static UiModel baseModel() {
    UiModel m;
    m.now = 5000;
    m.batPercent = 78; m.batMv = 3980; m.liveMv = 3981;
    m.rssiLow = -100; m.rssiHigh = -50;
    m.homeChannel = 6;
    m.deviceCount = 6;
    m.activeIndex = 1;
    m.devices[0] = dev("Stage L", true, false, false, 1.5f, 12.0f, 7.4f, -52, -55, 1);
    m.devices[1] = dev("Fog2", true, true, true, 2.0f, 15.0f, 0.8f, -58, -61, 2);
    m.devices[2] = dev("Back haze", false, false, false, 1.0f, 20.0f, 3.0f, 0, 0, 0);
    m.devices[3] = dev("Stage R", true, false, false, 1.5f, 12.0f, 3.1f, -71, -68, 9);
    m.devices[4] = dev("Haze 5", true, true, true, 1.0f, 30.0f, 0.4f, -64, -60, 4, 1);
    m.devices[5] = dev("Truss", true, false, false, 1.0f, 8.0f, 2.2f, -81, -79, 22, 2);
    m.devices[3].rebooted = true;
    return m;
}

struct Scene { std::string name; std::function<void(Canvas&)> draw; };

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : "docs/images/oled";
    std::vector<Scene> scenes;
    auto add = [&](const char* name, std::function<void(Canvas&)> f) { scenes.push_back({name, f}); };

    add("splash", [](Canvas& c) { screens::drawSplash(c); });
    add("main_running", [](Canvas& c) { UiModel m = baseModel(); drawMain(c, m); });
    add("main_off_phase", [](Canvas& c) { UiModel m = baseModel(); m.activeIndex = 0; drawMain(c, m); });
    add("main_force_fog", [](Canvas& c) { UiModel m = baseModel(); m.activeIndex = 4; m.holdingFog = true; drawMain(c, m); });
    add("main_paused", [](Canvas& c) { UiModel m = baseModel(); m.activeIndex = 5; drawMain(c, m); });
    add("main_offline", [](Canvas& c) { UiModel m = baseModel(); m.activeIndex = 2; drawMain(c, m); });
    add("main_toast", [](Canvas& c) { UiModel m = baseModel(); m.toast = "Stage R power-cycled"; drawMain(c, m); });
    add("main_pending", [](Canvas& c) { UiModel m = baseModel(); m.devices[1].pending = 1; drawMain(c, m); });
    add("main_pc", [](Canvas& c) { UiModel m = baseModel(); m.pcConnected = true; drawMain(c, m); });
    add("main_no_timers", [](Canvas& c) { UiModel m = baseModel(); m.deviceCount = 0; drawMain(c, m); });
    add("main_menu_hold", [](Canvas& c) { UiModel m = baseModel(); m.hashHoldMs = 640; drawMain(c, m); });
    add("main_charging", [](Canvas& c) { UiModel m = baseModel(); m.charging = true; drawMain(c, m); });
    add("main_usb", [](Canvas& c) { UiModel m = baseModel(); m.usbPower = true; drawMain(c, m); });

    auto menuScene = [&](const char* name, std::function<void(UiModel&, MenuState&)> f) {
        add(name, [f](Canvas& c) { UiModel m = baseModel(); MenuState s; s.inMenu = true; f(m, s); drawMenu(c, m, s); });
    };
    menuScene("menu_root", [](UiModel&, MenuState& s) { s.mode = Mode::ROOT; s.selected = 0; });
    menuScene("menu_root_middle", [](UiModel&, MenuState& s) { s.mode = Mode::ROOT; s.selected = 6; s.scrollOffset = 3; s.animScroll = 3; });
    menuScene("menu_root_end", [](UiModel&, MenuState& s) { s.mode = Mode::ROOT; s.selected = 16; s.scrollOffset = 12; s.animScroll = 12; });
    menuScene("fleet_list", [](UiModel&, MenuState& s) { s.mode = Mode::SELECT_ACTIVE; s.activeSel = 4; });
    menuScene("fleet_list_top", [](UiModel&, MenuState& s) { s.mode = Mode::SELECT_ACTIVE; s.activeSel = 0; });
    menuScene("pair_searching", [](UiModel& m, MenuState& s) { s.mode = Mode::PAIRING; m.discoveredCount = 0; m.discovering = true; });
    menuScene("pair_list", [](UiModel& m, MenuState& s) {
        s.mode = Mode::PAIRING; s.pairSel = 1; m.discoveredCount = 3;
        const uint8_t macs[3][6] = {{0x24, 0x6F, 0x28, 0x11, 0x3A, 0x7C}, {0x24, 0x6F, 0x28, 0x11, 0x9B, 0x02}, {0x24, 0x6F, 0x28, 0x11, 0xC4, 0xE8}};
        const char* names[3] = {"Fog2", "FogTimer", "FogTimer"};
        const int8_t rssi[3] = {-52, -63, -77};
        for (int i = 0; i < 3; ++i) {
            DiscoveredDevice& d = m.discovered[i];
            memcpy(d.mac, macs[i], 6); strncpy(d.name, names[i], sizeof(d.name)); d.rssi = rssi[i]; d.channel = 6; d.paired = (i == 0);
            d.ton = 1.0f; d.toff = 10.0f; d.lastSeen = 4900;
        }
    });
    menuScene("link_status", [](UiModel&, MenuState& s) { s.mode = Mode::LINK_STATUS; s.linkFirst = 0; });
    menuScene("find_timers", [](UiModel& m, MenuState& s) { s.mode = Mode::FIND_TIMERS; m.searching = true; });
    menuScene("channel_scanning", [](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Scanning; m.surveyProgress = 42; });
    auto fillChannels = [](UiModel& m, MenuState& s) {
        const uint8_t scores[13] = {48, 52, 40, 26, 18, 30, 55, 70, 62, 20, 9, 21, 38};
        const uint8_t aps[13] = {5, 4, 3, 2, 1, 2, 4, 6, 5, 1, 0, 1, 2};
        for (int i = 0; i < 13; ++i) { m.channelRows[i].channel = i + 1; m.channelRows[i].score = scores[i]; m.channelRows[i].aps = aps[i]; }
        const uint8_t order[13] = {11, 5, 10, 12, 4, 6, 13, 3, 1, 2, 7, 9, 8};
        memcpy(s.chOrder, order, 13); s.chCount = 13;
    };
    menuScene("channel_choose", [=](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Choose; fillChannels(m, s); s.chSel = 0; });
    menuScene("channel_choose_current", [=](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Choose; fillChannels(m, s); s.chSel = 5; });
    menuScene("channel_moving", [](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Moving; s.chTarget = 11; m.migTotal = 5; m.migOk = 3; });
    menuScene("channel_done", [](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Result; s.chTarget = 11; m.migResult = 1; m.migOk = 5; m.migFail = 1; m.homeChannel = 11; });
    menuScene("channel_none", [](UiModel& m, MenuState& s) { s.mode = Mode::CHANNEL; s.chStage = MenuState::ChannelStage::Result; s.chTarget = 11; m.migResult = 0; });
    menuScene("link_test_running", [](UiModel& m, MenuState& s) { s.mode = Mode::LINK_TEST; m.linkTest = {true, true, 23, 23, 22, 40, 7, 19}; });
    menuScene("link_test_good", [](UiModel& m, MenuState& s) { s.mode = Mode::LINK_TEST; m.linkTest = {true, false, 40, 40, 39, 40, 6, 19}; });
    menuScene("link_test_poor", [](UiModel& m, MenuState& s) { s.mode = Mode::LINK_TEST; m.linkTest = {true, false, 40, 33, 27, 40, 38, 140}; });
    menuScene("edit_timers", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_TIMERS; s.editToffTenths = 100; s.editTonTenths = 20; s.editDigitIndex = 3; });
    menuScene("edit_timers_on", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_TIMERS; s.editToffTenths = 100; s.editTonTenths = 20; s.editDigitIndex = 8; });
    menuScene("rename", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_NAME; strcpy(s.nameBuf, "Fog2     "); s.namePos = 3; });
    menuScene("battery_cal_intro", [](UiModel&, MenuState& s) { s.mode = Mode::BATTERY_CALIB; s.calibStarted = false; });
    menuScene("battery_cal", [](UiModel&, MenuState& s) { s.mode = Mode::BATTERY_CALIB; s.calibStarted = true; s.calField = 1; s.editCal[0] = 3300; s.editCal[1] = 3750; s.editCal[2] = 4150; });
    menuScene("rssi_cal", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_RSSI_CALIB; s.rssiField = 1; s.editRssiLow = -100; s.editRssiHigh = -50; });
    menuScene("auto_off", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_AUTO_OFF; s.autoOffIdx = 3; });
    menuScene("tx_power", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_TXPOWER; s.editTxPowerQdbm = 80; });
    menuScene("brightness", [](UiModel&, MenuState& s) { s.mode = Mode::EDIT_BRIGHTNESS; s.editBrightness = 180; });
    menuScene("confirm_reset_timer", [](UiModel&, MenuState& s) { s.mode = Mode::CONFIRM; s.confirmAction = MenuState::ConfirmAction::RESET_TIMER; });
    menuScene("confirm_reset_remote", [](UiModel&, MenuState& s) { s.mode = Mode::CONFIRM; s.confirmAction = MenuState::ConfirmAction::RESET_REMOTE; });
    add("update_mode", [](Canvas& c) { screens::drawUpdateCountdown(c, 42); });
    add("oled_error", [](Canvas& c) { screens::drawErrorScreen(c); });

    // annotated pictures for the manual (positions are screen pixels of the thing being pointed at)
    {
        Canvas c;
        UiModel m = baseModel();
        drawMain(c, m);
        savePictureAnnotated(c, outDir + "/main_annotated.png",
                             {{8, 4, 1, false}, {10, 18, 2, false}, {8, 33, 3, false}, {6, 56, 4, false},
                              {92, 3, 5, true}, {92, 27, 6, true}, {92, 51, 7, true}});
        Canvas c2;
        UiModel m2 = baseModel();
        m2.activeIndex = 2;
        drawMain(c2, m2);
        savePictureAnnotated(c2, outDir + "/main_offline_annotated.png", {{80, 51, 1, true}, {122, 58, 2, true}});
    }

    int ok = 0;
    for (const Scene& s : scenes) {
        Canvas c;
        s.draw(c);
        savePicture(c, outDir + "/" + s.name + ".png", true);
        ++ok;
    }
    std::printf("%d pictures written to %s\n", ok, outDir.c_str());
    return 0;
}
