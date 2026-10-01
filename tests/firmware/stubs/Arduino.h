// Minimal stand-ins for the Arduino/ESP8266 APIs used by the sketch, to compile and test it on a PC.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <ctime>

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define PROGMEM
#define strlen_P strlen
enum { D0 = 16, D1 = 5, D2 = 4, D3 = 0, D4 = 2, D5 = 14, D6 = 12, D7 = 13, D8 = 15, LED_BUILTIN = 2 };

inline size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t n = strlen(src);
  if (size) { size_t c = n < size - 1 ? n : size - 1; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}

class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &x) : s(x) {}
  size_t length() const { return s.size(); }
  char operator[](size_t i) const { return s[i]; }
  String &operator+=(char c) { s += c; return *this; }
  bool operator==(const char *o) const { return s == o; }
  void trim() {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
  }
  String substring(size_t from, size_t to = std::string::npos) const {
    if (from > s.size()) return String();
    return String(s.substr(from, to == std::string::npos ? std::string::npos : to - from));
  }
  void remove(size_t i) { s.erase(i); }
  const char *c_str() const { return s.c_str(); }
  long toInt() const { return atol(s.c_str()); }
};

struct SerialStub {
  template <class T> void print(T) {}
  template <class T> void println(T) {}
  void println() {}
  template <class... T> void printf(const char *, T...) {}
  void begin(long) {}
};
extern SerialStub Serial;

extern std::map<int, int> pinLevel, pinModeOf;
inline void digitalWrite(int p, int v) { pinLevel[p] = v; }
inline void pinMode(int p, int m) { pinModeOf[p] = m; }
extern unsigned long fakeMillis;
inline unsigned long millis() { return fakeMillis; }
inline void delay(unsigned long) {}
