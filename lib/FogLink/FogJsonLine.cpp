#include "FogJsonLine.h"

namespace Fog {

void JsonLine::prepareSerial(HWCDC& serial) {
    serial.setTxBufferSize(8192);      // a full 10-timer fleet event is ~2.2 KB
    serial.setRxBufferSize(4096);
    serial.setTxTimeoutMs(0);          // never stall the radio loop when nobody reads the port
}

void JsonLine::begin(Stream& stream, Handler handler) {
    stream_ = &stream;
    handler_ = handler;
    len_ = 0;
    overflow_ = false;
}

void JsonLine::poll(uint32_t nowMs) {
    if (!stream_) return;
    int budget = 512;                  // bound the work per call
    while (budget-- > 0 && stream_->available() > 0) {
        const int c = stream_->read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c != '\n') {
            if (len_ < kMaxLine - 1) buf_[len_++] = static_cast<char>(c);
            else overflow_ = true;
            continue;
        }
        // end of line
        const size_t n = len_;
        len_ = 0;
        const bool tooLong = overflow_;
        overflow_ = false;
        if (n == 0) continue;
        buf_[n] = 0;
        if (buf_[0] != '{') continue;                  // plain text (boot log etc.) - not ours
        if (tooLong) {
            ++badJson;
            JsonDocument res;
            res["t"] = "res";
            res["ok"] = false;
            res["err"] = "badRequest";
            res["msg"] = "line too long";
            send(res, true);
            continue;
        }
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, buf_, n);
        if (err || !doc["t"].is<const char*>()) {
            ++badJson;
            JsonDocument res;
            res["t"] = "res";
            res["ok"] = false;
            res["err"] = "badRequest";
            res["msg"] = err ? err.c_str() : "missing t";
            send(res, true);
            continue;
        }
        ++linesRx;
        lastRxMs_ = nowMs ? nowMs : 1;
        if (handler_) handler_(doc);
    }
}

bool JsonLine::send(const JsonDocument& doc, bool critical) {
    if (!stream_) return false;
    if (measureJson(doc) >= kMaxLine) { ++txDrop; return false; }      // never emit a truncated (invalid) line
    const size_t n = serializeJson(doc, out_, kMaxLine);
    if (n == 0) { ++txDrop; return false; }
    out_[n] = '\n';
    const size_t total = n + 1;
    // USB-CDC exposes the free space of its TX ring; never wait for the host.
    int tries = critical ? 20 : 1;
    while (stream_->availableForWrite() < static_cast<int>(total)) {
        if (--tries <= 0) { ++txDrop; return false; }
        delay(1);
    }
    stream_->write(reinterpret_cast<const uint8_t*>(out_), total);
    ++linesTx;
    return true;
}

}  // namespace Fog
