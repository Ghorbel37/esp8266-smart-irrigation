#pragma once
#include "Arduino.h"
// Flash emulation backed by a byte array that tests can inspect or pre-fill
struct EEPROMStub {
  uint8_t data[512];
  int commits = 0;
  EEPROMStub() { memset(data, 0xFF, sizeof(data)); }  // erased flash reads as 0xFF
  void begin(size_t) {}
  template <class T> void get(int addr, T &v) { memcpy(&v, data + addr, sizeof(T)); }
  template <class T> void put(int addr, const T &v) { memcpy(data + addr, &v, sizeof(T)); }
  bool commit() { commits++; return true; }
};
extern EEPROMStub EEPROM;
