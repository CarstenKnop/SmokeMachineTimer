# 📱 Single QR Code Onboarding for ESP32 IoT Devices  
(Unified Wi‑Fi provisioning + captive portal web app)

---

## 🔄 What This Guide Covers
This document explains how to onboard a user to an **ESP32-based IoT device** using a *single QR code*. The QR code encodes Wi‑Fi credentials for a SoftAP (device-hosted Wi‑Fi network). Once the user connects, a **captive portal** automatically (or semi‑automatically) opens, serving a locally hosted web interface (status UI, configuration, sensor data, etc.).

It also provides:
- ✅ Validated QR encoding format and caveats  
- ✅ Multiple implementation variants (Arduino / ESP-IDF / Async)  
- ✅ Expanded code examples (Wi‑Fi, DNS hijack, captive portal logic, APIs)  
- ✅ Platform behavior (iOS vs Android vs Desktop)  
- ✅ Security hardening & production considerations  
- ✅ Alternatives (BLE, SmartConfig, Easy Connect)  
- ✅ Troubleshooting & test matrix  

---

## 🧱 Architecture Overview

```
+-------------------+        Scan QR         +----------------------+ 
|   Printed QR      |  ------------------>   |  User Device (Phone) | 
| (Wi-Fi credentials|                        |  - Parses WIFI:...   | 
|  + optional URL)  |                        |  - Offers to join AP | 
+-------------------+                        +-----------+----------+ 
                                                          | 
                                             Joins SoftAP | 
                                                          v
                                         +---------------------------+
                                         | ESP32 (SoftAP + DNS + HTTP|
                                         | - SoftAP: SSID/PW         |
                                         | - DNS Hijack (wildcard)   |
                                         | - Captive Portal HTTP UI  |
                                         | - REST/WS endpoints       |
                                         +-----------+---------------+
                                                     | 
                                           (Optional) bridge to cloud
```

---

## 🧪 Reality Check: Common Misconceptions

| Claim | Reality |
|-------|---------|
| “Scanning a Wi-Fi QR connects automatically.” | Android often auto-connects; iOS usually prompts the user to tap Join. |
| “Captive portal always pops up automatically.” | Not guaranteed; depends on OS heuristics & DNS/HTTP behavior. |
| “Self-signed HTTPS fixes everything.” | Modern captive portals + self-signed TLS often cause warnings; many embedded flows stay on HTTP locally. |
| “Wildcard DNS alone triggers portal.” | Must also satisfy OS connectivity-check expectations (e.g., hijacking known probe domains). |
| “One QR works forever.” | SSID/password reuse can become a security, scaling, or collision issue. Consider per-device dynamic QR codes. |

---

## 🗂️ QR Code Types & Formats

### 1. Wi-Fi QR (Primary)
Format (case-insensitive key order not enforced):
```
WIFI:T:WPA;S:MyDevice_1234;P:MyPass!23;H:false;;
```
Fields:
- `T:` Authentication (`WPA`, `WPA2`, `WEP`, or omit/`nopass`).
- `S:` SSID (escape `;`, `,`, `\` with backslash: `\;`).
- `P:` Password (omit if `nopass`).
- `H:` `true|false` (hidden network), optional.
- MUST end with `;;`.

Edge Cases:
- If password contains `;` → escape: `P:My\;Pass;`
- Length: Keep under ~60 chars for readability in QR (ECC impacts complexity).

### 2. Dual / Multi-Stage QR (Wi-Fi + Fallback URL)
You cannot “multi-launch” actions natively; but you can:
- Primary QR: Wi-Fi provisioning
- Secondary small QR (or printed URL) for online help (docs, firmware portal)
- OR encode a short onboarding URL only; manual Wi-Fi join occurs later.

### 3. Dynamic / Signed Payload QR
Embed JSON (base64url) with:
```
{
  "ssid":"DEV_ABC123",
  "pw":"fe73c9ab",
  "exp":"2025-10-10T12:00:00Z",
  "sig":"HMAC256..."
}
```
Then parse in a provisioning app (requires a custom mobile app → defeats “no app” simplicity, but stronger security).

### 4. Tokenized Temporary Credentials
Backend issues time-bound password; QR printed at final assembly or generated on-demand.

---

## 🖨️ C# QR Generation (QRCoder) – Improved Example

```csharp
using System;
using System.Drawing;
using System.Drawing.Imaging;
using QRCoder;

class WifiQrGenerator
{
    public static void Main()
    {
        var ssid = "ESP_Sensor_1234";
        var password = "yourpassword";
        string wifiPayload = $"WIFI:T:WPA;S:{Escape(ssid)};P:{Escape(password)};;";

        using var generator = new QRCodeGenerator();
        var data = generator.CreateQrCode(wifiPayload, QRCodeGenerator.ECCLevel.Q); // Q ~25% recovery
        var qr = new PngByteQRCode(data);
        var pngBytes = qr.GetGraphic(pixelsPerModule: 8,
                                     darkColor: Color.Black,
                                     lightColor: Color.White,
                                     drawQuietZones: true);

        System.IO.File.WriteAllBytes("wifi_qr.png", pngBytes);
        Console.WriteLine("QR generated: wifi_qr.png\nPayload:\n" + wifiPayload);
    }

    static string Escape(string raw) =>
        raw.Replace("\\", "\\\\").Replace(";", "\\;").Replace(",", "\\,");
}
```

Enhancements to consider:
- Add a margin check to ensure scannability.
- Batch generate per-device QR with unique SSIDs/passwords.
- Embed a small logo (only if high ECC level; test scanning).
- Output SVG for print scalability:
  ```csharp
  var svg = new SvgQRCode(data);
  string svgText = svg.GetGraphic(4);
  System.IO.File.WriteAllText("wifi_qr.svg", svgText);
  ```

---

## 🧩 ESP32 SoftAP + Captive Portal (Arduino Framework)

### Libraries
- `WiFi.h`
- `DNSServer.h`
- `WebServer.h` (or `ESPAsyncWebServer.h` for async performance)

### Basic SoftAP + DNS + HTTP

```cpp
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

const char* AP_SSID = "ESP_Sensor_1234";
const char* AP_PASS = "yourpassword";
const byte DNS_PORT = 53;

DNSServer dnsServer;
WebServer server(80);

String homepage() {
  return R"HTML(
  <!DOCTYPE html>
  <html lang=\"en\">
  <head>
    <meta charset=\"utf-8\" />
    <title>ESP Sensor Portal</title>
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />
    <style>
      body { font-family: system-ui, sans-serif; margin:1.2rem; }
      h1 { font-size:1.4rem; }
      .metric { font-family: monospace; }
      button { padding:.6rem 1rem; }
    </style>
  </head>
  <body>
    <h1>ESP Sensor</h1>
    <div>Temperature: <span id=\"temp\" class=\"metric\">--</span> °C</div>
    <div>Humidity: <span id=\"hum\" class=\"metric\">--</span> %</div>
    <button onclick=\"refresh()\">Refresh</button>
    <script>
      async function refresh(){
        let r = await fetch('/api/env');
        if(r.ok){
          let j = await r.json();
          temp.textContent = j.temperature.toFixed(1);
          hum.textContent  = j.humidity.toFixed(1);
        }
      }
      refresh();
      setInterval(refresh, 5000);
    </script>
  </body>
  </html>
  )HTML";
}

void handleRoot() {
  server.send(200, "text/html", homepage());
}

void handleAPIEnv() {
  // Simulated sensor values; replace with real sensor reads.
  float temp = 22.4 + (millis() % 1000)/1000.0;
  float hum  = 48.2;
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"temperature\":%.2f,\"humidity\":%.2f}", temp, hum);
  server.send(200, "application/json", buf);
}

void captivePortalRedirect() {
  // Generic redirect for unknown paths (HTTP 302)
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "Redirecting...");
}

bool isCaptiveProbe(String host, String uri) {
  // Handle known OS captive checks
  uri.toLowerCase(); host.toLowerCase();
  if (uri == "/generate_204" || uri == "/gen_204") return true; // Android
  if (host.indexOf("connectivitycheck") != -1) return true;      // Android alt
  if (host.indexOf("apple") != -1 && uri.indexOf("hotspot") != -1) return true; // iOS
  if (host.indexOf("msftconnecttest") != -1) return true;        // Windows
  return false;
}

void handleNotFound() {
  String host = server.hostHeader();
  String uri  = server.uri();
  if (isCaptiveProbe(host, uri)) {
    // Some OS expect a 200 with simple content (not redirect) to open portal
    server.send(200, "text/html", homepage());
  } else {
    captivePortalRedirect();
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4); // channel=1, hidden=0, max 4 clients
  if (!ok) Serial.println("SoftAP start failed");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("AP IP: %s\n", apIP.toString().c_str());

  // Wildcard DNS: respond with our IP to any query
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handleRoot);
  server.on("/api/env", handleAPIEnv);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
}
```

### Notes
- Returning a redirect (302) for OS probe endpoints may prevent portal auto-popup. Test variations: 200 vs 302 vs 204.
- Keep responses lean (< 50 KB) for low memory / speed.
- Optionally gzip HTML and set: `server.sendHeader("Content-Encoding","gzip");`.

---

## 🔀 Async Variant (ESPAsyncWebServer)

Advantages:
- Non-blocking
- Better with WebSockets / streaming updates

```cpp
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

const char* AP_SSID = "ESP_Sensor_1234";
const char* AP_PASS = "yourpassword";
DNSServer dnsServer;
AsyncWebServer server(80);

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset=\"utf-8\"/><title>Async Portal</title>
<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/></head>
<body><h3>ESP Async Portal</h3><div id=\"v\"></div>
<script>
async function loop(){
 let r=await fetch('/api/env');
 if(r.ok){ let j=await r.json(); v.textContent = JSON.stringify(j); }
}
setInterval(loop, 3000); loop();
</script></body></html>)HTML";

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  dnsServer.start(53, "*", ip);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/env", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<128> doc;
    doc["temperature"] = 22.5;
    doc["humidity"] = 48.2;
    char buf[128];
    serializeJson(doc, buf);
    req->send(200, "application/json", buf);
  });
  server.onNotFound([](AsyncWebServerRequest *req){
    req->redirect("/");
  });
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
}
```

(Requires ArduinoJson and ESPAsyncWebServer libs.)

---

## 🛠️ ESP-IDF Snippet (SoftAP + HTTP)

```c
// Pseudocode (ESP-IDF v5.x)
wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_AP);
wifi_config_t ap_config = {
  .ap = {
    .ssid = "ESP_Sensor_1234",
    .ssid_len = 0,
    .password = "yourpassword",
    .channel = 1,
    .authmode = WIFI_AUTH_WPA2_PSK,
    .max_connection = 4,
    .beacon_interval = 100,
  },
};
esp_wifi_set_config(WIFI_IF_AP, &ap_config);
esp_wifi_start();

// For DNS + HTTP use lwIP raw sockets or integrate esp_async_webserver IDF port.
```

---

## 🌐 Captive Portal Behavior by Platform

| Platform | Trigger Mechanism | Probe URLs (examples) | Recommended Response |
|----------|-------------------|------------------------|---------------------|
| Android 6+ | HTTP 204 probe   | `http://connectivitycheck.gstatic.com/generate_204` | Return 200 + HTML portal; avoids “no internet” step |
| iOS 14+ | HTTP probe page    | `http://captive.apple.com/hotspot-detect.html` | Return small valid HTML (no redirect loop) |
| Windows 10+ | HTTP probe      | `http://www.msftconnecttest.com/connecttest.txt` | Return simple HTML; sometimes 200 + custom triggers portal |
| macOS | Similar to iOS | Apple captive domains | Provide consistent HTML |
| Desktop Chrome (manual) | User navigates | Any URL → DNS hijacked | Redirect or serve root directly |

Tips:
- If portal doesn’t open automatically, instruct user: open any `http://` site (NOT `https://`) → will be intercepted.
- HTTPS initial attempts may show certificate errors if you MITM; better to rely on HTTP for captive detection.

---

## 🔐 Security Hardening

| Risk | Mitigation |
|------|------------|
| Hardcoded password reused across fleet | Per-device randomized credentials; print QR at manufacturing |
| Offline portal spoofing | Display device serial & signed token in UI |
| Local HTTP sniffing (open network) | Use WPA2; optionally short-lived AP sessions |
| Replay of captured credentials | Rotate credentials post-onboarding; time-bound tokens |
| Unauthorized lingering connections | Idle timeout → call `WiFi.softAPdisconnect(true)` after N mins |
| Firmware tampering | Enable Secure Boot + Flash Encryption (ESP32 features) |
| Lack of OTA update path | Add `/update` protected endpoint or use ESP32 OTA library |
| CSRF on config endpoints | Require a session nonce or HMAC header |

### Ephemeral Credential Strategy (Simple)
1. On boot, generate random password (`16` chars).  
2. Display password on small OLED or serial during provisioning; encode into freshly generated QR.  
3. Once user finalizes config (e.g., enters home Wi‑Fi), disable SoftAP.

---

## 🗺️ Suggested REST / JSON Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/env` | GET | Sensor snapshot |
| `/api/info` | GET | Firmware, build, uptime |
| `/api/scan` | GET | (Optional) Surrounding Wi-Fi networks (STA mode temporarily) |
| `/api/config` | POST | Apply device configuration (JSON) |
| `/api/reboot` | POST | Controlled restart |
| `/metrics` | GET | Prometheus-style text (if bridging to local gateway) |

Example config POST body:
```json
{
  "cloudKey": "abcd1234",
  "reportIntervalSec": 60,
  "threshold": { "tempHigh": 40.0 }
}
```

---

## 🧪 Testing Matrix

| Scenario | Expected | Test Notes |
|----------|----------|------------|
| Android phone scan QR | Prompts connect → captive pops in <10s | Test with “data off” & “data on” |
| iPhone scan QR | Prompts join → may or may not auto portal | Open Safari if no popup |
| Windows laptop manual join | No auto portal unless HTTP navigation | Try `neverssl.com` |
| Multiple clients (≥4) | 5th denied | Adjust `max_connection` |
| High DNS query volume | Device remains responsive | Monitor `dnsServer` timing |
| Lost probe handling | Portal eventually opens manually | Provide fallback instructions |

---

## 🛠️ Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| QR not scannable | Too dense / low contrast | Increase module size / print quality |
| Portal not auto-opening (iOS) | OS caching / probe mismatch | Respond with simple HTML to Apple probe |
| Device “Connected, no internet” banner | Expected (no WAN) | Provide wording in UI: “Local device network” |
| DNS not catching all | DNS server not started early | Start DNS before HTTP serve |
| Browser stuck redirect loop | Redirecting probe endpoints | Serve 200 for known probe requests |
| Memory fragmentation | Large String concatenations | Use PROGMEM and `server.send_P` |

---

## 🔄 Optional Enhancements

| Feature | Approach |
|---------|----------|
| WebSockets Live Data | `AsyncWebSocket` for push updates |
| Service Worker Offline Cache | Register SW; all assets local |
| Local Authentication | `/login` sets HttpOnly cookie; simple session map |
| BLE Alternative | BLE GATT service with characteristics for SSID/PW |
| SmartConfig | Use ESP-IDF SmartConfig; requires mobile app support |
| Wi-Fi Easy Connect (DPP) | Emerging standard; more complex provisioning |
| mDNS | Advertise `_esp-sensor._tcp.local` for host discovery |
| OTA | `ArduinoOTA` or HTTPS GET signed bundle |
| Mesh | ESP-NOW for sensor cluster; Wi-Fi AP only for provisioning |

---

## 🧪 Example: Enabling mDNS (Arduino)

```cpp
#include <ESPmDNS.h>

if (MDNS.begin("esp-sensor")) {
  Serial.println("mDNS responder started: http://esp-sensor.local");
  MDNS.addService("http", "tcp", 80);
}
```

---

## 🧾 Production Checklist

- [ ] Unique per-device Wi-Fi credentials or ephemeral SoftAP mode
- [ ] Firmware version visible in portal
- [ ] Secure Boot + Flash Encryption (if protecting IP)
- [ ] Logging ring buffer (viewable `/logs`)
- [ ] Watchdog enabled
- [ ] Graceful memory usage (no giant dynamic allocations)
- [ ] Probe endpoint logic tested across OS versions
- [ ] Printed label includes device serial & support URL
- [ ] QR validated on low-end scanners / older phones
- [ ] Fallback instructions: “If portal doesn’t open, open any non-HTTPS site”

---

## 📚 References & Further Reading
- QRCoder Library: https://github.com/codebude/QRCoder  
- Wi-Fi QR Format (de-facto spec): https://github.com/zxing/zxing/wiki/Barcode-Contents#wifi-network-config-android-enterprise  
- ESP-IDF Docs (Wi-Fi): https://docs.espressif.com/  
- Captive Portal Behavior (Android): https://source.android.com/docs/core/connectivity  
- ESPAsyncWebServer: https://github.com/me-no-dev/ESPAsyncWebServer  

---

## 🏁 Summary
A single QR-based onboarding flow is practical and user-friendly for local-only provisioning. The key to reliability is:
- Proper QR encoding (escape characters, correct terminator)
- Robust SoftAP + wildcard DNS + captive portal response tailoring per OS
- Lightweight, well-structured local UI + JSON API
- Security hardening: ephemeral credentials, session control, signed firmware

For scalable and secure deployments, integrate dynamic credential issuance, optional cloud fallback, and standardized update mechanisms.

---

## ✅ Revision Notes (What Changed vs Original)
- Clarified Wi-Fi QR code syntax and escaping
- Added robust, production-grade code examples (Arduino sync + Async + ESP-IDF pseudo)
- Added captive portal OS probe handling logic
- Expanded security recommendations (ephemeral creds, secure boot, session handling)
- Added dynamic & tokenized QR strategies
- Added REST API structuring, test matrix, troubleshooting, and production checklist
- Corrected “auto connect” assumption and platform caveats
- Added performance & memory best practices

---

Feel free to adapt or request a trimmed printable variant (1-page quick deploy aide).