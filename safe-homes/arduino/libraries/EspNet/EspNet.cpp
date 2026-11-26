#include "EspNet.h"

WifiSetup::WifiSetup(const char* apName, const char* apPass)
    : _web(80), _apName(apName), _apPass(apPass), _apOn(false), _restartPending(false),
      _lostSince(0), _apSince(0), _restartAt(0) {}

void WifiSetup::begin() {
  EEPROM.begin(sizeof(WifiSettings));
  load();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (_cfg.ssid[0]) WiFi.begin(_cfg.ssid, _cfg.pass);
  _lostSince = millis();
  _web.on("/", HTTP_GET, [this]() { handleRoot(); });
  _web.on("/save", HTTP_POST, [this]() { handleSave(); });
  _web.begin();
}

void WifiSetup::update() {
  _web.handleClient();
  manage();
  if (_restartPending && (long)(millis() - _restartAt) >= 0) ESP.restart();
}

bool WifiSetup::connected() const {
  return WiFi.status() == WL_CONNECTED;
}

void WifiSetup::load() {
  EEPROM.get(0, _cfg);
  _cfg.magic[3] = 0;
  if (strcmp(_cfg.magic, "SH1") != 0) {
    memset(&_cfg, 0, sizeof(_cfg));
    strcpy(_cfg.magic, "SH1");
    strcpy(_cfg.server, "http://192.168.x.x:5000");
    strcpy(_cfg.key, "CHANGE_ME_DEVICE_KEY");
  }
  _cfg.ssid[32] = 0; _cfg.pass[64] = 0; _cfg.server[39] = 0; _cfg.key[39] = 0;
}

void WifiSetup::save() {
  strcpy(_cfg.magic, "SH1");
  EEPROM.put(0, _cfg);
  EEPROM.commit();
}

void WifiSetup::startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(_apName, _apPass);
  if (_cfg.ssid[0]) WiFi.begin(_cfg.ssid, _cfg.pass);
  _apOn = true;
  _apSince = millis();
}

void WifiSetup::stopAP() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  _apOn = false;
}

void WifiSetup::manage() {
  if (connected()) {
    _lostSince = 0;

    if (_apOn && WiFi.softAPgetStationNum() == 0 && millis() - _apSince > 120000UL) stopAP();
  } else {
    if (_lostSince == 0) _lostSince = millis();
    if (!_apOn && (!_cfg.ssid[0] || millis() - _lostSince > 20000UL)) startAP();
  }
}

String WifiSetup::esc(const String& s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

bool WifiSetup::onSetupNetwork() {
  return _web.client().localIP() == WiFi.softAPIP();
}

String WifiSetup::page(const String& msg) {
  bool onAp = onSetupNetwork();
  String h = F("<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Safe Homes setup</title><style>body{font-family:sans-serif;max-width:420px;margin:20px auto;padding:0 12px}"
               "input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}button{padding:10px 18px}</style></head><body>"
               "<h2>Safe Homes setup</h2>");
  if (msg.length()) h += "<p><b>" + msg + "</b></p>";
  if (connected()) h += "<p>WiFi: connected, IP " + WiFi.localIP().toString() + "</p>";
  else h += "<p>WiFi: not connected</p>";
  h += F("<form method=post action=/save>");
  h += "WiFi name<input name=ssid value=\"" + String(onAp ? esc(_cfg.ssid) : String("")) + "\">";
  h += F("WiFi password<input name=pass type=password>");
  h += "Server address<input name=server value=\"" + String(onAp ? esc(_cfg.server) : String("")) + "\" placeholder=\"http://192.168.x.x:5000\">";
  h += F("Device key (empty = keep the current one)<input name=key type=password>");
  if (!onAp) h += F("Current device key (required when not on the setup network)<input name=current type=password>");
  h += F("<button type=submit>Save and restart</button></form>");
  h += F("<p>Leave the password empty only for an open network.</p></body></html>");
  return h;
}

void WifiSetup::handleRoot() {
  _web.send(200, "text/html", page(""));
}

void WifiSetup::handleSave() {
  if (!onSetupNetwork() && !(_cfg.key[0] && _web.arg("current") == _cfg.key)) {
    _web.send(403, "text/html", page("Wrong current key. Nothing was changed."));
    return;
  }
  String s = _web.arg("ssid"), p = _web.arg("pass"), u = _web.arg("server"), k = _web.arg("key");
  if (s.length() == 0 || s.length() > 32 || p.length() > 64 || u.length() < 8 || u.length() > 39 || k.length() > 39) {
    _web.send(400, "text/html", page("Check the values: WiFi name and server address are required."));
    return;
  }
  strncpy(_cfg.ssid, s.c_str(), sizeof(_cfg.ssid) - 1);
  strncpy(_cfg.pass, p.c_str(), sizeof(_cfg.pass) - 1);
  strncpy(_cfg.server, u.c_str(), sizeof(_cfg.server) - 1);
  if (k.length() > 0) strncpy(_cfg.key, k.c_str(), sizeof(_cfg.key) - 1);
  save();
  _web.send(200, "text/html", "<html><body><h3>Saved. The ESP-01 is restarting...</h3></body></html>");
  _restartPending = true;
  _restartAt = millis() + 1500UL;
}

ServerLink::ServerLink(WifiSetup& wifi) : _wifi(wifi), _qCount(0) {}

bool ServerLink::finished(int code) {
  return code == 200 || (code >= 400 && code < 500);
}

int ServerLink::post(const String& path, const String& body) {
  if (!_wifi.connected()) return -1;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String(_wifi.server()) + path);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", _wifi.key());
  http.setTimeout(3000);
  int code = http.POST(body);
  http.end();
  return code;
}

void ServerLink::postEvent(const String& path, const String& body) {
  if (finished(post(path, body))) return;
  if (_qCount < QMAX) {
    _qPath[_qCount] = path;
    _qBody[_qCount] = body;
    _qCount++;
  }
}

void ServerLink::flushEvents() {
  while (_qCount > 0) {
    if (!finished(post(_qPath[0], _qBody[0]))) return;
    for (uint8_t i = 1; i < _qCount; i++) {
      _qPath[i - 1] = _qPath[i];
      _qBody[i - 1] = _qBody[i];
    }
    _qCount--;
  }
}

bool ServerLink::poll(String& body) {
  if (!_wifi.connected()) return false;
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String(_wifi.server()) + "/api/device/command");
  http.addHeader("X-API-Key", _wifi.key());
  http.setTimeout(3000);
  bool ok = (http.GET() == 200);
  if (ok) body = http.getString();
  http.end();
  return ok;
}
