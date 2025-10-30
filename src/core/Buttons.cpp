#include "Buttons.h"

void Buttons::begin() {
    // Button reads disabled by request. Do not configure or sample the pins so
    // the timer firmware will ignore any front-panel button presses.
    // Leave last* values cleared so no edge events are reported.
    lastUp = lastDown = lastHash = lastStar = false;
}

ButtonState Buttons::poll() {
    ButtonState st;
    // Buttons disabled: report all buttons as not pressed and no edges.
    st.up = st.down = st.hash = st.star = false;
    st.upEdge = st.downEdge = st.hashEdge = st.starEdge = false;
    // Keep last* cleared to avoid spurious edges if buttons are re-enabled later.
    lastUp = lastDown = lastHash = lastStar = false;
    return st;
}
