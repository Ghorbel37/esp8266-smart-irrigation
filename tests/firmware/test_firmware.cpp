// Firmware tests: compiles the real sketch on a PC against the stand-ins in stubs/ and checks its logic.
//
//   test_firmware --list     list the tests
//   test_firmware <name>     run one test (each test runs in its own process, so it starts from a fresh board)
//
// Built and run by tests/run_tests.py. By hand:
//   g++ -std=c++17 -Wall -Itests/firmware/stubs tests/firmware/test_firmware.cpp -o test_firmware
#include "Arduino.h"
#include "ESP8266WiFi.h"
#include "EEPROM.h"
SerialStub Serial;
WiFiStub WiFi;
EEPROMStub EEPROM;               // starts erased (all 0xFF), like a new board
std::map<int, int> pinLevel, pinModeOf;
unsigned long fakeMillis = 1000;  // what millis() returns; tests move it forward
time_t fakeNow = 1790000000;      // what time(nullptr) returns: a 2026 date (clock set) unless a test changes it
#define time(p) (fakeNow)

#include "../../esp8266-smart-irrigation.ino"

// ---- Test framework ----
struct Test { const char *name, *what; void (*run)(); };
std::vector<Test> &allTests() { static std::vector<Test> t; return t; }
#define TEST(name, what) \
  static void test_##name(); \
  static bool registered_##name = (allTests().push_back({#name, what, test_##name}), true); \
  static void test_##name()

int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("  FAIL line %d: %s\n    last response: %s\n", __LINE__, #cond, server.lastBody.c_str()); failures++; } } while (0)

// Send a request to one of the sketch's routes (the method is checked by the "methods" test)
void req(const char *path, std::vector<std::pair<std::string, std::string>> args = {}) {
  server.args = args;
  server.request(path);
}
bool responseHas(const char *text) { return server.lastBody.find(text) != std::string::npos; }

// ---- Tests ----

TEST(fresh, "A new board starts with the defaults and the relay off on D7") {
  setup();
  CHECK(std::string(deviceName) == "Irrigation ESP");
  CHECK(relayPin == D7);
  CHECK(!relayActiveHigh);
  CHECK(pinModeOf[D7] == OUTPUT && pinLevel[D7] == HIGH);  // active-LOW module: HIGH = off
  CHECK(EEPROM.commits == 1);                               // defaults stored once
  req("/api/state");
  CHECK(responseHas("\"mode\":\"auto\"") && responseHas("\"relay\":false"));
}

TEST(settings, "Device settings are validated, applied right away and survive a restart") {
  setup();
  req("/setSchedule", {{"day", "1"}, {"startTime", "09:00"}, {"endTime", "11:00"}});  // Monday 09-11 (now: Monday 10:00)
  req("/config", {{"name", "  My \"Garden\\ \x01 Zone  "}});
  CHECK(std::string(deviceName) == "My Garden  Zone");     // quotes, backslashes, control chars removed
  req("/config", {{"name", "Système d'arrosage du jardin principal"}});
  CHECK(strlen(deviceName) <= 31);                         // cut without breaking a UTF-8 character
  req("/config", {{"name", "   "}});
  CHECK(std::string(deviceName) == "Irrigation ESP");      // empty: back to the default
  req("/config", {{"pin", "0"}});                          // D3: not allowed
  CHECK(relayPin == D7);
  req("/config", {{"pin", "15"}});                         // D8: not allowed
  CHECK(relayPin == D7);
  req("/config", {{"pin", "5"}, {"activeHigh", "1"}, {"name", "Garden"}});  // D1, active-HIGH module
  CHECK(relayPin == D1 && pinModeOf[D7] == INPUT && pinModeOf[D1] == OUTPUT);
  CHECK(relayActiveHigh && relayOn && pinLevel[D1] == HIGH);  // watering, active-HIGH: HIGH = on

  // Restart: forget everything in RAM, reload from flash
  relayPin = DEFAULT_RELAY_PIN; relayActiveHigh = DEFAULT_ACTIVE_HIGH;
  strlcpy(deviceName, DEFAULT_DEVICE_NAME, NAME_SIZE); memset(&schedule, 0, sizeof(schedule));
  loadSettings();
  CHECK(std::string(deviceName) == "Garden" && relayPin == D1 && relayActiveHigh && schedule.days[1]);
  int before = EEPROM.commits;
  req("/config", {{"pin", "5"}, {"activeHigh", "1"}, {"name", "Garden"}});  // same values
  CHECK(EEPROM.commits == before);                                           // so flash isn't written
}

TEST(otherdata, "Saved data from another layout or with a bad checksum is ignored") {
  Settings s; memset(&s, 0, sizeof(s));
  s.magic = SETTINGS_MAGIC; s.version = SETTINGS_VERSION + 1;  // a different layout
  s.schedule.days[1] = true; s.schedule.startHour = 9; s.schedule.endHour = 11;
  s.relayPin = D1; strlcpy(s.deviceName, "Garden", NAME_SIZE);
  s.checksum = settingsChecksum(s);
  EEPROM.put(0, s);
  setup();
  CHECK(!schedule.days[1] && relayPin == D7 && std::string(deviceName) == "Irrigation ESP");
  Settings after; EEPROM.get(0, after);
  CHECK(after.version == SETTINGS_VERSION && after.checksum == settingsChecksum(after));  // defaults stored

  s.version = SETTINGS_VERSION; s.checksum = settingsChecksum(s) ^ 1;  // right layout, bad checksum
  EEPROM.put(0, s);
  loadSettings();
  CHECK(relayPin == D7 && std::string(deviceName) == "Irrigation ESP");

  s.checksum = settingsChecksum(s);                                     // valid: loaded
  EEPROM.put(0, s);
  loadSettings();
  CHECK(schedule.days[1] && relayPin == D1 && std::string(deviceName) == "Garden");
}

TEST(schedule, "The schedule waters on the chosen days, including runs that cross midnight") {
  setup();
  auto at = [](int dow, int h, int m) { rtc.dow = dow; rtc.hour = h; rtc.minute = m; return shouldWater(); };
  req("/setSchedule", {{"day", "1"}, {"startTime", "09:00"}, {"endTime", "11:00"}});  // normal run
  CHECK(!at(1, 8, 59) && at(1, 9, 0) && at(1, 10, 59) && !at(1, 11, 0) && !at(2, 10, 0));
  req("/setSchedule", {{"day", "1"}, {"startTime", "23:00"}, {"endTime", "01:00"}});  // Monday night
  CHECK(!at(1, 22, 59));
  CHECK(at(1, 23, 0));
  CHECK(at(1, 23, 59));
  CHECK(at(2, 0, 0));      // Tuesday just after midnight: still Monday's run
  CHECK(at(2, 0, 59));
  CHECK(!at(2, 1, 0));
  CHECK(!at(2, 23, 30));   // Tuesday is not scheduled
  CHECK(!at(1, 0, 30));    // Monday early morning belongs to Sunday's run (not scheduled)
  req("/setSchedule", {{"day", "6"}, {"startTime", "22:00"}, {"endTime", "02:00"}});  // Saturday night
  CHECK(at(0, 1, 0));      // wraps to Sunday
  req("/setSchedule", {{"day", "1"}, {"startTime", "09:00"}, {"endTime", "09:00"}});  // start = end
  CHECK(!at(1, 9, 0));     // never waters
  req("/setSchedule", {{"day", "9"}, {"startTime", "25:00"}, {"endTime", "xx"}});      // invalid values
  CHECK(schedule.startHour == 9 && schedule.endHour == 9);  // times unchanged
  for (auto bad : {"9:00", "09:0", "24:00", "12:60", "ab:cd", "12-30", "12:30:00", ""}) {
    req("/setSchedule", {{"startTime", bad}});
    CHECK(schedule.startHour == 9 && schedule.startMinute == 0);
  }
  req("/setSchedule", {{"startTime", "23:59"}, {"endTime", "00:00"}});
  CHECK(schedule.startHour == 23 && schedule.startMinute == 59 && schedule.endHour == 0 && schedule.endMinute == 0);
  for (bool d : schedule.days) CHECK(!d);                   // day 9 ignored
}

TEST(nextrun, "The next run shown on the page") {
  setup();
  rtc.dow = 1; rtc.hour = 10; rtc.minute = 0;  // Monday 10:00
  char next[32];
  getNextRunTime(next, sizeof(next));
  CHECK(std::string(next) == "none scheduled");
  req("/setSchedule", {{"day", "1"}, {"day", "3"}, {"startTime", "11:00"}, {"endTime", "12:00"}});
  getNextRunTime(next, sizeof(next));
  CHECK(std::string(next) == "today at 11:00");
  rtc.hour = 11;  // started: next is Wednesday
  getNextRunTime(next, sizeof(next));
  CHECK(std::string(next) == "Wednesday at 11:00");
  req("/setSchedule", {{"day", "2"}, {"startTime", "06:00"}, {"endTime", "07:00"}});
  getNextRunTime(next, sizeof(next));
  CHECK(std::string(next) == "tomorrow at 06:00");
  req("/setSchedule", {{"day", "1"}, {"startTime", "06:00"}, {"endTime", "07:00"}});
  getNextRunTime(next, sizeof(next));
  CHECK(std::string(next) == "Monday at 06:00");  // today's run is over: same day next week
}

TEST(modes, "Force OFF and back to schedule; Force OFF is saved, the pause lasts 5 minutes") {
  setup();
  req("/setSchedule", {{"day", "1"}, {"startTime", "09:00"}, {"endTime", "11:00"}});  // watering now
  CHECK(relayOn && pinLevel[D7] == LOW);
  req("/relay1/off");
  CHECK(!relayOn && pinLevel[D7] == HIGH && responseHas("\"mode\":\"off\""));
  Settings saved; EEPROM.get(0, saved);
  CHECK(saved.globalOff == 1);
  req("/relay1/clear");
  CHECK(relayOn && responseHas("\"mode\":\"auto\""));
  req("/disable");
  CHECK(!relayOn && responseHas("\"paused\":300"));
  fakeMillis += 5UL * 60 * 1000 - 1000; updateRelay();
  CHECK(!relayOn);
  fakeMillis += 1000; updateRelay();                   // 5 minutes later
  CHECK(relayOn);
}

TEST(forceon, "Force ON turns off after its duration and is never written to flash") {
  setup();
  int commits = EEPROM.commits;
  auto left = [] { return forceOnRemainingMs() / 1000; };
  req("/relay1/on");                                   // no duration: 60 min
  CHECK(globalOn && relayOn && left() == 3600);
  CHECK(responseHas("\"mode\":\"on\"") && responseHas("\"onLeft\":3600"));
  CHECK(EEPROM.commits == commits);                    // Force ON doesn't write flash
  fakeMillis += 59UL * 60 * 1000; updateRelay();
  CHECK(globalOn && relayOn);
  fakeMillis += 60UL * 1000; updateRelay();            // 60 min reached
  CHECK(!globalOn && !relayOn);
  req("/api/state");
  CHECK(responseHas("\"mode\":\"auto\"") && responseHas("\"onLeft\":0"));

  req("/relay1/on", {{"minutes", "5"}});    CHECK(left() == 300);
  req("/relay1/on", {{"minutes", "0"}});    CHECK(left() == 3600);  // invalid: default
  req("/relay1/on", {{"minutes", "abc"}});  CHECK(left() == 3600);
  req("/relay1/on", {{"minutes", "-3"}});   CHECK(left() == 3600);
  req("/relay1/on", {{"minutes", "5000"}}); CHECK(left() == 1440L * 60);  // capped at 24 h

  req("/relay1/off");                                  // Force OFF is saved...
  CHECK(EEPROM.commits == commits + 1);
  req("/relay1/on", {{"minutes", "1"}});               // ...so leaving it writes once
  CHECK(EEPROM.commits == commits + 2);
  req("/relay1/clear");                                // Force ON -> schedule: nothing to write
  CHECK(EEPROM.commits == commits + 2);

  // millis() wraps around every ~49.7 days
  fakeMillis = 0xFFFFFFFFUL - 1000;
  req("/relay1/on", {{"minutes", "1"}});
  fakeMillis += 30000; updateRelay();
  CHECK(globalOn && left() == 30);
  fakeMillis += 30000; updateRelay();
  CHECK(!globalOn);
}

TEST(clock, "The schedule waits until NTP has set the clock; Force ON still works") {
  setup();
  req("/setSchedule", {{"day", "4"}, {"startTime", "00:00"}, {"endTime", "23:59"}});  // all Thursday
  rtc.dow = 4; rtc.hour = 1; rtc.minute = 0;
  fakeNow = 3600;                        // 1 Jan 1970 01:00, a Thursday: clock not set
  CHECK(!shouldWater());
  req("/api/state");
  CHECK(responseHas("\"next\":\"waiting for the clock\""));
  req("/relay1/on");
  CHECK(relayOn);
  req("/relay1/clear");
  CHECK(!relayOn);
  fakeNow = 1790000000;                  // NTP synced
  CHECK(shouldWater());
}

TEST(methods, "Reading is GET, every action is POST") {
  setup();
  CHECK(server.methods.at("/") == HTTP_GET);
  CHECK(server.methods.at("/api/state") == HTTP_GET);
  for (auto p : {"/relay1/on", "/relay1/off", "/relay1/clear", "/disable", "/setSchedule", "/config"})
    CHECK(server.methods.at(p) == HTTP_POST);
  CHECK(server.methods.size() == 8);
}

TEST(json, "The longest possible state still fits the JSON buffer") {
  setup();
  setDeviceName("WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");  // cut to 31 characters
  for (int i = 0; i < 7; i++) schedule.days[i] = i == 3;
  rtc.dow = 3; rtc.hour = 23; rtc.minute = 59; schedule.startHour = 23; schedule.startMinute = 0;
  req("/disable");
  req("/relay1/on", {{"minutes", "1440"}});
  printf("  longest JSON: %zu of 383 bytes\n", server.lastBody.size());
  CHECK(server.lastBody.size() < 383 && server.lastBody.back() == '}');
  CHECK(responseHas("\"next\":\"Wednesday at 23:00\""));
}

TEST(page, "\"/\" serves the minified page from index_html.h") {
  setup();
  req("/");
  CHECK(server.lastBody == INDEX_HTML);
  CHECK(server.lastBody.rfind("<!DOCTYPE html>", 0) == 0);
  CHECK(server.lastBody.size() >= 7 && server.lastBody.compare(server.lastBody.size() - 7, 7, "</html>") == 0);
  printf("  page: %zu bytes\n", server.lastBody.size());
}

TEST(wifi, "Without Wi-Fi the board still starts, and the connection is retried every 30 s") {
  WiFi.connected = false;              // router down at boot
  setup();                             // must not wait forever
  CHECK(WiFi.mode_ == WIFI_STA && WiFi.autoReconnect && !WiFi.persistent_);
  int begins = WiFi.begins;
  checkWifi();                         // notices the outage
  fakeMillis += 29000; checkWifi();
  CHECK(WiFi.begins == begins);        // not yet
  fakeMillis += 1000; checkWifi();
  CHECK(WiFi.begins == begins + 1 && WiFi.disconnects == 1);  // 30 s: start again from scratch
  fakeMillis += 10000; checkWifi();
  CHECK(WiFi.begins == begins + 1);
  fakeMillis += 20000; checkWifi();
  CHECK(WiFi.begins == begins + 2);    // and every 30 s after that
  WiFi.connected = true; checkWifi();
  fakeMillis += 60000; checkWifi();
  CHECK(WiFi.begins == begins + 2);    // back: no more retries
  WiFi.connected = false; checkWifi(); // a new outage starts a new count
  fakeMillis += 30000; checkWifi();
  CHECK(WiFi.begins == begins + 3);
}

TEST(wifiwatering, "A Wi-Fi outage doesn't stop the schedule (the clock keeps running)") {
  setup();
  req("/setSchedule", {{"day", "1"}, {"startTime", "09:00"}, {"endTime", "11:00"}});  // now: Monday 10:00
  WiFi.connected = false;
  for (int i = 0; i < 20; i++) { fakeMillis += 30000; checkWifi(); updateRelay(); }   // 10 minutes
  CHECK(relayOn);
}

TEST(nokeepalive, "Every answer closes its connection, so one browser can't block the others") {
  setup();
  req("/");
  CHECK(!server.keepAlive_);
  server.keepAlive_ = true;
  req("/api/state");
  CHECK(!server.keepAlive_);
  server.keepAlive_ = true;
  req("/relay1/off");
  CHECK(!server.keepAlive_);
}

// ---- Main ----
int main(int argc, char **argv) {
  std::string arg = argc > 1 ? argv[1] : "--list";
  if (arg == "--list") {
    for (auto &t : allTests()) printf("%s\t%s\n", t.name, t.what);
    return 0;
  }
  for (auto &t : allTests()) {
    if (arg != t.name) continue;
    t.run();
    return failures ? 1 : 0;
  }
  printf("Unknown test: %s\n", arg.c_str());
  return 2;
}
