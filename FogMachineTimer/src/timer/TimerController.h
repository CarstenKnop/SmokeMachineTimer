// TimerController.h
// Implements the fog machine timer logic: manages ON/OFF cycles and output control.
#pragma once
#include <Arduino.h>

class TimerController {
public:
    TimerController(uint8_t outputPin);
    void begin(float tonSeconds, float toffSeconds);
    void update(unsigned long now);
    void setTimes(float tonSeconds, float toffSeconds);
    void overrideOutput(bool on);
    void resetState();
    void toggleAndReset();
        bool consumeStateChanged();
    bool isOutputOn() const;
    float getTon() const;
    float getToff() const;
    float getCurrentStateSeconds() const;
private:
    uint8_t pin;
    float ton, toff;
    bool outputOverride;
    bool outputState;
    unsigned long lastSwitch;
    float currentStateSeconds;
        bool stateChangedFlag=false;
};
