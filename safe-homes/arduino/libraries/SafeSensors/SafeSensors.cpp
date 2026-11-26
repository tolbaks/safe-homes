#include "SafeSensors.h"
#include <Wire.h>
#include <EEPROM.h>

Sensor::Sensor(uint8_t powerPin, unsigned long warmupMs)
    : _powerPin(powerPin), _warmupMs(warmupMs), _onAt(0), _powered(false) {}

void Sensor::begin() {
  if (_powerPin != NO_PIN) {
    pinMode(_powerPin, OUTPUT);
    digitalWrite(_powerPin, LOW);
  }
  _powered = false;
}

void Sensor::powerOn() {
  if (_powered) return;
  if (_powerPin != NO_PIN) digitalWrite(_powerPin, HIGH);
  _onAt = millis();
  _powered = true;
}

void Sensor::powerOff() {
  if (_powerPin == NO_PIN) return;
  digitalWrite(_powerPin, LOW);
  _powered = false;
}

bool Sensor::isReady() const {
  return _powered && (millis() - _onAt >= _warmupMs);
}

UltrasonicSensor::UltrasonicSensor(uint8_t trigPin, uint8_t echoPin, uint8_t powerPin)
    : Sensor(powerPin, 100UL), _trig(trigPin), _echo(echoPin), _last(0), _cm(-1) {}

void UltrasonicSensor::begin() {
  Sensor::begin();
  pinMode(_trig, OUTPUT);
  digitalWrite(_trig, LOW);
  pinMode(_echo, INPUT);
}

void UltrasonicSensor::update() {
  if (!isReady() || millis() - _last < 250UL) return;
  _last = millis();
  digitalWrite(_trig, LOW);
  delayMicroseconds(2);
  digitalWrite(_trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(_trig, LOW);
  unsigned long us = pulseIn(_echo, HIGH, 25000UL);
  _cm = (us == 0) ? -1 : us * 0.0343f / 2.0f;
}

Dht22Sensor::Dht22Sensor(uint8_t dataPin, uint8_t powerPin)
    : Sensor(powerPin, 2000UL), _dht(dataPin, DHT22), _last(0), _t(0), _h(0), _ok(false) {}

void Dht22Sensor::begin() {
  Sensor::begin();
  _dht.begin();
}

void Dht22Sensor::powerOff() {
  Sensor::powerOff();
  _ok = false;
}

void Dht22Sensor::update() {
  if (!isReady() || millis() - _last < 2000UL) return;
  _last = millis();
  float h = _dht.readHumidity();
  float t = _dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    _ok = false;
    return;
  }
  _h = h;
  _t = t;
  _ok = true;
}

SmokeSensor::SmokeSensor(uint8_t analogPin, uint8_t powerPin)
    : Sensor(powerPin, 20000UL), _pin(analogPin) {}

void SmokeSensor::begin() {
  Sensor::begin();
}

int SmokeSensor::raw() {
  return analogRead(_pin);
}

SensorHub::SensorHub(uint8_t trigPin, uint8_t echoPin, uint8_t usPwr,
                     uint8_t dhtPin, uint8_t dhtPwr,
                     uint8_t smokePin, uint8_t smokePwr)
    : _us(trigPin, echoPin, usPwr), _dht(dhtPin, dhtPwr), _smoke(smokePin, smokePwr) {}

void SensorHub::begin() {
  _us.begin();
  _dht.begin();
  _smoke.begin();
}

void SensorHub::powerOnAll() {
  _us.powerOn();
  _dht.powerOn();
  _smoke.powerOn();
}

void SensorHub::powerOffAll() {
  _us.powerOff();
  _dht.powerOff();
  _smoke.powerOff();
}

void SensorHub::update() {
  _dht.update();
  _us.update();
}

bool SensorHub::ready() const {
  return _us.isReady() && _dht.isReady() && _dht.valid() && _smoke.isReady();
}

SensorHub::Reading SensorHub::read() {
  Reading r;
  r.distance = _us.distanceCm();
  r.temp = _dht.temperature();
  r.hum = _dht.humidity();
  r.gas = _smoke.raw();
  return r;
}

Outputs::Outputs(uint8_t buzzerPin, uint8_t fanPin, uint8_t ledPin)
    : _buzzerPin(buzzerPin), _fanPin(fanPin), _ledPin(ledPin),
      _buzzer(false), _fan(false), _led(false), _flashEnd(0) {}

void Outputs::begin() {
  pinMode(_buzzerPin, OUTPUT);
  pinMode(_fanPin, OUTPUT);
  pinMode(_ledPin, OUTPUT);
  setBuzzer(false);
  setFan(false);
  setLed(false);
}

void Outputs::setBuzzer(bool on) { _buzzer = on; digitalWrite(_buzzerPin, on ? HIGH : LOW); }
void Outputs::setFan(bool on)    { _fan = on;    digitalWrite(_fanPin, on ? HIGH : LOW); }
void Outputs::setLed(bool on)    { _led = on;    digitalWrite(_ledPin, on ? HIGH : LOW); }

void Outputs::flashLed(unsigned long ms) {
  digitalWrite(_ledPin, _led ? LOW : HIGH);
  _flashEnd = millis() + ms;
  if (_flashEnd == 0) _flashEnd = 1;
}

void Outputs::update() {
  if (_flashEnd && (long)(millis() - _flashEnd) >= 0) {
    _flashEnd = 0;
    digitalWrite(_ledPin, _led ? HIGH : LOW);
  }
}

static bool sameBuildStored(const __FlashStringHelper* d, const __FlashStringHelper* t) {
  const char* pd = (const char*)d;
  const char* pt = (const char*)t;
  uint8_t a = 0;
  for (uint8_t i = 0; i < 11; i++) if (EEPROM.read(a++) != pgm_read_byte(pd + i)) return false;
  for (uint8_t i = 0; i < 8; i++)  if (EEPROM.read(a++) != pgm_read_byte(pt + i)) return false;
  return true;
}

static void rememberBuild(const __FlashStringHelper* d, const __FlashStringHelper* t) {
  const char* pd = (const char*)d;
  const char* pt = (const char*)t;
  uint8_t a = 0;
  for (uint8_t i = 0; i < 11; i++) EEPROM.update(a++, pgm_read_byte(pd + i));
  for (uint8_t i = 0; i < 8; i++)  EEPROM.update(a++, pgm_read_byte(pt + i));
}

SafeClock::SafeClock() : _ok(false), _tz(0) {}

bool SafeClock::begin(const __FlashStringHelper* buildDate, const __FlashStringHelper* buildTime, long tzOffsetSec) {
  _tz = tzOffsetSec;
  Wire.begin();
  if (!_rtc.begin()) return false;
  _ok = true;
  if (_rtc.lostPower() || !sameBuildStored(buildDate, buildTime)) {
    _rtc.adjust(DateTime(buildDate, buildTime));
    rememberBuild(buildDate, buildTime);
  }
  return true;
}

unsigned long SafeClock::unixUtc() {
  if (!_ok) return 0;
  return _rtc.now().unixtime() - (unsigned long)_tz;
}

void SafeClock::setUtc(unsigned long utc) {
  if (!_ok || utc < 1700000000UL) return;
  _rtc.adjust(DateTime((uint32_t)(utc + (unsigned long)_tz)));
}
