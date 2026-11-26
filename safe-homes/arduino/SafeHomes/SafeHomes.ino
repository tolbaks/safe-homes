/*
  Safe Homes - Arduino Pro Mini main program
  Board: Arduino Pro Mini 5V 16 MHz, pins follow the schematic

  Modes: SLEEP, AUTO, MAINTENANCE (enum + switch)
  Libraries:
    SafeSensors  sensors (ultrasonic, DHT22, smoke), outputs, RTC
    EspLink      link to the ESP-01 and the SD queue
  Install from the Library Manager:
    DHT sensor library, Adafruit Unified Sensor, RTClib

  Logging: every log row goes to the SD card first and is deleted
  only after the server confirms it. Log interval: 3 minutes.
  Interrupt: D2 (INT0) is the ESP TX line, the ESP pulls it low to wake the Arduino
  (see wakeISR() and enterSleep()).
  Pins, intervals and alarm limits are set below.
  No delay() is used, all timing uses millis().
*/
#include <avr/sleep.h>
#include <Wire.h>
#include <EEPROM.h>
#include <SD.h>
#include <RTClib.h>
#include <SafeSensors.h>
#include <EspLink.h>

// ---- pins
const uint8_t PIN_ESP_RX = 2;
const uint8_t PIN_BUZZER = 3;
const uint8_t PIN_FAN    = 4;
const uint8_t PIN_DHT    = 5;
const uint8_t PIN_TRIG   = 6;
const uint8_t PIN_ECHO   = 7;
const uint8_t PIN_LED    = 8;
const uint8_t PIN_ESP_TX = 9;
const uint8_t PIN_SD_CS  = 10;
const uint8_t PIN_SMOKE  = A0;
// A4 = SDA, A5 = SCL (RTC)

// sensor power pins: not used,
// the sensors are wired straight to 5V (no power switch on the PCB)
// to use them: wire a sensor to A1-A3 and set the pin here
const uint8_t PWR_ULTRASONIC = NO_PIN;
const uint8_t PWR_DHT        = NO_PIN;
const uint8_t PWR_SMOKE      = NO_PIN;

// unused pins, set LOW so they do not float
const uint8_t UNUSED_PINS[] = {A1, A2, A3};

// ---- settings
const unsigned long LIVE_INTERVAL_MS = 1000UL;
const unsigned long LOG_INTERVAL_MS  = 180000UL;
const long TZ_OFFSET_SEC = 8L * 3600L;              // RTC uses PH time, the server wants UTC

// alarm limits
const float THRESH_TEMP = 32.0;
const float THRESH_HUM  = 90.0;
const int   THRESH_GAS  = 400;
const float MOTION_CM   = 50.0;

// ---- objects
enum Mode { MODE_SLEEP, MODE_AUTO, MODE_MAINTENANCE };
Mode mode = MODE_AUTO;

SensorHub hub(PIN_TRIG, PIN_ECHO, PWR_ULTRASONIC, PIN_DHT, PWR_DHT, PIN_SMOKE, PWR_SMOKE);
EspLink link(PIN_ESP_RX, PIN_ESP_TX);
Outputs outputs(PIN_BUZZER, PIN_FAN, PIN_LED);
SafeClock rtcClock;
SdQueue sdq;

unsigned long lastLive = 0, lastLog = 0;
const char* wakeLabel = NULL;
bool lastMotion = false;
volatile bool woke = false;

// ---- interrupt
void wakeISR() {
  detachInterrupt(digitalPinToInterrupt(PIN_ESP_RX));
  woke = true;
}

// ---- helpers
const char* modeName() {
  switch (mode) {
    case MODE_SLEEP:       return "SLEEP";
    case MODE_AUTO:        return "AUTO";
    case MODE_MAINTENANCE: return "MAINTENANCE";
  }
  return "AUTO";
}

bool isMotion(const SensorHub::Reading& r) {
  return r.distance > 0 && r.distance < MOTION_CM;
}

float distCm(const SensorHub::Reading& r) {
  return (r.distance > 0 && r.distance < 400.0f) ? r.distance : 400.0f;
}

void sendData(char kind, const SensorHub::Reading& r) {
  link.sendData(kind, modeName(), distCm(r), r.temp, r.hum, r.gas,
                outputs.buzzer(), outputs.fan(), outputs.led(), rtcClock.unixUtc());
}

void resetTimers() {
  lastLive = millis() - LIVE_INTERVAL_MS;
  lastLog  = millis() - LOG_INTERVAL_MS;
}

void setMode(Mode m) {
  mode = m;
  outputs.setBuzzer(false);
  outputs.setFan(false);
  outputs.setLed(false);
  resetTimers();
  if (hub.ready()) sendData('L', hub.read());
}

// ---- log rows
void logRow(const SensorHub::Reading& r, const char* label) {
  char row[64], d[10], t[10], h[10];
  unsigned long ts = rtcClock.unixUtc();
  dtostrf(distCm(r), 1, 1, d);
  dtostrf(r.temp, 1, 1, t);
  dtostrf(r.hum, 1, 1, h);
  snprintf(row, sizeof(row), "R,%s,%s,%s,%s,%d,%d,%d,%d,%lu", label, d, t, h, r.gas,
           outputs.buzzer() ? 1 : 0, outputs.fan() ? 1 : 0, outputs.led() ? 1 : 0, ts);
  if (ts != 0 && sdq.add(row)) return;
  link.sendLine(row);
}

// ---- commands from the ESP
void handleCommand(const EspCommand& cmd) {
  switch (cmd.type) {
    case CMD_AUTO:        setMode(MODE_AUTO);        break;
    case CMD_MAINTENANCE: setMode(MODE_MAINTENANCE); break;
    case CMD_SLEEP:       mode = MODE_SLEEP;         break;
    case CMD_READ:
      if (hub.ready()) sendData('L', hub.read());
      break;
    case CMD_SET:
      if (mode != MODE_MAINTENANCE) break;
      if (!strcmp(cmd.target, "buzzer")) outputs.setBuzzer(cmd.value != 0);
      else if (!strcmp(cmd.target, "fan")) outputs.setFan(cmd.value != 0);
      else if (!strcmp(cmd.target, "led")) outputs.setLed(cmd.value != 0);
      if (hub.ready()) sendData('L', hub.read());
      break;
    case CMD_SAVED:       sdq.onSaved((unsigned long)cmd.value); break;
    case CMD_TIME:        rtcClock.setUtc((unsigned long)cmd.value); break;
    default: break;
  }
  if (cmd.type != CMD_SAVED && cmd.type != CMD_TIME && cmd.type != CMD_NONE)
    outputs.flashLed(150UL);
}

// ---- modes
void runAuto() {
  if (!hub.ready()) return;
  SensorHub::Reading r = hub.read();

  bool smoke = r.gas >= THRESH_GAS;
  bool hot   = r.temp >= THRESH_TEMP;
  bool humid = r.hum >= THRESH_HUM;
  bool beep  = isMotion(r) && ((millis() / 200UL) % 2UL == 0UL);
  bool alarm = hot || humid || beep;
  outputs.setBuzzer(alarm);
  outputs.setLed(alarm);
  outputs.setFan(smoke || hot);

  unsigned long now = millis();
  bool m = isMotion(r);
  // send live data once a second, or when motion changes (max every 0.5 s)
  // the Pro Mini cannot receive while it sends,
  // so it must not send too often
  if (now - lastLive >= LIVE_INTERVAL_MS || (m != lastMotion && now - lastLive >= 500UL)) {
    lastLive = now;
    lastMotion = m;
    sendData('L', r);
  }
  if (now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;
    logRow(r, wakeLabel ? wakeLabel : modeName());
    wakeLabel = NULL;
  }
}

void runMaintenance() {
  // no alarms here, the website controls the outputs
  // still logs every 3 minutes as MAINTENANCE
  if (!hub.ready()) return;
  unsigned long now = millis();
  if (now - lastLive >= LIVE_INTERVAL_MS) {
    lastLive = now;
    sendData('L', hub.read());
  }
  if (now - lastLog >= LOG_INTERVAL_MS) {
    lastLog = now;
    logRow(hub.read(), modeName());
  }
}

void enterSleep() {
  if (hub.ready()) logRow(hub.read(), "SLEEP");
  link.sendEvent('S', rtcClock.unixUtc());
  link.end();
  hub.powerOffAll();
  outputs.setBuzzer(false);
  outputs.setFan(false);
  outputs.setLed(false);
  woke = false;

  pinMode(PIN_ESP_RX, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ESP_RX), wakeISR, LOW);  // only a LOW level wakes power-down
  ADCSRA &= ~(1 << ADEN);

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  noInterrupts();
  if (!woke) {
    sleep_enable();
    sleep_bod_disable();                            // BOD off, right before sleep
    interrupts();
    sleep_cpu();
    sleep_disable();
  }
  interrupts();

  ADCSRA |= (1 << ADEN);
  link.begin();
  sdq.reset();
  hub.powerOnAll();
  setMode(MODE_AUTO);
  wakeLabel = "WAKE";
  link.sendEvent('W', rtcClock.unixUtc());
}

// ---- setup / loop
void setup() {
  for (uint8_t i = 0; i < sizeof(UNUSED_PINS); i++) {
    pinMode(UNUSED_PINS[i], OUTPUT);
    digitalWrite(UNUSED_PINS[i], LOW);
  }
  pinMode(0, INPUT_PULLUP);
  pinMode(1, INPUT_PULLUP);

  outputs.begin();
  rtcClock.begin(F(__DATE__), F(__TIME__), TZ_OFFSET_SEC);  // set RTC from the compile time (once per upload)
  sdq.begin(PIN_SD_CS);

  hub.begin();
  hub.powerOnAll();
  link.begin();
  resetTimers();
}

void loop() {
  EspCommand cmd;
  if (link.poll(cmd)) handleCommand(cmd);
  outputs.update();
  hub.update();

  switch (mode) {
    case MODE_AUTO:        runAuto();        break;
    case MODE_MAINTENANCE: runMaintenance(); break;
    case MODE_SLEEP:       enterSleep();     break;
  }
  sdq.update(link);
}
