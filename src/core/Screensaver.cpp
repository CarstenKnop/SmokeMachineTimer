#include "Screensaver.h"

void Screensaver::begin(Adafruit_SSD1306* d) { display = d; }
void Screensaver::configure(uint16_t delaySec) { delaySeconds = delaySec; recompute(); }
void Screensaver::noteActivity(unsigned long now) {
    lastActivity = now;
    if (delaySeconds>0) nextBlankAt = lastActivity + (unsigned long)delaySeconds*1000UL; else nextBlankAt=0;
}
void Screensaver::loop(unsigned long now) {
    if (!blanked && nextBlankAt!=0 && (long)(now - nextBlankAt) >= 0) {
        display->ssd1306_command(SSD1306_DISPLAYOFF);
        blanked = true;
    }
}
bool Screensaver::handleWake(const ButtonState& bs, unsigned long now) {
    if (!blanked) return false;
    if (bs.up || bs.down || bs.hash || bs.star) {
        display->ssd1306_command(SSD1306_DISPLAYON);
        blanked = false;
        noteActivity(now);
        consume = true;
        return true;
    }
    return false;
}
unsigned long Screensaver::remainingMs(unsigned long now) const {
    if (blanked) return 0;
    if (delaySeconds==0 || nextBlankAt==0) return 0;
    long diff = (long)(nextBlankAt - now);
    if (diff <= 0) return 0;
    return (unsigned long)diff;
}
uint16_t Screensaver::remainingSeconds(unsigned long now) const {
    unsigned long ms = remainingMs(now);
    if (ms==0) return 0;
    return (uint16_t)((ms + 999UL)/1000UL);
}
void Screensaver::recompute() {
    if (delaySeconds>0) nextBlankAt = lastActivity + (unsigned long)delaySeconds*1000UL; else nextBlankAt=0;
}
