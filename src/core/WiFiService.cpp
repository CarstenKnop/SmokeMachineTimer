#include "WiFiService.h"

void WiFiService::begin(const char* apSsid, const char* apPass, uint16_t port) {
  if (started) return;
  ssid = apSsid; pass = apPass; serverPort = port;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), pass.length()? pass.c_str(): nullptr);
  apIP = WiFi.softAPIP();
  setupRoutes();
  server.begin();
  dns.begin(apIP); // start captive DNS
  started=true;
  if (Serial) {
    Serial.print(F("WiFi AP started: SSID=")); Serial.print(ssid); Serial.print(F(" IP=")); Serial.println(apIP);
  }
}

void WiFiService::setupRoutes() {
  server.on("/", [this](){
  String html = F("<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>FogMachineTimer</title><style>body{font-family:sans-serif;margin:10px;}h1{font-size:1.2em;}label{display:block;margin-top:8px;}footer{margin-top:16px;font-size:0.7em;color:#666;}button{font-size:1em;padding:4px 10px;margin-top:8px;}</style></head><body><h1>Fog Machine Timer</h1>");
    html += F("<p>Off Time (tenths): "); html += F("<span id='off'></span></p>");
    html += F("<p>On Time (tenths): <span id='on'></span></p>");
    html += F("<p><em>Adjust values on device for now.</em></p>");
    html += F("<script>fetch('/values').then(r=>r.json()).then(j=>{off.innerText=j.off;on.innerText=j.on;});</script>");
    html += F("<footer>Portal captured at "); html += WiFi.softAPIP().toString(); html += F("</footer></body></html>");
    server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
    server.sendHeader(F("Pragma"), F("no-cache"));
    server.sendHeader(F("Expires"), F("0"));
    server.send(200, F("text/html"), html);
  });
  // Captive portal detection endpoints - respond with content to trigger portal display
  auto portalHtml = F("<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>FogTimer Portal</title></head><body><h2>FogMachineTimer</h2><p>Device captive portal.</p><p><a href='/' style='font-size:1.2em'>Open Control Page</a></p></body></html>");
  server.on("/generate_204", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /generate_204")); server.send(200,F("text/html"),portalHtml); }); // Android
  server.on("/gen_204", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /gen_204")); server.send(200,F("text/html"),portalHtml); });
  server.on("/library/test/success.html", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /library/test/success.html")); server.send(200,F("text/html"),portalHtml); }); // iOS older
  server.on("/hotspot-detect.html", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /hotspot-detect.html")); server.send(200,F("text/html"),portalHtml); }); // iOS/macOS
  server.on("/kindle-wifi/wifistub.html", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: kindle")); server.send(200,F("text/html"),portalHtml); });
  server.on("/ncsi.txt", [this](){ if(Serial) Serial.println(F("Captive: /ncsi.txt")); server.send(200,F("text/plain"),F("FogTimer")); }); // Windows expects a specific text, but any differing triggers portal
  server.on("/connecttest.txt", [this](){ if(Serial) Serial.println(F("Captive: /connecttest.txt")); server.send(200,F("text/plain"),F("FogTimer")); });
  server.on("/redirect", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /redirect")); server.send(200,F("text/html"),portalHtml); });
  server.on("/success.txt", [this](){ if(Serial) Serial.println(F("Captive: /success.txt")); server.send(200,F("text/plain"),F("FogTimer")); });
  server.on("/chrome-variations/seed", [this, portalHtml](){ if(Serial) Serial.println(F("Captive: /chrome-variations/seed")); server.send(200,F("text/html"),portalHtml); });
  server.on("/values", [this](){
    // Placeholder; real values will be injected externally by global access or callbacks
    String json = F("{\"off\":0,\"on\":0}");
    server.send(200, F("application/json"), json);
  });
  server.onNotFound([this](){
    if(Serial) Serial.println(F("Portal fallback (404->portal)"));
    // Serve portal for any unknown path (forces walled garden effect)
  String html = F("<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Portal</title></head><body><h2>FogMachineTimer Portal</h2><p>This network is captive. <a href='/' style='font-size:1.1em'>Control Page</a></p></body></html>");
    server.sendHeader(F("Cache-Control"), F("no-store, no-cache, must-revalidate, max-age=0"));
    server.sendHeader(F("Pragma"), F("no-cache"));
    server.sendHeader(F("Expires"), F("0"));
    server.send(200, F("text/html"), html);
  });
}

void WiFiService::loop() { if (started) { server.handleClient(); dns.loop(); } }

String WiFiService::qrContent() const {
  // Basic http URL for AP root
  String url = F("http://"); url += apIP.toString(); if (serverPort!=80){ url += ':'; url += serverPort; }
  return url;
}
