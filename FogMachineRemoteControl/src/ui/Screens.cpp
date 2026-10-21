#include "ui/Screens.h"

#include <stdio.h>
#include <string.h>
#include "Defaults.h"

namespace screens {

using Mode = MenuState::Mode;

namespace {

void header(Adafruit_GFX& g, const char* title, const char* right = nullptr) {
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
    g.setCursor(0, 0);
    g.print(title);
    if (right) {
        const int w = static_cast<int>(strlen(right)) * 6;
        g.setCursor(128 - w, 0);
        g.print(right);
    }
    g.drawLine(0, 9, 127, 9, kWhite);
}

void footer(Adafruit_GFX& g, const char* text) {
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
    g.setCursor(0, 56);
    g.print(text);
}

void text(Adafruit_GFX& g, int x, int y, const char* s, bool inverse = false, int size = 1) {
    g.setTextSize(size);
    if (inverse) {
        g.fillRect(x - 1, y - 1, static_cast<int>(strlen(s)) * 6 * size + 2, 8 * size + 2, kWhite);
        g.setTextColor(kBlack, kWhite);
    } else {
        g.setTextColor(kWhite, kBlack);
    }
    g.setCursor(x, y);
    g.print(s);
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
}

void listRow(Adafruit_GFX& g, int y, bool highlight) {
    if (highlight) {
        g.fillRect(0, y, 128, 10, kWhite);
        g.setTextColor(kBlack, kWhite);
    } else {
        g.setTextColor(kWhite, kBlack);
    }
}

void progressBar(Adafruit_GFX& g, int x, int y, int w, int h, int percent) {
    g.drawRect(x, y, w, h, kWhite);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int fill = (w - 2) * percent / 100;
    if (fill > 0) g.fillRect(x + 1, y + 1, fill, h - 2, kWhite);
}

const char* nameOf(const SlaveDevice& d) { return d.name[0] ? d.name : "(noname)"; }

// 0..6 bars from an RSSI value and the user's calibration range
int barsFor(int8_t rssi, int8_t low, int8_t high) {
    if (rssi >= 0 || rssi <= -120) return 0;
    if (high <= low) high = static_cast<int8_t>(low + 5);
    if (rssi <= low) return 0;
    if (rssi >= high) return 6;
    return static_cast<int>(6.0f * (rssi - low) / (high - low) + 0.5f);
}

void timerDigits(Adafruit_GFX& g, int tenths, int y, int startX, int highlightFirst = -1, int highlightIndex = -1) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d%01d", tenths / 10, tenths % 10);
    g.setTextSize(2);
    int x = startX;
    const int w = Defaults::UI_DIGIT_WIDTH;
    for (int i = 0; i < 5; ++i) {
        const bool inv = highlightFirst >= 0 && (highlightFirst + i) == highlightIndex;
        if (inv) { g.fillRect(x, y, w, 16, kWhite); g.setTextColor(kBlack, kWhite); }
        else { g.fillRect(x, y, w, 16, kBlack); g.setTextColor(kWhite, kBlack); }
        g.setCursor(x, y);
        g.print(buf[i]);
        if (i == 3) {                                // decimal point lives in its own (never highlighted) cell
            g.fillRect(x + w, y, w, 16, kBlack);
            g.setTextColor(kWhite, kBlack);
            g.setCursor(x + w, y);
            g.print('.');
            x += w;
        }
        x += w;
    }
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
}

void rowLabel(Adafruit_GFX& g, int y, const char* label, int startX = Defaults::UI_TIMER_START_X) {
    const int labelX = startX + Defaults::UI_DIGIT_WIDTH * 6 + Defaults::UI_LABEL_GAP_X;
    text(g, labelX, y + 7, label);
}

}  // namespace

// ---------------------------------------------------------------------------------------- parts
void drawSignalBars(Adafruit_GFX& g, int8_t rssi, int x, int y, int8_t low, int8_t high) {
    const int level = barsFor(rssi, low, high);
    const int barW = 3, gap = 1, areaH = 12;
    for (int i = 0; i < 6; ++i) {
        const int h = 2 + i * 2;
        const int bx = x + i * (barW + gap);
        const int baseY = y + areaH - 1;
        const int by = baseY - (h - 1);
        g.fillRect(bx, y, barW, areaH, kBlack);
        g.fillRect(bx, baseY, barW, 1, kWhite);
        if (i < level) { if (h > 1) g.fillRect(bx, by, barW, h - 1, kWhite); }
        else g.drawRect(bx, by, barW, h - 1, kWhite);
    }
}

void drawBattery(Adafruit_GFX& g, uint8_t percent, bool charging, bool usbPower, uint32_t now) {
    const int x = Defaults::UI_BATT_X, y = Defaults::UI_BATT_Y, w = Defaults::UI_BATT_W, h = Defaults::UI_BATT_H;
    const int termW = Defaults::UI_BATT_TERM_W, termH = Defaults::UI_BATT_TERM_H;
    g.fillRect(x, y, w + termW + 1, h, kBlack);
    const int innerW = w - 2, innerH = h - 2;
    if (percent > 100) percent = 100;
    if (usbPower && !charging) {
        // plug glyph instead of the battery: powered from USB, nothing to show
        const int px = x + 1, py = y + 1;
        g.drawRect(px + 2, py + 1, 9, h - 2, kWhite);
        g.drawLine(px, py + 3, px + 2, py + 3, kWhite);
        g.drawLine(px + 11, py + 3, px + 14, py + 3, kWhite);
        g.drawLine(px + 4, py, px + 4, py + 1, kWhite);
        g.drawLine(px + 8, py, px + 8, py + 1, kWhite);
        return;
    }
    g.drawRect(x, y, w, h, kWhite);
    g.fillRect(x + w, y + (h - termH) / 2, termW, termH, kWhite);
    if (charging) {
        if (((now / 350) % 2) == 0) g.fillRect(x + 1, y + 1, innerW, innerH, kWhite);
    } else {
        const int fill = innerW * percent / 100;
        if (fill > 0) g.fillRect(x + 1, y + 1, fill, innerH, kWhite);
    }
}

// ---------------------------------------------------------------------------------------- simple screens
void drawSplash(Adafruit_GFX& g) {
    header(g, "FogMachine Remote");
    text(g, 0, 20, Defaults::VERSION());
    text(g, 0, 32, "up to 10 timers");
}

void drawBootStatus(Adafruit_GFX& g, const char* msg) {
    const int y = 54;
    g.fillRect(0, y - 1, 128, 11, kBlack);
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
    char buf[24];
    strncpy(buf, msg ? msg : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    buf[21] = 0;
    g.setCursor(0, y);
    g.print(buf);
}

void drawUpdateCountdown(Adafruit_GFX& g, uint8_t secondsRemaining) {
    header(g, "Firmware Update Mode");
    text(g, 0, 16, "Connect USB and flash");
    char buf[8];
    snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(secondsRemaining));
    const int size = 3;
    const int w = static_cast<int>(strlen(buf)) * 6 * size;
    text(g, (128 - w) / 2, 34, buf, false, size);
}

void drawErrorScreen(Adafruit_GFX& g) {
    text(g, 0, 0, "OLED/I2C not found");
    text(g, 0, 12, "Check wiring:");
    text(g, 0, 22, "SDA=D4 (GPIO6)");
    text(g, 0, 32, "SCL=D5 (GPIO7)");
    text(g, 0, 44, "Addr 0x3C");
}

// ---------------------------------------------------------------------------------------- main screen
void drawMain(Adafruit_GFX& g, const UiModel& m) {
    drawBattery(g, m.batPercent, m.charging, m.usbPower, m.now);

    if (m.deviceCount == 0) {
        text(g, 0, 14, "No paired", false, 2);
        text(g, 0, 30, "timers.", false, 2);
        text(g, 0, 56, "Hold # for menu");
        return;
    }
    const SlaveDevice* a = m.active();
    if (!a) { text(g, 0, 14, "No active", false, 2); return; }

    drawSignalBars(g, a->online ? a->rssiSlave : 0, 0, Defaults::UI_BATT_Y + Defaults::UI_BATT_H + 4, m.rssiLow, m.rssiHigh);

    char idx[24];
    snprintf(idx, sizeof(idx), "%d/%d", m.activeIndex + 1, m.deviceCount);
    text(g, 0, 30, idx);
    if (a->pending && ((m.now / 200) % 2) == 0) g.fillRect(20, 40, 4, 4, kWhite);      // blinking dot: command in flight
    if (m.pcConnected) text(g, 116, 0, "PC");

    timerDigits(g, static_cast<int>(a->toff * 10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_OFF, Defaults::UI_TIMER_START_X);
    rowLabel(g, Defaults::UI_TIMER_ROW_Y_OFF, "OFF");
    timerDigits(g, static_cast<int>(a->ton * 10.0f + 0.5f), Defaults::UI_TIMER_ROW_Y_ON, Defaults::UI_TIMER_START_X);
    rowLabel(g, Defaults::UI_TIMER_ROW_Y_ON, "ON");

    const int y = Defaults::UI_TIMER_ROW_Y_TIME;
    if (!a->online) {
        // not heard from for a while: machine unpowered (heating?) or out of range
        g.fillRect(Defaults::UI_TIMER_START_X - 1, y - 1, 128 - Defaults::UI_TIMER_START_X + 1, 18, kWhite);
        g.setTextColor(kBlack, kWhite);
        g.setTextSize(2);
        g.setCursor(Defaults::UI_TIMER_START_X + 2, y + 1);
        g.print(a->ageMs == 0xFFFFFFFFu ? "NO LINK" : "OFFLINE");
        g.setTextSize(1);
        g.setTextColor(kWhite, kBlack);
        if (a->ageMs != 0xFFFFFFFFu) {
            char age[8];
            const unsigned long secs = a->ageMs / 1000;
            if (secs > 99) snprintf(age, sizeof(age), "99+"); else snprintf(age, sizeof(age), "%lus", secs);
            g.setTextColor(kBlack, kWhite);
            g.setCursor(110, y + 4);
            g.print(age);
            g.setTextColor(kWhite, kBlack);
        }
    } else {
        float shown = a->elapsed;
        float cap = a->phaseOn ? a->ton : a->toff;
        if (a->mode == 0 && shown > cap) shown = cap;
        timerDigits(g, static_cast<int>(shown * 10.0f + 0.5f), y, Defaults::UI_TIMER_START_X);
        const char* label = a->mode == 1 ? "FOG" : (a->mode == 2 ? "STOP" : "TIME");   // 4 characters is all the label column holds
        rowLabel(g, y, label);
        if (a->outputState) text(g, 0, Defaults::UI_STATE_CHAR_Y, "*", false, 2);
    }

    if (m.hashHoldMs >= Defaults::MENU_PROGRESS_START_MS) {
        const int full = static_cast<int>(Defaults::BUTTON_LONG_PRESS_MS - Defaults::MENU_PROGRESS_START_MS);
        int pct = static_cast<int>((m.hashHoldMs - Defaults::MENU_PROGRESS_START_MS) * 100 / full);
        if (pct > 100) pct = 100;
        g.fillRect(0, 48, 128, 16, kBlack);
        progressBar(g, 0, 48, 128, 16, pct);
        g.setTextColor(kBlack, kWhite);
        g.setCursor(52, 52);
        if (pct >= 99) g.print("MENU");
        else { char b[8]; snprintf(b, sizeof(b), "%3d%%", pct); g.print(b); }
        g.setTextColor(kWhite, kBlack);
    }

    if (m.toast) {
        g.fillRect(0, 55, 128, 9, kWhite);
        g.setTextColor(kBlack, kWhite);
        g.setCursor(2, 56);
        g.print(m.toast);
        g.setTextColor(kWhite, kBlack);
    }
}

// ---------------------------------------------------------------------------------------- menu screens
namespace {

void drawRootList(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    const int start = s.visibleStart();
    const int lines = s.visibleCount();
    const int rowH = 10, yBase = 10;
    const bool scrollAnim = s.scrollAnimating(m.now);
    const bool selAnim = s.selectionAnimating(m.now) && !scrollAnim;
    float progress = 1.0f;
    if (scrollAnim) {
        uint32_t dt = m.now - s.scrollAnimStart;
        if (dt > MenuState::SCROLL_ANIM_MS) dt = MenuState::SCROLL_ANIM_MS;
        progress = static_cast<float>(dt) / MenuState::SCROLL_ANIM_MS;
    }
    auto list = [&](int base, int shift) {
        for (int i = 0; i < lines; ++i) {
            const int idx = base + i;
            if (idx >= MenuState::kItemCount) break;
            text(g, 2, yBase + i * rowH + shift, MenuState::kItemLabels[idx]);
        }
    };
    if (scrollAnim) {
        list(s.prevScroll, static_cast<int>(-s.scrollDir * rowH * progress + 0.5f));
        list(start, static_cast<int>(s.scrollDir * rowH * (1.0f - progress) + 0.5f));
    } else {
        list(start, 0);
    }
    const int rel = s.selected - start;
    if (rel >= 0 && rel < lines) {
        int y = yBase + rel * rowH;
        if (selAnim) {
            uint32_t dt = m.now - s.lastSelChange;
            if (dt > MenuState::SELECTION_ANIM_MS) dt = MenuState::SELECTION_ANIM_MS;
            const float p = static_cast<float>(dt) / MenuState::SELECTION_ANIM_MS;
            const int prevRel = s.prevSelected - start;
            if (prevRel >= 0 && prevRel < lines) {
                const int yPrev = yBase + prevRel * rowH;
                y = static_cast<int>(yPrev + (y - yPrev) * p + 0.5f);
            }
        }
        g.fillRect(0, y - 1, 128, rowH, kWhite);
        g.setTextColor(kBlack, kWhite);
        g.setCursor(2, y);
        g.print(MenuState::kItemLabels[s.selected]);
        g.setTextColor(kWhite, kBlack);
    }
    if (start > 0) g.drawTriangle(120, 8, 125, 8, 122, 3, kWhite);
    if (start + lines < MenuState::kItemCount) g.drawTriangle(120, 57, 125, 57, 122, 62, kWhite);
}

void drawFleetList(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    header(g, "Active Timer", nullptr);
    if (m.deviceCount == 0) { text(g, 0, 16, "No timers paired"); text(g, 0, 28, "Menu > Pair Timer"); footer(g, "*=Back"); return; }
    int sel = s.activeSel;
    if (sel >= m.deviceCount) sel = m.deviceCount - 1;
    int first = sel >= 4 ? sel - 3 : 0;
    for (int i = 0; i < 4 && first + i < m.deviceCount; ++i) {
        const int idx = first + i;
        const SlaveDevice& d = m.devices[idx];
        const int y = 12 + i * 11;
        listRow(g, y, idx == sel);
        const bool hi = idx == sel;
        const uint16_t fg = hi ? kBlack : kWhite;
        g.setCursor(2, y + 1);
        g.print(idx == m.activeIndex ? '>' : ' ');
        g.print(nameOf(d));
        // right side: state word + signal
        const char* st = !d.online ? "--" : (d.mode == 1 ? "FORCE" : (d.mode == 2 ? "PAUSE" : (d.outputState ? "ON" : "off")));
        g.setCursor(70, y + 1);
        g.print(st);
        const int bars = d.online ? (barsFor(d.rssiSlave, m.rssiLow, m.rssiHigh) + 1) / 2 : 0;   // 0..3
        for (int b = 0; b < 3; ++b) {
            const int bx = 108 + b * 6, bh = 3 + b * 2;
            if (b < bars) g.fillRect(bx, y + 8 - bh, 4, bh, fg);
            else g.drawRect(bx, y + 8 - bh, 4, bh, fg);
        }
        if (d.rebooted) g.fillRect(101, y + 3, 3, 3, fg);        // power cycle noticed recently
    }
    g.setTextColor(kWhite, kBlack);
    footer(g, "#=Select *=Back");
}

void drawPairing(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    char right[8];
    snprintf(right, sizeof(right), "ch%u", m.homeChannel);
    header(g, "Pair Timer", right);
    if (m.discoveredCount == 0) {
        text(g, 0, 16, "Searching...");
        text(g, 0, 28, "Power on the timer");
        text(g, 0, 38, "(all channels)");
        footer(g, "*=Back");
        return;
    }
    int sel = s.pairSel;
    if (sel >= m.discoveredCount) sel = m.discoveredCount - 1;
    const int first = sel >= 4 ? sel - 3 : 0;
    for (int i = 0; i < 4 && first + i < m.discoveredCount; ++i) {
        const DiscoveredDevice& d = m.discovered[first + i];
        const int y = 12 + i * 11;
        listRow(g, y, first + i == sel);
        char line[28];
        snprintf(line, sizeof(line), "%c%-9.9s", d.paired ? '*' : ' ', d.name[0] ? d.name : "(new)");
        g.setCursor(2, y + 1);
        g.print(line);
        char tail[16];
        snprintf(tail, sizeof(tail), "%02X%02X", d.mac[4], d.mac[5]);
        g.setCursor(70, y + 1);
        g.print(tail);
        snprintf(tail, sizeof(tail), "%d", d.rssi);
        g.setCursor(128 - static_cast<int>(strlen(tail)) * 6 - 1, y + 1);
        g.print(tail);
    }
    g.setTextColor(kWhite, kBlack);
    const DiscoveredDevice& cur = m.discovered[sel];
    footer(g, cur.paired ? "#=Unpair  *=Back" : "#=Pair    *=Back");
}

void drawLinkStatus(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    header(g, "Links");
    text(g, 74, 1, "R");
    text(g, 98, 1, "T");
    text(g, 116, 1, "L%");
    if (m.deviceCount == 0) { text(g, 0, 16, "No timers paired"); footer(g, "*=Back"); return; }
    int first = s.linkFirst;
    if (first > m.deviceCount - 1) first = m.deviceCount - 1;
    if (first < 0) first = 0;
    for (int i = 0; i < 4 && first + i < m.deviceCount; ++i) {
        const SlaveDevice& d = m.devices[first + i];
        const int y = 12 + i * 11;
        g.setTextColor(kWhite, kBlack);
        g.setCursor(0, y);
        g.print(first + i == m.activeIndex ? '>' : ' ');
        char nm[10];
        snprintf(nm, sizeof(nm), "%-8.8s", nameOf(d));
        g.print(nm);
        if (!d.online) {
            g.setCursor(66, y);
            g.print("offline");
        } else {
            char b[8];
            snprintf(b, sizeof(b), "%d", d.rssiRemote);
            g.setCursor(80 - static_cast<int>(strlen(b)) * 6, y);
            g.print(b);
            snprintf(b, sizeof(b), "%d", d.rssiSlave);
            g.setCursor(104 - static_cast<int>(strlen(b)) * 6, y);
            g.print(b);
            if (d.lossPct >= 0) snprintf(b, sizeof(b), "%d%%", d.lossPct); else snprintf(b, sizeof(b), "--");
            g.setCursor(128 - static_cast<int>(strlen(b)) * 6, y);
            g.print(b);
        }
    }
    footer(g, "Up/Dn scroll  *=Back");
}

void drawFind(Adafruit_GFX& g, const UiModel& m) {
    int lost = 0;
    for (int i = 0; i < m.deviceCount; ++i) if (!m.devices[i].online) ++lost;
    char right[12];
    snprintf(right, sizeof(right), "%d lost", lost);
    header(g, "Find Timers", right);
    if (m.deviceCount == 0) { text(g, 0, 16, "No timers paired"); footer(g, "*=Back"); return; }
    for (int i = 0; i < 4 && i < m.deviceCount; ++i) {
        const SlaveDevice& d = m.devices[i];
        const int y = 12 + i * 11;
        char line[28];
        if (d.online) snprintf(line, sizeof(line), "%-9.9s  OK  ch%u", nameOf(d), d.channel);
        else if (d.ageMs == 0xFFFFFFFFu) snprintf(line, sizeof(line), "%-9.9s  not seen", nameOf(d));
        else snprintf(line, sizeof(line), "%-9.9s  lost %lus", nameOf(d), static_cast<unsigned long>(d.ageMs / 1000));
        text(g, 0, y, line);
    }
    footer(g, m.searching ? "Searching..." : "#=Search  *=Back");
}

void drawChannel(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    char right[10];
    snprintf(right, sizeof(right), "now ch%u", m.homeChannel);
    header(g, "Channel", right);
    switch (s.chStage) {
        case MenuState::ChannelStage::Scanning:
            text(g, 0, 16, "Scanning the band...");
            progressBar(g, 0, 30, 128, 10, m.surveyProgress);
            text(g, 0, 44, "Radio paused ~3 s");
            footer(g, "*=Cancel");
            break;
        case MenuState::ChannelStage::Choose: {
            const int first = s.chSel >= 3 ? s.chSel - 2 : 0;
            for (int i = 0; i < 4 && first + i < s.chCount; ++i) {
                const int idx = first + i;
                const ChannelRow* row = nullptr;
                for (const ChannelRow& r : m.channelRows) if (r.channel == s.chOrder[idx]) row = &r;
                const int y = 12 + i * 11;
                listRow(g, y, idx == s.chSel);
                const bool hi = idx == s.chSel;
                char line[16];
                snprintf(line, sizeof(line), "%cCh%02u", s.chOrder[idx] == m.homeChannel ? '*' : ' ', s.chOrder[idx]);
                g.setCursor(2, y + 1);
                g.print(line);
                const int score = row ? row->score : 0;
                g.drawRect(46, y + 1, 50, 7, hi ? kBlack : kWhite);
                const int fill = 48 * score / 100;
                if (fill > 0) g.fillRect(47, y + 2, fill, 5, hi ? kBlack : kWhite);
                g.setCursor(100, y + 1);
                if (idx == 0) g.print("best");
                else { char b[6]; snprintf(b, sizeof(b), "%d", score); g.print(b); }
            }
            g.setTextColor(kWhite, kBlack);
            footer(g, "#=Use #L=Scan *=Back");
            break;
        }
        case MenuState::ChannelStage::Moving: {
            char l[40];
            snprintf(l, sizeof(l), "Moving to ch%u...", s.chTarget);
            text(g, 0, 16, l);
            const int done = m.migOk + m.migFail;
            progressBar(g, 0, 30, 128, 10, m.migTotal ? done * 100 / m.migTotal : 50);
            snprintf(l, sizeof(l), "%d of %d answered", m.migOk, m.migTotal);
            text(g, 0, 44, l);
            break;
        }
        case MenuState::ChannelStage::Result: {
            char l[40];
            if (m.migResult == 1) {
                snprintf(l, sizeof(l), "Channel %u active", s.chTarget);
                text(g, 0, 16, l);
                snprintf(l, sizeof(l), "%d timer(s) moved", m.migOk);
                text(g, 0, 28, l);
                if (m.migFail) { snprintf(l, sizeof(l), "%d offline - follows", m.migFail); text(g, 0, 40, l); }
                footer(g, "#=OK");
            } else {
                text(g, 0, 16, "No timer answered.");
                snprintf(l, sizeof(l), "Staying on ch%u", m.homeChannel);
                text(g, 0, 28, l);
                text(g, 0, 40, "Power them on first");
                footer(g, "#=Force   *=Back");
            }
            break;
        }
        case MenuState::ChannelStage::Failed:
            text(g, 0, 16, "Scan failed");
            footer(g, "#=Retry   *=Back");
            break;
    }
}

void drawLinkTest(Adafruit_GFX& g, const UiModel& m) {
    header(g, "Link Test");
    const SlaveDevice* a = m.active();
    char l[28];
    snprintf(l, sizeof(l), "Timer: %.9s", a ? nameOf(*a) : "-");
    text(g, 0, 13, l);
    const LinkTestView& t = m.linkTest;
    snprintf(l, sizeof(l), "Sent %u/%u", t.sent, t.total);
    text(g, 0, 23, l);
    progressBar(g, 78, 23, 49, 8, t.total ? t.sent * 100 / t.total : 0);
    snprintf(l, sizeof(l), "MAC ack %u", t.macAck);
    text(g, 0, 33, l);
    snprintf(l, sizeof(l), "Reply   %u", t.echo);
    text(g, 0, 43, l);
    if (t.sent > 4) {
        const int pct = t.sent ? t.echo * 100 / t.sent : 0;
        const char* verdict = pct >= 95 ? "GOOD" : (pct >= 80 ? "FAIR" : "POOR");
        text(g, 80, 34, verdict, !t.running, 2);
        snprintf(l, sizeof(l), "%lums", static_cast<unsigned long>(t.rttAvgMs));
        text(g, 128 - static_cast<int>(strlen(l)) * 6, 56, l);
    }
    footer(g, t.running ? "*=Back" : "#=Again *=Back");
}

void drawEditTimers(Adafruit_GFX& g, const MenuState& s) {
    timerDigits(g, s.editToffTenths, Defaults::UI_TIMER_ROW_Y_OFF, Defaults::UI_TIMER_START_X, 0, s.editDigitIndex);
    rowLabel(g, Defaults::UI_TIMER_ROW_Y_OFF, "Toff");
    timerDigits(g, s.editTonTenths, Defaults::UI_TIMER_ROW_Y_ON, Defaults::UI_TIMER_START_X, 5, s.editDigitIndex);
    rowLabel(g, Defaults::UI_TIMER_ROW_Y_ON, "Ton");
    footer(g, "#=Next #L=Prev *=Quit");
}

void drawEditName(Adafruit_GFX& g, const MenuState& s) {
    header(g, "Rename Timer");
    g.setTextSize(2);
    const int cw = 12, ch = 16, y = 18;
    for (int i = 0; s.nameBuf[i] && i < 9; ++i) {
        const int x = i * cw;
        const bool inv = i == s.namePos;
        g.fillRect(x, y, cw, ch, inv ? kWhite : kBlack);
        g.setTextColor(inv ? kBlack : kWhite, inv ? kWhite : kBlack);
        g.setCursor(x, y);
        g.print(s.nameBuf[i]);
    }
    g.setTextSize(1);
    g.setTextColor(kWhite, kBlack);
    text(g, 0, 40, "Up/Dn letter  #=Next");
    footer(g, "#L=Prev  *=Cancel");
}

void drawSettingScreens(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    switch (s.mode) {
        case Mode::EDIT_AUTO_OFF: {
            header(g, "Auto Off");
            const int v = s.autoOffSeconds();
            char b[24];
            if (v == 0) snprintf(b, sizeof(b), "Never"); else snprintf(b, sizeof(b), "%d s", v);
            text(g, 0, 18, b, false, 2);
            text(g, 0, 40, "Display off, then sleep");
            footer(g, "#=Save  *=Back");
            break;
        }
        case Mode::EDIT_TXPOWER: {
            header(g, "Radio TX Power");
            char b[24];
            snprintf(b, sizeof(b), "%.2f dBm", s.editTxPowerQdbm / 4.0f);
            text(g, 0, 18, b, false, 2);
            text(g, 0, 40, "Lower = less range");
            footer(g, "#=Save  *=Back");
            break;
        }
        case Mode::EDIT_BRIGHTNESS: {
            header(g, "OLED Brightness");
            char b[16];
            snprintf(b, sizeof(b), "%d", s.editBrightness);
            text(g, 0, 18, b, false, 2);
            progressBar(g, 0, 40, 128, 8, s.editBrightness * 100 / 255);
            footer(g, "#=Save  *=Back");
            break;
        }
        default: break;
    }
}

void drawBatteryCal(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    header(g, "Battery Calibration");
    char b[28];
    snprintf(b, sizeof(b), "Now: %u mV  (%u%%)", m.liveMv, m.batPercent);
    text(g, 0, 13, b);
    if (!s.calibStarted) {
        text(g, 0, 28, "Set what 0/50/100%");
        text(g, 0, 38, "mean for your cell");
        footer(g, "#=Start  *=Cancel");
        return;
    }
    static const char* names[3] = {"  0%", " 50%", "100%"};
    for (int i = 0; i < 3; ++i) {
        const int y = 26 + i * 10;
        snprintf(b, sizeof(b), "%s %4u mV", names[i], s.editCal[i]);
        text(g, 0, y, b, i == s.calField);
    }
    footer(g, "#=Next #L=Take now");
}

void drawRssiCal(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    header(g, "Signal Bars");
    char b[28];
    snprintf(b, sizeof(b), "0 bars at %d dBm", s.editRssiLow);
    text(g, 0, 14, b, s.rssiField == 0);
    snprintf(b, sizeof(b), "6 bars at %d dBm", s.editRssiHigh);
    text(g, 0, 26, b, s.rssiField == 1);
    const SlaveDevice* a = m.active();
    if (a && a->online) snprintf(b, sizeof(b), "Live: %d dBm", a->rssiSlave); else snprintf(b, sizeof(b), "Live: --");
    text(g, 0, 40, b);
    if (a && a->online) drawSignalBars(g, a->rssiSlave, 100, 38, s.editRssiLow, s.editRssiHigh);
    footer(g, "#=Next/Save *=Cancel");
}

void drawConfirm(Adafruit_GFX& g, const MenuState& s) {
    header(g, "Confirm");
    const char* l1 = "";
    const char* l2 = "";
    switch (s.confirmAction) {
        case MenuState::ConfirmAction::RESET_TIMER: l1 = "Factory reset the"; l2 = "selected timer?"; break;
        case MenuState::ConfirmAction::RESET_REMOTE: l1 = "Forget all timers &"; l2 = "reset settings?"; break;
        case MenuState::ConfirmAction::REBOOT: l1 = "Restart the remote?"; break;
        default: break;
    }
    text(g, 0, 20, l1);
    text(g, 0, 32, l2);
    footer(g, "#=Yes   *=No");
}

}  // namespace

void drawMenu(Adafruit_GFX& g, const UiModel& m, const MenuState& s) {
    switch (s.mode) {
        case Mode::ROOT: drawRootList(g, m, s); break;
        case Mode::EDIT_AUTO_OFF:
        case Mode::EDIT_TXPOWER:
        case Mode::EDIT_BRIGHTNESS: drawSettingScreens(g, m, s); break;
        case Mode::PAIRING: drawPairing(g, m, s); break;
        case Mode::SELECT_ACTIVE: drawFleetList(g, m, s); break;
        case Mode::LINK_STATUS: drawLinkStatus(g, m, s); break;
        case Mode::BATTERY_CALIB: drawBatteryCal(g, m, s); break;
        case Mode::EDIT_RSSI_CALIB: drawRssiCal(g, m, s); break;
        case Mode::EDIT_TIMERS: drawEditTimers(g, s); break;
        case Mode::EDIT_NAME: drawEditName(g, s); break;
        case Mode::CONFIRM: drawConfirm(g, s); break;
        case Mode::CHANNEL: drawChannel(g, m, s); break;
        case Mode::FIND_TIMERS: drawFind(g, m); break;
        case Mode::LINK_TEST: drawLinkTest(g, m); break;
    }
}

}  // namespace screens
