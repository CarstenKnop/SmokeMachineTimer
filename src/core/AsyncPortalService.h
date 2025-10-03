#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

// Lightweight Async captive portal + OTA uploader.
// Starts its own SoftAP; provides QR-friendly SSID/PASS accessors.
class AsyncPortalService {
public:
  struct AuthConfig { const char* user; const char* pass; };
  void setAuth(const char* user, const char* pass) { authUser = user; authPass = pass; }
  void setControlAuth(const char* user, const char* pass) { ctrlUser = user; ctrlPass = pass; }
  void setOtaAuth(const char* user, const char* pass) { otaUser = user; otaPass = pass; }
  bool begin(const char* apSsid, const char* apPass, uint16_t port=80) {
    if (started) return true;
    ssid = apSsid ? apSsid : "PortalAP";
    pass = apPass ? apPass : "";
    WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(ssid.c_str(), pass.length()==0? nullptr: pass.c_str())) {
      return false;
    }
    apIP = WiFi.softAPIP();
    dnsServer.start(DNS_PORT, "*", apIP);
    setupRoutes(port);
    started = true;
    return true;
  }
  void loop() {
    if (!started) return; dnsServer.processNextRequest();
    // Update AP client count each loop (cheap call)
    apClientCount = WiFi.softAPgetStationNum();
    // Periodic SSE broadcast if any clients connected
    if (eventSource.count() > 0) {
      unsigned long now = millis();
      if (now - lastSsePushMs >= sseIntervalMs) {
        lastSsePushMs = now;
        if (statusFn) {
          String json; statusFn(json);
          if (json.length()) {
            // Ensure JSON ends with '}' before injecting appended fields
            bool closed = json.length() && json[json.length()-1] == '}';
            if (closed) json.remove(json.length()-1); // strip trailing }
            json += F(",\"apClients\":"); json += apClientCount;
            long msSinceAuth = (lastAuthSuccessMs==0)?-1: (long)(millis() - lastAuthSuccessMs);
            json += F(",\"lastAuthMs\":"); json += msSinceAuth;
            json += F(",\"staRssi\":"); json += getStaRssi();
            json += '}';
            eventSource.send(json.c_str(), "status", now);
          }
        }
      }
    }
    // STA connection state machine polling
    if (staState == StaState::CONNECTING) {
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        staState = StaState::CONNECTED;
        staLastChange = millis();
        staIp = WiFi.localIP();
      } else if (millis() - staConnectStart > staConnectTimeoutMs) {
        staState = StaState::FAILED;
        staLastChange = millis();
      }
    } else if (staState == StaState::CONNECTED) {
      // Monitor disconnect
      if (WiFi.status() != WL_CONNECTED) {
        staState = StaState::FAILED; // treat as failed; could add RETRY later
        staLastChange = millis();
      } else {
        staIp = WiFi.localIP();
      }
    }
  }
  bool isStarted() const { return started; }
  void stop() {
    if (!started) return;
    dnsServer.stop();
    server.end();
    WiFi.softAPdisconnect(true);
    started = false;
  }
  const char* getSSID() const { return ssid.c_str(); }
  const char* getPass() const { return pass.c_str(); }
  IPAddress ip() const { return apIP; }
  // Provide a callback for building JSON status: signature should write JSON string into provided String reference
  using JsonStatusFn = std::function<void(String&)>;
  void setStatusCallback(JsonStatusFn fn) { statusFn = fn; }
  using TimerUpdateFn = std::function<bool(uint32_t offTenths, uint32_t onTenths, String &err)>;
  using RelayToggleFn = std::function<bool(bool &newState)>; // toggles relay, returns success, outputs new state
  void setTimerUpdateCallback(TimerUpdateFn fn) { timerUpdateFn = fn; }
  void setRelayToggleCallback(RelayToggleFn fn) { relayToggleFn = fn; }
  // External configuration mutation callbacks
  using BoolSetterFn = std::function<void(bool)>;
  void setWifiEnableSetter(BoolSetterFn fn) { wifiEnableSetter = fn; }
  void setApAlwaysSetter(BoolSetterFn fn) { apAlwaysSetter = fn; }
  // Mirror initialization & direct update helpers (avoid exposing raw members)
  void initConfigMirror(bool wifiEn, bool apAlways) { wifiEnabledFlag = wifiEn; apAlwaysFlag = apAlways; }
  void setWifiEnabledMirror(bool v) { wifiEnabledFlag = v; }
  void setApAlwaysMirror(bool v) { apAlwaysFlag = v; }
  // STA helpers
  enum class StaState : uint8_t { IDLE, SCANNING, CONNECTING, CONNECTED, FAILED };
  StaState getStaState() const { return staState; }
  IPAddress getStaIp() const { return staIp; }
  int16_t getStaRssi() const { return (WiFi.status()==WL_CONNECTED)? WiFi.RSSI() : 0; }
  void beginScan() {
    if (staState == StaState::SCANNING) return;
    ensureApStaMode();
    if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
      WiFi.scanDelete();
      if (Serial) Serial.println(F("[SCAN] Starting async WiFi scan"));
      WiFi.scanNetworks(true /* async */);
      staState = StaState::SCANNING; staLastChange = millis();
    }
  }
  int scanResultCount() {
    int c = WiFi.scanComplete();
    if (c >= 0 && staState == StaState::SCANNING) {
      // finished
      staState = StaState::IDLE; // results available until next delete
    }
    return c;
  }
  bool beginJoin(const String& ssid, const String& pass) {
    if (ssid.length()==0) return false;
    ensureApStaMode();
    WiFi.begin(ssid.c_str(), pass.length()? pass.c_str(): nullptr);
    staState = StaState::CONNECTING; staConnectStart = millis(); staLastChange = staConnectStart; staIp = IPAddress(0,0,0,0); pendingSsid = ssid; pendingPass = pass; return true;
  }
  bool hasPendingCreds() const { return pendingSsid.length()>0; }
  String pendingSsidName() const { return pendingSsid; }
  bool connectionSucceeded() const { return staState == StaState::CONNECTED; }
  unsigned long stateAge() const { return millis() - staLastChange; }
private:
  enum class AuthKind { LegacyBoth, Control, OTA };
  bool requireAuth(AsyncWebServerRequest *req) { return requireAuthKind(req, AuthKind::LegacyBoth); }
  bool requireAuthKind(AsyncWebServerRequest *req, AuthKind kind) {
    const char* u=nullptr; const char* p=nullptr; String* cache=nullptr;
    switch(kind) {
      case AuthKind::LegacyBoth: u=authUser; p=authPass; cache=&cachedCreds; break;
      case AuthKind::Control: u=ctrlUser?ctrlUser:authUser; p=ctrlPass?ctrlPass:authPass; cache=&cachedCtrlCreds; break;
      case AuthKind::OTA: u=otaUser?otaUser:authUser; p=otaPass?otaPass:authPass; cache=&cachedOtaCreds; break;
    }
    if (!u || !p) return false; // no auth enforced
    String expected = String("Basic ") + encodedCreds(u,p,*cache);
    if (!req->hasHeader("Authorization")) {
      auto *resp = new AsyncBasicResponse(401, "text/plain", "Auth Required");
  resp->addHeader("WWW-Authenticate", "Basic realm=FogTimer");
      req->send(resp);
      return true;
    }
    const AsyncWebHeader* h = req->getHeader("Authorization");
    if (!h || h->value() != expected) {
      auto *resp = new AsyncBasicResponse(401, "text/plain", "Bad Credentials");
  resp->addHeader("WWW-Authenticate", "Basic realm=FogTimer");
      req->send(resp);
      return true;
    }
    lastAuthSuccessMs = millis();
    return false; // authorized
  }
  String encodedCreds(const char* u, const char* p, String &cache) {
    if (cache.length()) return cache;
    if (!u || !p) return String();
    String combo = String(u)+":"+p;
    // Simple base64 implementation for small string
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int len = combo.length();
    const uint8_t* bytes = (const uint8_t*)combo.c_str();
    String out; out.reserve(((len+2)/3)*4);
    for(int i=0;i<len;i+=3){
      uint32_t n = (uint32_t)bytes[i]<<16;
      if(i+1<len) n|=(uint32_t)bytes[i+1]<<8;
      if(i+2<len) n|=bytes[i+2];
      out += tbl[(n>>18)&63];
      out += tbl[(n>>12)&63];
      if(i+1<len) out += tbl[(n>>6)&63]; else out += '=';
      if(i+2<len) out += tbl[n&63]; else out += '=';
    }
    cache = out;
    return cache;
  }
  void setupRoutes(uint16_t port) {
    (void)port; // captured if needed for alt ports
    // Attach event source (SSE) before other handlers so it stays persistent
    server.addHandler(&eventSource);
    // Root/status
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
  String html = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>FogTimer</title><style>body{font-family:sans-serif;margin:14px;}h1{font-size:1.2em;}a.btn{display:inline-block;margin:6px 8px;padding:8px 12px;border:1px solid #444;border-radius:4px;text-decoration:none;}code{background:#eee;padding:2px 4px;border-radius:3px;}#s{font-size:.85em;color:#555;margin-top:10px;}ul{padding-left:18px;}footer{margin-top:28px;font-size:.7em;color:#666;}table{border-collapse:collapse;}td,th{border:1px solid #ddd;padding:4px 6px;font-size:.72em;}</style></head><body>");
  html += F("<h1>Fog Machine Timer</h1>");
      html += F("<p>AP: <code>"); html += ssid; html += F("</code> IP: <code>"); html += apIP.toString(); html += F("</code></p>");
  html += F("<div id='live'><div style='margin-bottom:6px'><button id='relayBtn' onclick='tglRelay()'>Relay</button> <form id='tform' onsubmit='return updTimers()' style='display:inline'><input id='offIn' type='number' step='0.1' min='0.1' max='999999' style='width:70px' placeholder='Off (s)'> <input id='onIn' type='number' step='0.1' min='0.1' max='999999' style='width:70px' placeholder='On (s)'> <button>Set</button></form> <span id='msg' style='font-size:.65em;color:#555'></span></div><table><tbody>");
  html += F("<tr><th>Off</th><td id='off'></td></tr>");
  html += F("<tr><th>On</th><td id='on'></td></tr>");
  html += F("<tr><th>Elapsed</th><td id='elapsed'></td></tr>");
  html += F("<tr><th>Relay</th><td id='relay'></td></tr>");
  html += F("<tr><th>Phase</th><td id='phase'></td></tr>");
  html += F("<tr><th>STA</th><td id='sta'></td></tr>");
  html += F("<tr><th>RSSI</th><td id='rssi'></td></tr>");
  html += F("<tr><th>AP Active</th><td id='apact'></td></tr>");
  html += F("<tr><th>AP Clients</th><td id='apc'></td></tr>");
  html += F("<tr><th>Last Auth (s)</th><td id='auth'></td></tr>");
  html += F("</tbody></table></div>");
      html += F("<p><a class='btn' href='/dashboard'>Dashboard</a><a class='btn' href='/wifi'>WiFi</a><a class='btn' href='/join'>Join</a><a class='btn' href='/control'>Timers</a><a class='btn' href='/scan?start=1'>Scan JSON</a><a class='btn' href='/health'>Health</a><a class='btn' href='/update'>OTA</a></p>");
  html += F("<footer id='s'>FogTimer</footer><script>function fmtAuth(ms){if(ms<0)return '';return Math.floor(ms/1000);}function tenths(v){return (v/10).toFixed(1);}function setTxt(id,v){var el=document.getElementById(id);if(el)el.textContent=v;}var es=new EventSource('/events');es.addEventListener('status',function(ev){try{var o=JSON.parse(ev.data);setTxt('off',tenths(o.off));setTxt('on',tenths(o.on));setTxt('elapsed',tenths(o.currentElapsed));setTxt('relay',o.relay?'ON':'OFF');var rb=document.getElementById('relayBtn');if(rb){rb.className=o.relay?'on':'off';rb.textContent=o.relay?'Relay ON':'Relay OFF';}setTxt('phase',o.phase);setTxt('sta',o.staConnected?'UP':o.staStatus);setTxt('rssi',(o.staRssi!==undefined)? (o.staRssi+' dBm') : '');setTxt('apact',o.apActive?(o.apSuppressed?'SUPPR':'ON'):'OFF');setTxt('apc',o.apClients);setTxt('auth',fmtAuth(o.lastAuthMs));var oi=document.getElementById('offIn');if(oi && !oi.value) oi.value=(o.off/10).toFixed(1); var ni=document.getElementById('onIn'); if(ni && !ni.value) ni.value=(o.on/10).toFixed(1);}catch(e){}});function tglRelay(){fetch('/api/relayToggle',{method:'POST'}).then(r=>r.json()).then(j=>{document.getElementById('msg').textContent=j.message||'';});}function updTimers(){var offSecs=parseFloat(document.getElementById('offIn').value);var onSecs=parseFloat(document.getElementById('onIn').value);if(isNaN(offSecs)||isNaN(onSecs)){document.getElementById('msg').textContent='Bad input';return false;}var off=Math.round(offSecs*10);var on=Math.round(onSecs*10);fetch('/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'off='+off+'&on='+on}).then(r=>r.text()).then(t=>{document.getElementById('msg').textContent=t;});return false;}</script></body></html>");
      req->send(200, F("text/html"), html);
    });

    // Values placeholder used by existing UI expectation
    server.on("/values", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return; // guard values too (optional)
      if (statusFn) {
        String json; statusFn(json);
        if (json.length()==0) json = F("{\"error\":\"empty\"}");
        req->send(200, F("application/json"), json);
      } else {
        req->send(200, F("application/json"), F("{\"off\":0,\"on\":0}"));
      }
    });

    // Timer control page (expects statusFn to reflect changes). For now only displays instructions; actual processing occurs on POST /control
    server.on("/control", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      String html = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Control</title></head><body><h2>Timer Control</h2>");
      html += F("<form method='POST' action='/control'><label>OFF (tenths)<input name='off'></label><br><label>ON (tenths)<input name='on'></label><br><button>Apply</button></form>");
      html += F("<p><a href='/'>Back</a></p></body></html>");
      req->send(200, F("text/html"), html);
    });
    // Dashboard page (live via SSE similar to root but focused layout)
    server.on("/dashboard", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
  String h = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Dashboard</title><style>body{font-family:sans-serif;margin:12px;}h2{font-size:1.1em;}table{border-collapse:collapse;}td,th{border:1px solid #ccc;padding:4px 6px;font-size:.8em;}button{padding:6px 10px;margin:4px;}#relayBtn.on{background:#4c4;color:#fff;}#relayBtn.off{background:#c44;color:#fff;}form.inline{display:inline-block;margin:6px 0;}input[type=number]{width:70px;}</style></head><body><h2>Live Dashboard</h2><div><button id='relayBtn' onclick='tglRelay()'>Toggle Relay</button><form class='inline' id='timersForm' onsubmit='return updTimers()'><label>Off (s) <input type='number' step='0.1' id='offIn' min='0.1' max='99999'></label><label> On (s) <input type='number' step='0.1' id='onIn' min='0.1' max='99999'></label><button>Apply</button></form><span id='msg' style='font-size:.7em;color:#555;margin-left:6px;'></span></div><table><tbody><tr><th>Off (s)</th><td id='off'></td></tr><tr><th>On (s)</th><td id='on'></td></tr><tr><th>Elapsed (s)</th><td id='elapsed'></td></tr><tr><th>Phase</th><td id='phase'></td></tr><tr><th>Relay</th><td id='relay'></td></tr><tr><th>STA</th><td id='sta'></td></tr><tr><th>RSSI</th><td id='rssi'></td></tr><tr><th>AP</th><td id='ap'></td></tr><tr><th>AP Clients</th><td id='apc'></td></tr><tr><th>Last Auth (s)</th><td id='auth'></td></tr></tbody></table><p><a href='/'>&larr; Home</a></p><script>function fmtAuth(ms){if(ms<0)return '';return Math.floor(ms/1000);}function tenths(v){return (v/10).toFixed(1);}function S(i,v){var e=document.getElementById(i);if(e)e.textContent=v;}var es=new EventSource('/events');es.addEventListener('status',function(ev){try{var o=JSON.parse(ev.data);S('off',tenths(o.off));S('on',tenths(o.on));S('elapsed',tenths(o.currentElapsed));S('phase',o.phase);S('relay',o.relay?'ON':'OFF');var rb=document.getElementById('relayBtn');if(rb){rb.className=o.relay?'on':'off';}S('sta',o.staConnected?'UP':o.staStatus);S('rssi',(o.staRssi!==undefined)?(o.staRssi+' dBm'):'');S('ap',o.apActive?(o.apSuppressed?'SUPPR':'ON'):'OFF');S('apc',o.apClients);S('auth',fmtAuth(o.lastAuthMs));var oi=document.getElementById('offIn');if(oi && !oi.value) oi.value=(o.off/10).toFixed(1);var ni=document.getElementById('onIn');if(ni && !ni.value) ni.value=(o.on/10).toFixed(1);}catch(e){}});function tglRelay(){fetch('/api/relayToggle',{method:'POST'}).then(r=>r.json()).then(j=>{document.getElementById('msg').textContent=j.message||'';});}function updTimers(){var offSecs=parseFloat(document.getElementById('offIn').value);var onSecs=parseFloat(document.getElementById('onIn').value);if(isNaN(offSecs)||isNaN(onSecs)){document.getElementById('msg').textContent='Bad input';return false;}var off=Math.round(offSecs*10);var on=Math.round(onSecs*10);fetch('/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'off='+off+'&on='+on}).then(r=>r.text()).then(t=>{document.getElementById('msg').textContent=t;});return false;}</script></body></html>");
      req->send(200,F("text/html"),h);
    });
    // Relay toggle API
    server.on("/api/relayToggle", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if (!relayToggleFn) { req->send(500,F("application/json"),F("{\"error\":\"no handler\"}")); return; }
      bool newState=false; bool ok = relayToggleFn(newState);
      String out = F("{"); out += F("\"ok\":"); out += (ok?1:0); out += F(",\"relay\":"); out += (newState?1:0); out += F(",\"message\":\""); out += (ok? (newState?F("Relay ON"):F("Relay OFF")) : F("Failed")); out += F("\"}");
      req->send(ok?200:500, F("application/json"), out);
    });
    // WiFi management page placeholder
    server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
  String h = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi</title><style>body{font-family:sans-serif;margin:12px;}button{margin:4px 6px;padding:6px 10px;}table{border-collapse:collapse;margin-top:8px;}td,th{border:1px solid #ccc;padding:4px 6px;font-size:.7em;}#act{font-size:.7em;color:#555;margin-top:10px;}#scanTable td{font-size:.7em;}input[type=password]{width:90px;}form.inline{display:inline-block;margin:0;}#apSsidIn{width:140px;}</style></head><body><h2>WiFi Settings</h2>");
      h += F("<p>AP: <code>"); h+=ssid; h+=F("</code> IP: <code>"); h+=apIP.toString(); h+=F("</code></p>");
      h += F("<div><button onclick=toggleWifi()>Toggle WiFi</button><button onclick=toggleApAlways()>Toggle AP Always</button><button onclick=startScan()>Scan</button><label style='font-size:.7em;margin-left:8px'>Auto <input type='checkbox' id='autoScanCk' onchange=autoScanToggle()></label></div>");
      h += F("<div style='margin:6px 0'><form class='inline' onsubmit='return chAp(event)'><label>AP SSID <input id='apSsidIn' maxlength='31' placeholder='" ); h += ssid; h += F("'></label><button>Rename</button></form></div>");
      h += F("<table><tbody><tr><th>wifiEnabled</th><td id='wifien'></td></tr><tr><th>apAlwaysOn</th><td id='apalways'></td></tr><tr><th>apActive</th><td id='apact'></td></tr><tr><th>apSuppressed</th><td id='aps'></td></tr><tr><th>staStatus</th><td id='stast'></td></tr><tr><th>staRssi</th><td id='rssi'></td></tr><tr><th>apClients</th><td id='apc'></td></tr><tr><th>lastAuth(s)</th><td id='auth'></td></tr></tbody></table>");
  h += F("<h3>Scan Results</h3><div id='scanStatus'>Idle</div><table id='scanTable'><tbody></tbody></table><p id='act'></p><p><a href='/'>&larr; Home</a></p><script>var es=new EventSource('/events');function fmtAuth(ms){if(ms<0)return '';return Math.floor(ms/1000);}function S(i,v){var e=document.getElementById(i);if(e)e.textContent=v;}es.addEventListener('status',function(ev){try{var o=JSON.parse(ev.data);S('wifien',o.wifiEnabled);S('apalways',o.apAlwaysOn);S('apact',o.apActive);S('aps',o.apSuppressed);S('stast',o.staStatus);S('rssi',(o.staRssi!==undefined)?(o.staRssi+' dBm'):'');S('apc',o.apClients);S('auth',fmtAuth(o.lastAuthMs));}catch(e){}});var scanTimer=null;var autoScan=false;function startScan(){fetch('/scan?start=1').then(()=>{document.getElementById('scanStatus').textContent='Scanning...';if(scanTimer)clearInterval(scanTimer);pollScan();scanTimer=setInterval(pollScan,1100);});}function autoScanToggle(){autoScan=document.getElementById('autoScanCk').checked;if(autoScan){startScan();}else if(scanTimer){clearInterval(scanTimer);scanTimer=null;}}function pollScan(){fetch('/scan').then(r=>r.json()).then(j=>{if(j.status==='scanning'){document.getElementById('scanStatus').textContent='Scanning...';return;}if(j.status==='idle'){document.getElementById('scanStatus').textContent='Idle';if(autoScan){startScan();}return;}if(j.status==='done'){document.getElementById('scanStatus').textContent='Done';if(!autoScan && scanTimer){clearInterval(scanTimer);scanTimer=null;}var body=document.querySelector('#scanTable tbody');body.innerHTML='';j.results.forEach(function(r){var tr=document.createElement('tr');tr.innerHTML='<td>'+r.ssid+'</td><td>'+r.rssi+'</td><td>'+(r.open?'Y':'N')+'</td><td>'+buildJoin(r)+'</td>';body.appendChild(tr);});if(autoScan){setTimeout(startScan,2000);}}});}function buildJoin(r){if(r.open){return '<button onclick=joinOpen(\''+r.ssid+'\')>Join</button>';}return '<form onsubmit=joinSec(event,\''+r.ssid+'\')><input type=password placeholder=Pass id=p_'+encodeURIComponent(r.ssid)+'><button>Join</button></form>';}function joinOpen(s){fetch('/join',{method:'POST',headers:{\"Content-Type\":'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)}).then(r=>r.text()).then(t=>{document.getElementById('act').textContent=t;});}function joinSec(ev,s){ev.preventDefault();var pw=document.getElementById('p_'+encodeURIComponent(s)).value;fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(pw)}).then(r=>r.text()).then(t=>{document.getElementById('act').textContent=t;});}function toggleWifi(){fetch('/api/wifiEnabled',{method:'POST'}).then(r=>r.text()).then(t=>{document.getElementById('act').textContent=t});}function toggleApAlways(){fetch('/api/apAlways',{method:'POST'}).then(r=>r.text()).then(t=>{document.getElementById('act').textContent=t});}function chAp(ev){ev.preventDefault();var v=document.getElementById('apSsidIn').value.trim();if(!v){document.getElementById('act').textContent='SSID empty';return false;}fetch('/api/apSsid',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(v)}).then(r=>r.text()).then(t=>{document.getElementById('act').textContent=t;});return false;}startScan();</script></body></html>");
      req->send(200,F("text/html"),h);
    });
    // API endpoints for toggles
    server.on("/api/wifiEnabled", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      wifiEnabledFlag = !wifiEnabledFlag; if (wifiEnableSetter) wifiEnableSetter(wifiEnabledFlag);
      req->send(200,F("text/plain"), wifiEnabledFlag?F("wifiEnabled=1"):F("wifiEnabled=0"));
    });
    server.on("/api/apAlways", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      apAlwaysFlag = !apAlwaysFlag; if (apAlwaysSetter) apAlwaysSetter(apAlwaysFlag);
      req->send(200,F("text/plain"), apAlwaysFlag?F("apAlwaysOn=1"):F("apAlwaysOn=0"));
    });
    server.on("/api/apSsid", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if(!req->hasParam("ssid", true)){ req->send(400,F("text/plain"),F("missing ssid")); return; }
      String ns = req->getParam("ssid", true)->value(); ns.trim();
      if(ns.length()<1 || ns.length()>31){ req->send(400,F("text/plain"),F("len 1-31")); return; }
      ssid = ns; // update stored
      // Restart / (re)enable AP with new SSID if active or suppressed (bring it up)
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_AP_STA); // keep STA if joined
      if(!WiFi.softAP(ssid.c_str(), pass.length()==0? nullptr: pass.c_str())) {
        req->send(500,F("text/plain"),F("ap restart fail")); return; }
      apIP = WiFi.softAPIP();
      apSuppressedAfterSta = false;
      req->send(200,F("text/plain"),String("apSsid=")+ssid.c_str());
    });
    server.on("/control", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      // Extract params and stash into pending lambda (statusFn can't mutate; external code should provide setter via callback pattern in future)
      if (!(req->hasParam("off", true) && req->hasParam("on", true))) { req->send(400, F("text/plain"), F("Missing off/on")); return; }
      String offStr = req->getParam("off", true)->value();
      String onStr  = req->getParam("on", true)->value();
      uint32_t offVal = offStr.toInt();
      uint32_t onVal  = onStr.toInt();
      String err;
      bool ok=false;
      if (timerUpdateFn) ok = timerUpdateFn(offVal, onVal, err); else err = F("No handler");
      if (ok) {
        req->send(200, F("text/plain"), F("OK"));
      } else {
        req->send(400, F("text/plain"), err.length()?err:F("Update failed"));
      }
    });

    // Join WiFi (Station) page (basic form; actual connection logic should be integrated externally)
    server.on("/join", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if (req->hasParam("scan")) { beginScan(); }
  String h = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Join</title><style>body{font-family:sans-serif;margin:12px;}table{border-collapse:collapse;margin-top:6px;}td,th{border:1px solid #ccc;padding:4px 6px;font-size:.72em;}#log{font-size:.7em;color:#555;margin-top:6px;}button{padding:4px 8px;}input[type=password]{width:90px;}label{font-size:.7em;}#scanStatus{font-size:.7em;}</style></head><body><h2>Join Network</h2><div><button onclick=startScan()>Scan</button><label style='margin-left:8px'>Auto <input type='checkbox' id='autoScanCk' onchange=autoScanToggle()></label></div><div id='scanStatus'>Idle</div><table id='scanTable'><tbody></tbody></table><h3>Connection</h3><p>State: <code id='cstate'></code> <span id='ip'></span> <span id='crssi'></span></p><div id='log'></div><p><a href='/'>&larr; Home</a></p><script>var es=new EventSource('/events');function S(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}es.addEventListener('status',function(ev){try{var o=JSON.parse(ev.data);S('crssi',(o.staRssi!==undefined)?('RSSI '+o.staRssi+' dBm'):'');if(o.staConnected){S('cstate','CONNECTED');S('ip','IP '+(o.staIp||''));}else{S('cstate',o.staStatus);} }catch(e){}});var autoScan=false;var scanTimer=null;function startScan(){fetch('/scan?start=1').then(()=>{document.getElementById('scanStatus').textContent='Scanning...';if(scanTimer)clearInterval(scanTimer);pollScan();scanTimer=setInterval(pollScan,1100);});}function autoScanToggle(){autoScan=document.getElementById('autoScanCk').checked;if(autoScan){startScan();}else if(scanTimer){clearInterval(scanTimer);scanTimer=null;}}function pollScan(){fetch('/scan').then(r=>r.json()).then(j=>{if(j.status==='scanning'){document.getElementById('scanStatus').textContent='Scanning...';return;}if(j.status==='idle'){document.getElementById('scanStatus').textContent='Idle';if(autoScan){startScan();}return;}if(j.status==='done'){document.getElementById('scanStatus').textContent='Done';if(!autoScan && scanTimer){clearInterval(scanTimer);scanTimer=null;}var body=document.querySelector('#scanTable tbody');body.innerHTML='<tr><th>SSID</th><th>RSSI</th><th>Open</th><th>Join</th></tr>';j.results.forEach(function(r){var tr=document.createElement('tr');tr.innerHTML='<td>'+r.ssid+'</td><td>'+r.rssi+'</td><td>'+(r.open?'Y':'N')+'</td><td>'+buildJoin(r)+'</td>';body.appendChild(tr);});if(autoScan){setTimeout(startScan,2000);}}});}function buildJoin(r){if(r.open){return '<button onclick=joinOpen(\''+r.ssid+'\')>Join</button>';}return '<form onsubmit=joinSec(event,\''+r.ssid+'\')><input type=password id=p_'+encodeURIComponent(r.ssid)+' placeholder=Pass><button>Join</button></form>';}function joinOpen(s){fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)}).then(r=>r.text()).then(t=>{document.getElementById('log').textContent=t;});}function joinSec(ev,s){ev.preventDefault();var pw=document.getElementById('p_'+encodeURIComponent(s)).value;fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(pw)}).then(r=>r.text()).then(t=>{document.getElementById('log').textContent=t;});}startScan();</script></body></html>");
      req->send(200,F("text/html"),h);
    });
    server.on("/join", HTTP_POST, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if (!req->hasParam("ssid", true)) { req->send(400, F("text/plain"), F("Missing ssid")); return; }
      String ssid = req->getParam("ssid", true)->value();
      String pass = req->hasParam("pass", true)? req->getParam("pass", true)->value(): String();
      if (!beginJoin(ssid, pass)) { req->send(400, F("text/plain"), F("Join start failed")); return; }
      req->send(200, F("text/plain"), F("Connecting... refresh /join"));
    });

    // Network scan JSON
    server.on("/scan", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if (req->hasParam("start")) { beginScan(); }
      int c = scanResultCount();
      if (staState == StaState::SCANNING) { if(Serial) Serial.println(F("[SCAN] Still scanning")); req->send(200, F("application/json"), F("{\"status\":\"scanning\"}")); return; }
      if (c < 0) { if(Serial) Serial.println(F("[SCAN] Idle (no results)")); req->send(200, F("application/json"), F("{\"status\":\"idle\"}")); return; }
      if (Serial) { Serial.print(F("[SCAN] Done, networks=")); Serial.println(c); }
      String out = F("{\"status\":\"done\",\"results\":[");
      for(int i=0;i<c;i++) {
        if (i) out += ',';
        out += '{';
        out += F("\"ssid\":\""); out += WiFi.SSID(i); out += F("\",");
        out += F("\"rssi\":"); out += WiFi.RSSI(i); out += ',';
        out += F("\"open\":"); out += (WiFi.encryptionType(i)==WIFI_AUTH_OPEN?1:0);
        out += '}';
      }
      out += ']';
      if (staState == StaState::CONNECTED) { out += F(",\"staIp\":\""); out += getStaIp().toString(); out += '"'; }
      out += '}';
      req->send(200, F("application/json"), out);
    });

    // Timers JSON API
    server.on("/api/timers", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      if (!statusFn) { req->send(500, F("application/json"), F("{\"error\":\"no status\"}")); return; }
      String json; statusFn(json);
      req->send(200, F("application/json"), json);
    });

    // Lightweight health metrics (auth optional? keep protected for consistency)
    server.on("/health", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::Control)) return;
      String out;
      out.reserve(160);
      out += '{';
      out += F("\"uptimeMs\":"); out += millis();
      out += F(",\"freeHeap\":"); out += ESP.getFreeHeap();
      out += F(",\"apActive\":"); out += (isApActive()?1:0);
      out += F(",\"apSuppressed\":"); out += (isApSuppressed()?1:0);
      extern unsigned long loopsPerSec; extern unsigned long remoteUpdateCount; // from main.cpp
      out += F(",\"loopsPerSec\":"); out += loopsPerSec;
      out += F(",\"remoteUpdates\":"); out += remoteUpdateCount;
      out += F(",\"staState\":\"");
      switch(staState){
        case StaState::IDLE: out += F("IDLE"); break; case StaState::SCANNING: out += F("SCANNING"); break; case StaState::CONNECTING: out += F("CONNECTING"); break; case StaState::CONNECTED: out += F("CONNECTED"); break; case StaState::FAILED: out += F("FAILED"); break;
      }
      out += F("\"");
      if (staState==StaState::CONNECTED) { out += F(",\"staRssi\":"); out += WiFi.RSSI(); }
      out += '}';
      req->send(200, F("application/json"), out);
    });

    // Captive portal detection endpoints (return simple 200 or 204 as expected)
    server.on("/generate_204", HTTP_GET, [this](AsyncWebServerRequest *r){ r->send(204); }); // Android
    server.on("/gen_204", HTTP_GET, [this](AsyncWebServerRequest *r){ r->send(204); }); // Some variants
    server.on("/hotspot-detect.html", HTTP_GET, [this](AsyncWebServerRequest *r){ r->send(200, F("text/html"), F("<html><head><title>Success</title></head><body>OK</body></html>")); }); // Apple
    server.on("/ncsi.txt", HTTP_GET, [this](AsyncWebServerRequest *r){ r->send(200, F("text/plain"), F("Microsoft NCSI")); }); // Windows
    server.on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest *r){ r->send(200, F("text/plain"), F("OK")); });

    // OTA GET form
    server.on("/update", HTTP_GET, [this](AsyncWebServerRequest *req){
      if (requireAuthKind(req, AuthKind::OTA)) return;
      String h = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>OTA</title><style>body{font-family:sans-serif;margin:14px;}input[type=file]{margin:10px 0;}button{padding:6px 10px;}</style></head><body><h2>Firmware Update</h2><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware'><br><button>Upload</button></form><p><a href='/'>&larr; Home</a></p></body></html>");
      req->send(200,F("text/html"),h);
    });
    // OTA upload POST
    server.on("/update", HTTP_POST,
      [this](AsyncWebServerRequest *request){
        if (requireAuthKind(request, AuthKind::OTA)) return;
        bool ok = !Update.hasError();
        request->send(ok?200:500, F("text/plain"), ok?F("OK - Rebooting") : F("Update Failed"));
        if (ok) { delay(500); ESP.restart(); }
      },
      [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if (requireAuthKind(request, AuthKind::OTA)) return; // will have sent 401 for first chunk
        if (!index) {
          if (Update.isRunning()) Update.end();
          // Assume size unknown; use max
          if(!Update.begin()) {
            Update.printError(Serial);
          }
        }
        if (Update.write(data, len) != len) {
          Update.printError(Serial);
        }
        if (final) {
          if(!Update.end(true)) {
            Update.printError(Serial);
          }
        }
      }
    );

    // Captive / fallback: serve portal for anything else (HTTP only)
    server.onNotFound([this](AsyncWebServerRequest *req){ req->redirect("/"); });
    server.begin();
  }
  static constexpr uint8_t DNS_PORT = 53;
  bool started=false;
  String ssid; String pass; IPAddress apIP;
  DNSServer dnsServer; AsyncWebServer server{80};
  AsyncEventSource eventSource{"/events"};
  unsigned long lastSsePushMs = 0; static constexpr unsigned long sseIntervalMs = 1000;
  const char* authUser=nullptr; const char* authPass=nullptr; String cachedCreds;
  const char* ctrlUser=nullptr; const char* ctrlPass=nullptr; String cachedCtrlCreds;
  const char* otaUser=nullptr; const char* otaPass=nullptr; String cachedOtaCreds;
  JsonStatusFn statusFn;
  TimerUpdateFn timerUpdateFn;
  RelayToggleFn relayToggleFn = nullptr;
  // STA management
  StaState staState = StaState::IDLE;
  unsigned long staLastChange = 0;
  unsigned long staConnectStart = 0;
  static constexpr unsigned long staConnectTimeoutMs = 15000; // 15s
  IPAddress staIp{0,0,0,0};
  String pendingSsid; String pendingPass;
  void ensureApStaMode() {
    wifi_mode_t m = WiFi.getMode();
    if (m != WIFI_MODE_APSTA) {
      WiFi.mode(WIFI_AP_STA); // keep AP running while scanning/connecting
    }
  }
  bool apSuppressedAfterSta = false;
  unsigned long lastAuthSuccessMs = 0;
  uint8_t apClientCount = 0;
  // Mirror of config flags (updated by main via setters); used for API toggles (public for simplicity)
public:
  bool wifiEnabledFlag = false;
  bool apAlwaysFlag = false;
private:
  BoolSetterFn wifiEnableSetter = nullptr;
  BoolSetterFn apAlwaysSetter = nullptr;
public:
  // SSE support
  AsyncEventSource& events() { return eventSource; }
  bool maybeDisableApOnSta(unsigned long stableMs=5000) {
    if (staState == StaState::CONNECTED && !apSuppressedAfterSta) {
      if (millis() - staLastChange > stableMs) {
        WiFi.softAPdisconnect(true);
        apSuppressedAfterSta = true;
        return true;
      }
    }
    return false;
  }
  void ensureApIfSuppressed() {
    if (apSuppressedAfterSta && (staState != StaState::CONNECTED)) {
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(ssid.c_str(), pass.length()==0? nullptr: pass.c_str());
      apIP = WiFi.softAPIP();
      apSuppressedAfterSta = false;
    }
  }
  bool isApSuppressed() const { return apSuppressedAfterSta; }
  bool isApActive() const { return started && !apSuppressedAfterSta; }
  unsigned long lastAuthMs() const { return lastAuthSuccessMs; }
  uint8_t apClients() const { return apClientCount; }
  bool wifiEnabledMirror() const { return wifiEnabledFlag; }
  bool apAlwaysMirror() const { return apAlwaysFlag; }
};
