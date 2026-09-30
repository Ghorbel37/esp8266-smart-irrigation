#pragma once
// Stand-in for ESP8266WiFi.h: always connected, NTP never answers (tests fake the clock with fakeNow)
#include "Arduino.h"
#define WL_CONNECTED 3
struct WiFiStub {
  void begin(const char *, const char *) {}
  int status() { return WL_CONNECTED; }
  const char *localIP() { return "127.0.0.1"; }
};
extern WiFiStub WiFi;
inline void configTime(long, int, const char *) {}
inline bool getLocalTime(struct tm *) { return false; }
