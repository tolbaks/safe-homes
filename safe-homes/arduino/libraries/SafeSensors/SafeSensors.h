#ifndef SAFESENSORS_H
#define SAFESENSORS_H

#include <Arduino.h>
#include <DHT.h>
#include <RTClib.h>

const uint8_t NO_PIN = 255;

class Sensor {
 public:
  Sensor(uint8_t powerPin, unsigned long warmupMs);
  virtual ~Sensor() {}
  virtual void begin();
  void powerOn();
  virtual void powerOff();
  bool isReady() const;

 protected:
  uint8_t _powerPin;
  unsigned long _warmupMs;
  unsigned long _onAt;
  bool _powered;
};

class UltrasonicSensor : public Sensor {
 public:
  UltrasonicSensor(uint8_t trigPin, uint8_t echoPin, uint8_t powerPin);
  void begin();
  void update();
  float distanceCm() const { return _cm; }
 private:
  uint8_t _trig, _echo;
  unsigned long _last;
  float _cm;
};

class Dht22Sensor : public Sensor {
 public:
  Dht22Sensor(uint8_t dataPin, uint8_t powerPin);
  void begin();
  void powerOff();
  void update();
  float temperature() const { return _t; }
  float humidity() const { return _h; }
  bool valid() const { return _ok; }
 private:
  DHT _dht;
  unsigned long _last;
  float _t, _h;
  bool _ok;
};

class SmokeSensor : public Sensor {
 public:
  SmokeSensor(uint8_t analogPin, uint8_t powerPin);
  void begin();
  int raw();
 private:
  uint8_t _pin;
};

class SensorHub {
 public:
  struct Reading {
    float distance;
    float temp;
    float hum;
    int gas;
  };

  SensorHub(uint8_t trigPin, uint8_t echoPin, uint8_t usPwr,
            uint8_t dhtPin, uint8_t dhtPwr,
            uint8_t smokePin, uint8_t smokePwr);
  void begin();
  void powerOnAll();
  void powerOffAll();
  void update();
  bool ready() const;
  Reading read();

 private:
  UltrasonicSensor _us;
  Dht22Sensor _dht;
  SmokeSensor _smoke;
};

class Outputs {
 public:
  Outputs(uint8_t buzzerPin, uint8_t fanPin, uint8_t ledPin);

  void begin();
  void setBuzzer(bool on);
  void setFan(bool on);
  void setLed(bool on);

  bool buzzer() const { return _buzzer; }
  bool fan() const { return _fan; }
  bool led() const { return _led; }

  void flashLed(unsigned long ms);
  void update();

 private:
  uint8_t _buzzerPin, _fanPin, _ledPin;
  bool _buzzer, _fan, _led;
  unsigned long _flashEnd;
};

class SafeClock {
 public:
  SafeClock();

  bool begin(const __FlashStringHelper* buildDate, const __FlashStringHelper* buildTime, long tzOffsetSec);
  bool ok() const { return _ok; }
  unsigned long unixUtc();
  void setUtc(unsigned long unixUtc);

 private:
  RTC_DS3231 _rtc;
  bool _ok;
  long _tz;
};

#endif
