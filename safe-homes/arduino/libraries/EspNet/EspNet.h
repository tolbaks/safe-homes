#ifndef ESPNET_H
#define ESPNET_H

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

struct WifiSettings {
  char magic[4];
  char ssid[33];
  char pass[65];
  char server[40];
  char key[40];
};

class WifiSetup {
 public:
  WifiSetup(const char* apName, const char* apPass);
  void begin();
  void update();
  bool connected() const;
  const char* server() const { return _cfg.server; }
  const char* key() const { return _cfg.key; }

 private:
  void load();
  void save();
  void startAP();
  void stopAP();
  void manage();
  void handleRoot();
  void handleSave();
  bool onSetupNetwork();
  String page(const String& msg);
  static String esc(const String& s);

  WifiSettings _cfg;
  ESP8266WebServer _web;
  const char* _apName;
  const char* _apPass;
  bool _apOn, _restartPending;
  unsigned long _lostSince, _apSince, _restartAt;
};

#include <Arduino.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

class ServerLink {
 public:
  ServerLink(WifiSetup& wifi);
  int post(const String& path, const String& body);
  void postEvent(const String& path, const String& body);
  void flushEvents();
  bool poll(String& body);
  static bool finished(int code);

 private:
  static const uint8_t QMAX = 4;
  WifiSetup& _wifi;
  String _qPath[QMAX];
  String _qBody[QMAX];
  uint8_t _qCount;
};

#endif
