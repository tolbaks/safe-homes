/*
  Safe Homes - ESP-01 main program

  Library: EspNet (WifiSetup and ServerLink)
  The ESP stays awake. It:
    - sends the Arduino readings and sleep/wake events to the server
    - sends K,<ts> back when the server saved a log row
    - asks the server for website commands and passes them to the Arduino
    - wakes the Arduino by pulling GPIO1 (TX, wired to D2) low for 150 ms
  Board: Generic ESP8266 Module
*/
#include <EspNet.h>

const char* AP_NAME = "SafeHomes-Setup";
const char* AP_PASS = "change-me-123";               // AP password
const unsigned long POLL_MS = 1000UL;

WifiSetup wifi(AP_NAME, AP_PASS);
ServerLink server(wifi);

bool sleeping = false, waking = false;
unsigned long lastPoll = 0, wakeAt = 0;
String line;

// a command can get lost on the wire (the Arduino cannot listen while it sends),
// so it is repeated until the Arduino confirms, 10 s at most
String wantLine = "";
String wantMode = "";
int wantField = -1, wantVal = 0;
unsigned long wantDeadline = 0, lastResend = 0;

// laptop time from the server, used to correct the Arduino RTC
unsigned long srvUnix = 0, srvAt = 0, lastTimeSend = 0;

// ---- Arduino -> server
String field(const String& s, uint8_t index) {
  int start = 0;
  for (uint8_t i = 0; i < index; i++) {
    start = s.indexOf(',', start);
    if (start < 0) return "";
    start++;
  }
  int end = s.indexOf(',', start);
  return end < 0 ? s.substring(start) : s.substring(start, end);
}

void handleLine(const String& s) {
  char kind = s.charAt(0);
  if (sleeping && kind == 'L') {
    sleeping = false;
    server.postEvent("/api/device/sleep", String("{\"event\":\"wake\",\"ts\":") + field(s, 9) + "}");
  }
  if (kind == 'L' || kind == 'R') {
    // kind,mode,motion,temp,hum,gas,alarm,fan,led,ts
    String body = "{\"mode\":\"" + field(s, 1) + "\",\"motion\":" + field(s, 2) +
                  ",\"temperature\":" + field(s, 3) + ",\"humidity\":" + field(s, 4) +
                  ",\"gas\":" + field(s, 5) + ",\"alarm_state\":" + field(s, 6) +
                  ",\"fan\":" + field(s, 7) + ",\"led\":" + field(s, 8) +
                  ",\"ts\":" + field(s, 9) + "}";
    if (kind == 'L') {
      if (wantLine.length()) {
        bool done = wantMode.length() ? (field(s, 1) == wantMode) : (field(s, wantField).toInt() == wantVal);
        if (done) wantLine = "";
      }
      unsigned long devTs = strtoul(field(s, 9).c_str(), NULL, 10);  // RTC time (UTC unix)
      if (srvUnix && devTs && !waking && !sleeping && millis() - srvAt < 10000UL &&
          millis() - lastTimeSend > 5000UL) {
        long expected = (long)srvUnix + (long)((millis() - srvAt) / 1000UL);
        long diff = (long)devTs - expected;
        if (diff > 3 || diff < -3) {
          lastTimeSend = millis();
          Serial.print("C,TIME,");
          Serial.print(expected);
          Serial.print('\n');
        }
      }
      server.post("/api/device/live", body);
    } else if (ServerLink::finished(server.post("/api/device/reading", body))) {
      Serial.print("K,");
      Serial.print(field(s, 9));
      Serial.print('\n');
    }
  } else if (kind == 'S' || kind == 'W') {
    sleeping = (kind == 'S');
    wantLine = "";
    String body = String("{\"event\":\"") + (kind == 'S' ? "sleep" : "wake") +
                  "\",\"ts\":" + field(s, 1) + "}";
    server.postEvent("/api/device/sleep", body);
  }
}

void readArduino() {
  if (waking) return;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (line.length() > 2) handleLine(line);
      line = "";
    } else if (line.length() < 80) {
      line += c;
    } else {
      line = "";
    }
  }
}

// ---- wake the Arduino
void startWake() {
  Serial.end();
  pinMode(1, FUNCTION_3);
  pinMode(1, OUTPUT);
  digitalWrite(1, LOW);
  wakeAt = millis();
  waking = true;
}

void runWake() {
  if (waking && millis() - wakeAt >= 150UL) {
    digitalWrite(1, HIGH);
    pinMode(1, FUNCTION_0);
    Serial.begin(9600);
    waking = false;
  }
}

// ---- server -> Arduino
String jsonVal(const String& s, const char* key) {
  int i = s.indexOf(String("\"") + key + "\"");
  if (i < 0) return "";
  i = s.indexOf(':', i);
  if (i < 0) return "";
  i++;
  while (i < (int)s.length() && (s[i] == ' ' || s[i] == '"')) i++;
  int j = i;
  while (j < (int)s.length() && s[j] != '"' && s[j] != ',' && s[j] != '}' && s[j] != ' ') j++;
  return s.substring(i, j);
}

void handleCommand(const String& body) {
  String t = jsonVal(body, "t");
  if (t.length()) { srvUnix = strtoul(t.c_str(), NULL, 10); srvAt = millis(); }
  String cmd = jsonVal(body, "cmd");
  if (cmd == "") return;
  if (cmd == "WAKE") {
    startWake();
    return;
  }
  if (sleeping) return;
  wantLine = "";
  if (cmd == "SET") {
    String target = jsonVal(body, "target"), value = jsonVal(body, "value");
    Serial.print("C,SET,");
    Serial.print(target);
    Serial.print(',');
    Serial.print(value);
    Serial.print('\n');
    wantLine = "C,SET," + target + "," + value;
    wantMode = "";
    wantField = target == "buzzer" ? 6 : target == "fan" ? 7 : 8;
    wantVal = value.toInt();
    wantDeadline = millis() + 10000UL;
    lastResend = millis();
  } else if (cmd == "READ") {
    Serial.print("C,READ,");
    Serial.print(jsonVal(body, "target"));
    Serial.print('\n');
  } else {
    Serial.print("C,");
    Serial.print(cmd);
    Serial.print('\n');
    if (cmd == "AUTO" || cmd == "MAINTENANCE" || cmd == "SLEEP") {
      wantLine = "C," + cmd;
      wantMode = cmd;
      wantDeadline = millis() + 10000UL;
      lastResend = millis();
    }
  }
}

void repeatWanted() {
  if (!wantLine.length() || waking || sleeping) return;
  if ((long)(millis() - wantDeadline) >= 0) { wantLine = ""; return; }
  if (millis() - lastResend >= 800UL) {
    lastResend = millis();
    Serial.print(wantLine);
    Serial.print('\n');
  }
}

void pollCommand() {
  if (waking) return;
  String body;
  if (server.poll(body)) handleCommand(body);
}

// ---- setup / loop
void setup() {
  Serial.begin(9600);
  wifi.begin();
}

void loop() {
  wifi.update();
  runWake();
  readArduino();
  repeatWanted();
  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    pollCommand();
    server.flushEvents();
  }
}
