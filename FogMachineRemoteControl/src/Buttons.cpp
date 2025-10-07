#include "Buttons.h"

void Buttons::begin() {
    pinMode(Defaults::BTN_UP, INPUT_PULLUP);
    pinMode(Defaults::BTN_DOWN, INPUT_PULLUP);
    pinMode(Defaults::BTN_HASH, INPUT_PULLUP);
    pinMode(Defaults::BTN_STAR, INPUT_PULLUP);
}

ButtonState Buttons::poll() {
    ButtonState st;
    bool up = !digitalRead(Defaults::BTN_UP);
    bool down = !digitalRead(Defaults::BTN_DOWN);
    bool hash = !digitalRead(Defaults::BTN_HASH);
    bool star = !digitalRead(Defaults::BTN_STAR);
    st.upEdge = up && !lastUp; st.downEdge = down && !lastDown;
    st.hashEdge = hash && !lastHash; st.starEdge = star && !lastStar;
    st.up = up; st.down = down; st.hash = hash; st.star = star;
    lastUp = up; lastDown = down; lastHash = hash; lastStar = star;
    return st;
}
