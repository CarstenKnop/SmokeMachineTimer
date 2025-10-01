#include "MenuSystem.h"
#include "MenuItems/Help.h"

void MenuSystem::begin() {
    state = State::INACTIVE;
    menuIndex = 0;
    menuScrollPos = 0.0f;
}

void MenuSystem::startProgress(unsigned long now) {
    if (state == State::INACTIVE) {
        state = State::PROGRESS;
        hashHoldStart = now;
    }
}
void MenuSystem::startProgressDeferred(unsigned long now) {
    if (state == State::INACTIVE) {
        state = State::PROGRESS;
        hashHoldStart = now - Defaults::MENU_PROGRESS_START_MS;
    }
}
void MenuSystem::updateProgress(bool hashHeld, bool hashReleased, unsigned long now) {
    if (state != State::PROGRESS) return;
    if (hashHeld) { return; }
    unsigned long held = now - hashHoldStart;
    if (held >= Defaults::MENU_PROGRESS_FULL_MS) enterSelect(now); else cancel();
}
void MenuSystem::cancel() { state = State::INACTIVE; menuHint=false; }

void MenuSystem::navigate(const ButtonState& bs, unsigned long now) {
    if (state != State::SELECT) return;
    int count = getMenuCount();
    if (bs.upEdge) { menuIndex = (menuIndex - 1 + count) % count; }
    if (bs.downEdge) { menuIndex = (menuIndex + 1) % count; }
    animateScroll(now);
}

bool MenuSystem::handleSelect(const ButtonState& bs, unsigned long now, Config& config) {
    if (state != State::SELECT) return false;
    if (bs.starEdge) { state = State::INACTIVE; return true; }
    if (bs.hashEdge) {
        selectedMenu = menuIndex;
        switch(selectedMenu) {
            case 0: beginSaverEdit(config.get().screensaverDelaySec); break;
            case 1: state = State::WIFI_INFO; break;
            case 2: state = State::QR_DYN; break;
            case 3: state = State::RICK; break;
            case 4: enterHelp(); break;
            default: state = State::RESULT; menuResultStart = now; break;
        }
        return true;
    }
    return false;
}

void MenuSystem::updateResult(unsigned long now) {
    if (state == State::RESULT && (now - menuResultStart) >= Defaults::MENU_RESULT_TIMEOUT_MS) {
        state = State::INACTIVE;
    }
}

// old handleSaverEdit removed (delegated)

void MenuSystem::animateScroll(unsigned long now) {
    unsigned long dtMs = now - lastScrollUpdate;
    if (dtMs == 0) return;
    lastScrollUpdate = now;
    float dt = dtMs / 1000.0f;
    float target = (float)menuIndex;
    float diff = target - menuScrollPos;
    // no wrap-around with dynamic small count; simple easing
    float step = Defaults::MENU_SCROLL_SPEED * dt;
    if (fabs(diff) <= step) menuScrollPos = target; else {
    menuScrollPos += (diff > 0 ? step : -step);
    if (menuScrollPos < 0) menuScrollPos = 0; int maxIndex = getMenuCount()-1; if (menuScrollPos > maxIndex) menuScrollPos = (float)maxIndex;
    }
}

float MenuSystem::progressFraction(unsigned long now) const {
    if (state != State::PROGRESS) return 0.0f;
    unsigned long held = now - hashHoldStart;
    if (held <= Defaults::MENU_PROGRESS_START_MS) return 0.0f;
    unsigned long span = held - Defaults::MENU_PROGRESS_START_MS;
    unsigned long total = Defaults::MENU_PROGRESS_FULL_MS - Defaults::MENU_PROGRESS_START_MS;
    if (span > total) span = total;
    return (float)span / (float)total;
}

bool MenuSystem::progressFull(unsigned long now) const {
    return state == State::PROGRESS && (now - hashHoldStart) >= Defaults::MENU_PROGRESS_FULL_MS;
}

void MenuSystem::enterSelect(unsigned long now) {
    state = State::SELECT;
    menuScrollPos = (float)menuIndex;
    lastScrollUpdate = now;
    menuHint=false;
}

void MenuSystem::beginSaverEdit(uint16_t current) {
    saverEdit.begin(current);
    state = State::SAVER_EDIT;
}

// repeat logic now resides in SaverMenu::EditController

void MenuSystem::enterHelp() {
    state = State::HELP; menuHint=false; helpCtrl.enter();
}

void MenuSystem::handleHelp(const ButtonState& bs) {
    if (state != State::HELP) return; if (helpCtrl.handleInput(bs)) { state = State::SELECT; }
}

void MenuSystem::updateHelpAnimation(unsigned long now) { if (state==State::HELP) helpCtrl.update(now); }

int MenuSystem::getHelpLines() const { return HelpContent::LINES_COUNT; }
const char* MenuSystem::getHelpLine(int i) const { return HelpContent::line(i); }

void MenuSystem::processInput(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver) {
        switch(state) {
            case State::SELECT:
                // navigation handled separately via navigate(); selection via hashEdge
                if (bs.hashEdge || bs.starEdge) {
                    // delegate to handleSelect for consistency
                    handleSelect(bs, now, config);
                }
                break;
            case State::SAVER_EDIT:
                if (saverEdit.handle(bs, now, config, saver)) { state = State::SELECT; }
                break;
            case State::HELP:
                handleHelp(bs);
                break;
            case State::WIFI_INFO:
            case State::QR_DYN:
            case State::RICK:
                if (bs.hashEdge || bs.starEdge) { state = State::SELECT; }
                break;
            case State::RESULT:
                if (bs.hashEdge || bs.starEdge) { state = State::INACTIVE; }
                break;
            case State::PROGRESS:
            case State::INACTIVE:
                // handled externally (progress logic)
                break;
        }
}
