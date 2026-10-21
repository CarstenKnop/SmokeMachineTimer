// FogJsonLine.h - JSON-lines transport over the USB serial port (FogSerial/2, see docs/SERIAL_PROTOCOL.md).
//
// Never blocks the main loop: outgoing data that does not fit into the USB buffer is dropped (events) or
// retried for a few milliseconds (responses). Incoming lines are parsed one at a time.
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

namespace Fog {

class JsonLine {
public:
    using Handler = std::function<void(JsonDocument& doc)>;

    static constexpr size_t kMaxLine = 4096;

    // Call before Serial.begin() so the bigger buffer takes effect.
    static void prepareSerial(HWCDC& serial);

    void begin(Stream& stream, Handler handler);
    void poll(uint32_t nowMs);

    // PC session: true for 6 s after the last valid line.
    bool pcPresent(uint32_t nowMs) const { return lastRxMs_ && (nowMs - lastRxMs_) < 6000; }

    // Serialises 'doc' + '\n'. critical = a response that must get out; otherwise it may be dropped.
    bool send(const JsonDocument& doc, bool critical);

    uint32_t linesRx = 0, linesTx = 0, badJson = 0, txDrop = 0;

private:
    Stream* stream_ = nullptr;
    Handler handler_;
    char buf_[kMaxLine];
    size_t len_ = 0;
    bool overflow_ = false;
    uint32_t lastRxMs_ = 0;
    char out_[kMaxLine + 2];
};

}  // namespace Fog
