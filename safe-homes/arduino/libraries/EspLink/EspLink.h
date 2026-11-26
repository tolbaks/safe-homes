#ifndef ESPLINK_H
#define ESPLINK_H

#include <Arduino.h>
#include <SoftwareSerial.h>

enum CmdType { CMD_NONE, CMD_AUTO, CMD_MAINTENANCE, CMD_SLEEP, CMD_READ, CMD_SET, CMD_SAVED, CMD_TIME };

struct EspCommand {
  CmdType type;
  char target[12];
  long value;
};

class EspLink {
 public:
  EspLink(uint8_t rxPin, uint8_t txPin, long baud = 9600);
  void begin();
  void end();

  void sendData(char kind, const char* mode, float distance, float temp, float hum,
                int gas, bool alarm, bool fan, bool led, unsigned long ts);
  void sendEvent(char kind, unsigned long ts);
  void sendLine(const char* line);

  bool poll(EspCommand& cmd);

 private:
  bool parse(EspCommand& cmd);

  SoftwareSerial _ss;
  long _baud;
  char _buf[40];
  uint8_t _len;
};

#ifndef SAFEBOARD_USE_SD
#define SAFEBOARD_USE_SD 1
#endif

class SdQueue {
 public:
  SdQueue();

  bool begin(uint8_t csPin);
  bool ready() const { return _ok; }
  bool add(const char* row);
  void update(EspLink& link);
  void onSaved(unsigned long ts);
  void reset();

 private:
  void loadOffset();
  void saveOffset();

  bool _ok, _outstanding;
  unsigned long _sentAt, _nextTry, _outTs, _offset, _last;
  int _curLen;
  char _buf[64];
};

#endif
