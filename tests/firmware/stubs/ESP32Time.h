#pragma once
// Stand-in for ESP32Time: tests set the day and time directly (rtc.dow, rtc.hour, rtc.minute)
#include "Arduino.h"
struct ESP32Time {
  int hour = 10, minute = 0, dow = 1;  // Monday 10:00
  int getHour(bool) { return hour; }
  int getMinute() { return minute; }
  int getDayofWeek() { return dow; }
  void setTimeStruct(struct tm) {}
};
