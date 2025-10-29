// TimerController.cpp
// Implements the fog machine timer logic.
#include "TimerController.h"

TimerController::TimerController(uint8_t outputPin)
    : pin(outputPin), ton(0.1f), toff(10.0f), outputOverride(false), outputState(false), lastSwitch(0), currentStateSeconds(0) {}

void TimerController::begin(float tonSeconds, float toffSeconds) {
    ton = tonSeconds;
    toff = toffSeconds;
    outputOverride = false;
    outputState = false;
    lastSwitch = millis();
    currentStateSeconds = 0;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

void TimerController::update(unsigned long now) {
    if (outputOverride) {
        bool prev = outputState;
        outputState = true;
        digitalWrite(pin, HIGH);
        if (outputState != prev) stateChangedFlag = true;
        return;
    }
    float elapsed = (now - lastSwitch) / 1000.0f;
    currentStateSeconds = elapsed;
    if (outputState) {
        if (elapsed >= ton) {
            outputState = false; stateChangedFlag = true;
            lastSwitch = now;
            digitalWrite(pin, LOW);
        }
    } else {
        if (elapsed >= toff) {
            outputState = true; stateChangedFlag = true;
            lastSwitch = now;
            digitalWrite(pin, HIGH);
        }
    }
}

void TimerController::setTimes(float tonSeconds, float toffSeconds) {
    ton = tonSeconds;
    toff = toffSeconds;
}

void TimerController::overrideOutput(bool on) {
    bool prev = outputState;
    outputOverride = on;
    digitalWrite(pin, on ? HIGH : LOW);
    outputState = on;
    if (outputState != prev) stateChangedFlag = true;
}

void TimerController::resetState() {
    bool prev = outputState;
    outputState = false;
    lastSwitch = millis();
    digitalWrite(pin, LOW);
    if (outputState != prev) stateChangedFlag = true;
}

void TimerController::toggleAndReset() {
    // Invert current state, reset timer baseline, resume normal cycling (no permanent override)
    outputOverride = false; // ensure override not latched
    outputState = !outputState; stateChangedFlag = true;
    lastSwitch = millis();
    digitalWrite(pin, outputState ? HIGH : LOW);
}

bool TimerController::isOutputOn() const {
    return outputState;
}

float TimerController::getTon() const {
    return ton;
}

float TimerController::getToff() const {
    return toff;
}

float TimerController::getCurrentStateSeconds() const {
    return currentStateSeconds;
}

bool TimerController::consumeStateChanged() {
    bool v = stateChangedFlag; stateChangedFlag = false; return v;
}

bool TimerController::isOverrideActive() const {
    return outputOverride;
}
