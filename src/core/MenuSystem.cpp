#include "MenuSystem.h"

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
    if (bs.upEdge) { menuIndex = (menuIndex - 1 + MENU_COUNT) % MENU_COUNT; }
    if (bs.downEdge) { menuIndex = (menuIndex + 1) % MENU_COUNT; }
    animateScroll(now);
}

bool MenuSystem::handleSelect(const ButtonState& bs, unsigned long now, Config& config) {
    if (state != State::SELECT) return false;
    if (bs.starEdge) { state = State::INACTIVE; return true; }
    if (bs.hashEdge) {
        selectedMenu = menuIndex;
        if (selectedMenu == 0) { beginSaverEdit(config.get().screensaverDelaySec); }
        else if (selectedMenu == 9) { enterHelp(); }
        else { state = State::RESULT; menuResultStart = now; }
        return true;
    }
    return false;
}

void MenuSystem::updateResult(unsigned long now) {
    if (state == State::RESULT && (now - menuResultStart) >= Defaults::MENU_RESULT_TIMEOUT_MS) {
        state = State::INACTIVE;
    }
}

bool MenuSystem::handleSaverEdit(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver) {
    if (state != State::SAVER_EDIT) return false;
    ButtonState local = bs;
    if (ignoreFirstHashEdgeSaver && bs.hashEdge) { local.hashEdge=false; ignoreFirstHashEdgeSaver=false; }
    repeatHandler(local, now);
    bool changed = false;
    if (actUp) {
        if (editingSaverValue == 0) editingSaverValue = 10;
        else if (editingSaverValue == 990) editingSaverValue = 0;
        else editingSaverValue += 10;
        changed = true;
    }
    if (actDown) {
        if (editingSaverValue == 0) editingSaverValue = 990;
        else if (editingSaverValue == 10) editingSaverValue = 0;
        else editingSaverValue -= 10;
        changed = true;
    }
    if (changed) saver.noteActivity(now);
    if (local.starEdge) {
        state = State::SELECT; resetRepeat(); return true; }
    if (local.hashEdge) {
        config.saveScreensaverIfChanged(editingSaverValue);
        saver.configure(config.get().screensaverDelaySec = editingSaverValue);
        saver.noteActivity(now);
        state = State::SELECT; resetRepeat(); return true; }
    return true;
}

void MenuSystem::animateScroll(unsigned long now) {
    unsigned long dtMs = now - lastScrollUpdate;
    if (dtMs == 0) return;
    lastScrollUpdate = now;
    float dt = dtMs / 1000.0f;
    float target = (float)menuIndex;
    float diff = target - menuScrollPos;
    if (diff > (MENU_COUNT / 2)) diff -= MENU_COUNT; else if (diff < -(MENU_COUNT / 2)) diff += MENU_COUNT;
    float step = Defaults::MENU_SCROLL_SPEED * dt;
    if (fabs(diff) <= step) menuScrollPos = target; else {
        menuScrollPos += (diff > 0 ? step : -step);
        if (menuScrollPos < 0) menuScrollPos += MENU_COUNT; if (menuScrollPos >= MENU_COUNT) menuScrollPos -= MENU_COUNT;
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
    editingSaverValue = current - (current % 10);
    state = State::SAVER_EDIT;
    resetRepeat();
    ignoreFirstHashEdgeSaver = true;
}

void MenuSystem::repeatHandler(const ButtonState& bs, unsigned long now) {
    if (!repeatInited) { repeatInited=true; holdStart=0; }
    actUp = bs.upEdge; actDown = bs.downEdge;
    bool heldAny = bs.up || bs.down;
    if (heldAny) {
        if (holdStart==0) { holdStart=now; lastStep=now; }
        unsigned long held = now - holdStart;
        if (held > Defaults::EDIT_INITIAL_DELAY_MS) {
            if (now - lastStep >= Defaults::EDIT_REPEAT_INTERVAL_MS) {
                if (bs.up) actUp = true; if (bs.down) actDown = true; lastStep = now;
            } else { actUp = actDown = false; }
        } else {
            if (!bs.upEdge) actUp=false; if (!bs.downEdge) actDown=false;
        }
    } else {
        holdStart=0;
    }
}

void MenuSystem::resetRepeat() { holdStart=0; lastStep=0; repeatInited=false; actUp=actDown=false; }

void MenuSystem::enterHelp() {
    state = State::HELP;
    helpScroll = 0;
    helpScrollPos = 0.0f;
    helpScrollTarget = 0;
    lastHelpAnimMs = millis();
    menuHint = false;
    if (Serial) Serial.println(F("Entering HELP"));
}

void MenuSystem::handleHelp(const ButtonState& bs) {
    if (state != State::HELP) return;
    if (bs.upEdge) { if (helpScrollTarget>0) helpScrollTarget--; }
    if (bs.downEdge) { if (helpScrollTarget < HELP_LINES_COUNT-4) helpScrollTarget++; }
    if (bs.hashEdge || bs.starEdge) { state = State::SELECT; if (Serial) Serial.println(F("Exit HELP")); }
}

void MenuSystem::updateHelpAnimation(unsigned long now) {
    if (state != State::HELP) return;
    unsigned long dt = now - lastHelpAnimMs;
    if (dt == 0) return;
    lastHelpAnimMs = now;
    // simple smooth approach: move at fixed speed lines/sec toward target
    const float speed = 8.0f; // lines per second
    float diff = (float)helpScrollTarget - helpScrollPos;
    float step = speed * (dt / 1000.0f);
    if (fabs(diff) <= step) {
        helpScrollPos = (float)helpScrollTarget;
        helpScroll = helpScrollTarget;
    } else {
        helpScrollPos += (diff > 0 ? step : -step);
        // update integer anchor
        helpScroll = (int)floor(helpScrollPos + 0.001f);
    }
}

void MenuSystem::processInput(const ButtonState& bs, unsigned long now, Config& config, Screensaver& saver) {
        switch(state) {
            case State::SELECT:
                // navigation handled separately via navigate(); selection via hashEdge
                if (bs.hashEdge) {
                    selectedMenu = menuIndex;
                    if (selectedMenu == 0) { beginSaverEdit(config.get().screensaverDelaySec); }
                    else if (selectedMenu == 9) { enterHelp(); }
                    else { state = State::RESULT; menuResultStart = now; }
                } else if (bs.starEdge) {
                    state = State::INACTIVE;
                }
                break;
            case State::SAVER_EDIT:
                handleSaverEdit(bs, now, config, saver); // already consumes
                break;
            case State::HELP:
                handleHelp(bs);
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
