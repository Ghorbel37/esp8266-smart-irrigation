#pragma once
// Stand-in for ESP8266WiFi.h: always connected, NTP never answers (tests fake the clock with fakeNow)
#include "Arduino.h"
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6
#define WIFI_STA 1
// Tests set `connected` and read how often the sketch called begin() and disconnect()
struct WiFiStub {
  bool connected = true;
  int begins = 0, disconnects = 0, mode_ = 0;
  bool autoReconnect = false, persistent_ = true;
  void begin(const char *, const char *) { begins++; }
  int status() { return connected ? WL_CONNECTED : WL_DISCONNECTED; }
  bool disconnect() { disconnects++; return true; }
  void persistent(bool p) { persistent_ = p; }
  bool mode(int m) { mode_ = m; return true; }
  bool setAutoReconnect(bool a) { autoReconnect = a; return true; }
  const char *localIP() { return "127.0.0.1"; }
};
extern WiFiStub WiFi;
inline void configTime(long, int, const char *) {}
inline bool getLocalTime(struct tm *) { return false; }
