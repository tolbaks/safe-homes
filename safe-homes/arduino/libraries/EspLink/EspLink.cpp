#include "EspLink.h"
#if SAFEBOARD_USE_SD
#include <SD.h>
#endif

EspLink::EspLink(uint8_t rxPin, uint8_t txPin, long baud)
    : _ss(rxPin, txPin), _baud(baud), _len(0) {}

void EspLink::begin() {
  _ss.begin(_baud);
  _len = 0;
}

void EspLink::end() {
  _ss.end();
}

void EspLink::sendData(char kind, const char* mode, float distance, float temp, float hum,
                       int gas, bool alarm, bool fan, bool led, unsigned long ts) {
  _ss.print(kind);   _ss.print(',');
  _ss.print(mode);   _ss.print(',');
  _ss.print(distance, 1); _ss.print(',');
  _ss.print(temp, 1); _ss.print(',');
  _ss.print(hum, 1);  _ss.print(',');
  _ss.print(gas);    _ss.print(',');
  _ss.print(alarm ? 1 : 0); _ss.print(',');
  _ss.print(fan ? 1 : 0);   _ss.print(',');
  _ss.print(led ? 1 : 0);   _ss.print(',');
  _ss.print(ts);
  _ss.print('\n');
}

void EspLink::sendEvent(char kind, unsigned long ts) {
  _ss.print(kind);
  _ss.print(',');
  _ss.print(ts);
  _ss.print('\n');
}

void EspLink::sendLine(const char* line) {
  _ss.print(line);
  _ss.print('\n');
}

bool EspLink::poll(EspCommand& cmd) {
  while (_ss.available()) {
    char c = _ss.read();
    if (c == '\r') continue;
    if (c == '\n') {
      _buf[_len] = 0;
      _len = 0;
      return parse(cmd);
    }
    if (_len < sizeof(_buf) - 1) _buf[_len++] = c;
    else _len = 0;
  }
  return false;
}

bool EspLink::parse(EspCommand& cmd) {
  cmd.type = CMD_NONE;
  cmd.target[0] = 0;
  cmd.value = 0;

  char* tok = strtok(_buf, ",");
  if (!tok) return false;
  if (tok[0] == 'K') {
    tok = strtok(NULL, ",");
    cmd.type = CMD_SAVED;
    cmd.value = tok ? atol(tok) : 0;
    return true;
  }
  if (tok[0] != 'C') return false;
  tok = strtok(NULL, ",");
  if (!tok) return false;

  if (!strcmp(tok, "AUTO")) cmd.type = CMD_AUTO;
  else if (!strcmp(tok, "MAINTENANCE")) cmd.type = CMD_MAINTENANCE;
  else if (!strcmp(tok, "SLEEP")) cmd.type = CMD_SLEEP;
  else if (!strcmp(tok, "READ")) cmd.type = CMD_READ;
  else if (!strcmp(tok, "SET")) cmd.type = CMD_SET;
  else if (!strcmp(tok, "TIME")) {
    cmd.type = CMD_TIME;
    tok = strtok(NULL, ",");
    cmd.value = tok ? atol(tok) : 0;
    return true;
  }
  else return false;

  tok = strtok(NULL, ",");
  if (tok) strncpy(cmd.target, tok, sizeof(cmd.target) - 1);
  cmd.target[sizeof(cmd.target) - 1] = 0;
  tok = strtok(NULL, ",");
  if (tok) cmd.value = atol(tok);
  return true;
}

SdQueue::SdQueue()
    : _ok(false), _outstanding(false), _sentAt(0), _nextTry(0), _outTs(0), _offset(0), _last(0), _curLen(0) {}

void SdQueue::reset() {
  _outstanding = false;
  _nextTry = 0;
}

#if SAFEBOARD_USE_SD

bool SdQueue::begin(uint8_t csPin) {
  _ok = SD.begin(csPin);
  if (_ok) loadOffset();
  return _ok;
}

void SdQueue::loadOffset() {
  _offset = 0;
  File f = SD.open("Q.OFF", FILE_READ);
  if (f) { _offset = f.parseInt(); f.close(); }
}

void SdQueue::saveOffset() {
  SD.remove("Q.OFF");
  File f = SD.open("Q.OFF", FILE_WRITE);
  if (f) { f.print(_offset); f.close(); }
}

bool SdQueue::add(const char* row) {
  if (!_ok) return false;
  File f = SD.open("Q.CSV", FILE_WRITE);
  if (!f) return false;
  f.print(row);
  f.print('\n');
  f.close();
  return true;
}

void SdQueue::update(EspLink& link) {
  if (!_ok || millis() - _last < 500UL) return;
  _last = millis();

  if (_outstanding) {
    if (millis() - _sentAt > 6000UL) {
      _outstanding = false;
      _nextTry = millis() + 10000UL;
    }
    return;
  }
  if ((long)(millis() - _nextTry) < 0) return;

  File f = SD.open("Q.CSV", FILE_READ);
  if (!f) return;
  if (_offset >= f.size()) {
    f.close();
    SD.remove("Q.CSV");
    SD.remove("Q.OFF");
    _offset = 0;
    return;
  }
  f.seek(_offset);
  int n = f.readBytesUntil('\n', _buf, sizeof(_buf) - 1);
  f.close();
  _buf[n] = 0;
  _curLen = n + 1;

  char* lastComma = strrchr(_buf, ',');
  if (!lastComma) { _offset += _curLen; saveOffset(); return; }
  _outTs = strtoul(lastComma + 1, NULL, 10);
  link.sendLine(_buf);
  _outstanding = true;
  _sentAt = millis();
}

void SdQueue::onSaved(unsigned long ts) {
  if (_outstanding && ts == _outTs) {
    _offset += _curLen;
    saveOffset();
    _outstanding = false;
    _nextTry = 0;
  }
}

#else

bool SdQueue::begin(uint8_t) { return false; }
bool SdQueue::add(const char*) { return false; }
void SdQueue::update(EspLink&) {}
void SdQueue::onSaved(unsigned long) {}
void SdQueue::loadOffset() {}
void SdQueue::saveOffset() {}

#endif
