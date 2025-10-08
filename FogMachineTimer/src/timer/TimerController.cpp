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
        outputState = true;
        digitalWrite(pin, HIGH);
        return;
    }
    float elapsed = (now - lastSwitch) / 1000.0f;
    currentStateSeconds = elapsed;
    if (outputState) {
        if (elapsed >= ton) {
            outputState = false;
            lastSwitch = now;
            digitalWrite(pin, LOW);
        }
    } else {
        if (elapsed >= toff) {
            outputState = true;
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
    outputOverride = on;
    digitalWrite(pin, on ? HIGH : LOW);
}

void TimerController::resetState() {
    outputState = false;
    lastSwitch = millis();
    digitalWrite(pin, LOW);
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
