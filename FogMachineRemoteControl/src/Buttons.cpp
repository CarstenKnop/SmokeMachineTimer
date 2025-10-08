#include "Buttons.h"

void Buttons::begin() {
    pinMode(BUTTON_UP_PIN, INPUT_PULLUP);
    pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);
    pinMode(BUTTON_LEFT_PIN, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT_PIN, INPUT_PULLUP);
}

ButtonState Buttons::poll() {
    ButtonState st;
    bool up = !digitalRead(BUTTON_UP_PIN);
    bool down = !digitalRead(BUTTON_DOWN_PIN);
    bool left = !digitalRead(BUTTON_LEFT_PIN);
    bool right = !digitalRead(BUTTON_RIGHT_PIN);
    st.upEdge = up && !lastUp; st.downEdge = down && !lastDown;
    st.leftEdge = left && !lastLeft; st.rightEdge = right && !lastRight;
    st.up = up; st.down = down; st.left = left; st.right = right;
    // legacy mappings
    st.hash = st.left; st.star = st.right;
    st.hashEdge = st.leftEdge; st.starEdge = st.rightEdge;
    lastUp = up; lastDown = down; lastLeft = left; lastRight = right;
    return st;
}
