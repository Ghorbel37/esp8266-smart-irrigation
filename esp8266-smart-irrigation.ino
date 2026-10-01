#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "time.h"
#include <ESP32Time.h>

// Defaults, used until they are changed from the web page's settings
#define DEFAULT_RELAY_PIN   D7                        // The ESP8266 pin connected to the relay
#define DEFAULT_ACTIVE_HIGH false                     // false: the relay turns on when the pin is LOW
#define DEFAULT_DEVICE_NAME "Irrigation ESP"          // Shown at the top of the page and in the browser tab
#define NAME_SIZE 32                                  // Including the terminating zero
#define DEFAULT_ON_MINUTES 60                         // Force ON goes back to the schedule after this, unless the page sends another duration
#define MAX_ON_MINUTES 1440                           // Longest Force ON: 24 hours

// Pins allowed for the relay. D0, D3, D4 and D8 are left out on purpose: they change level
// during boot (the relay could click) or decide how the board boots, and D0/D4 drive the built-in LEDs.
const uint8_t RELAY_PINS[] = {D1, D2, D5, D6, D7};

const char *ssid = "REPLACE_WITH_SSID";     // CHANGE IT
const char *password = "REPLACE_WITH_PASSWORD";  // CHANGE IT

ESP32Time rtc;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;
struct tm timeinfo;

ESP8266WebServer server(80); // Web server on port 80

uint8_t relayPin = DEFAULT_RELAY_PIN;
bool relayActiveHigh = DEFAULT_ACTIVE_HIGH;
char deviceName[NAME_SIZE] = DEFAULT_DEVICE_NAME;
bool relayOn = false;
unsigned long disableUntil = 0; // Timestamp until which the relay is disabled
unsigned long forceOnUntil = 0; // Timestamp when Force ON goes back to the schedule
int globalOn = LOW;
int globalOff = LOW;

// Schedule structure
struct Schedule {
  bool days[7];
  int startHour;
  int startMinute;
  int endHour;
  int endMinute;
};

Schedule schedule;

// ---- Persistent settings (saved in flash via EEPROM emulation) ----
// Uploading a new sketch keeps them (unless Tools > Erase Flash is set to "All Flash Contents").
// Bump SETTINGS_VERSION whenever the Settings layout changes: data saved with another layout is
// then ignored and the defaults are used.
#define SETTINGS_MAGIC   0x49525247  // "IRRG"
#define SETTINGS_VERSION 1

// Force ON and the pause are timed and not saved: after a restart the board follows the schedule
struct Settings {
  uint32_t magic;
  uint8_t  version;
  Schedule schedule;
  uint8_t  globalOff;
  uint8_t  relayPin;
  uint8_t  relayActiveHigh;
  char     deviceName[NAME_SIZE];
  uint8_t  checksum;
};

uint8_t settingsChecksum(const Settings &s) {
  const uint8_t *p = (const uint8_t *)&s;
  uint8_t sum = 0;
  for (size_t i = 0; i < offsetof(Settings, checksum); i++) sum ^= p[i];
  return sum;
}

bool validTime(int h, int m) {
  return h >= 0 && h < 24 && m >= 0 && m < 60;
}

// Read "HH:MM" into hour and minute; anything else leaves them unchanged
bool parseTime(const String &text, int &hour, int &minute) {
  if (text.length() != 5 || text[2] != ':') return false;
  for (int i : {0, 1, 3, 4}) if (text[i] < '0' || text[i] > '9') return false;
  int h = (text[0] - '0') * 10 + (text[1] - '0');
  int m = (text[3] - '0') * 10 + (text[4] - '0');
  if (!validTime(h, m)) return false;
  hour = h;
  minute = m;
  return true;
}

bool validSchedule(const Schedule &sc) {
  return validTime(sc.startHour, sc.startMinute) && validTime(sc.endHour, sc.endMinute);
}

bool validRelayPin(int pin) {
  for (uint8_t p : RELAY_PINS) if (p == pin) return true;
  return false;
}

// Keep a device name that is safe to put in JSON: printable, no quotes or backslashes, max NAME_SIZE-1 bytes
void setDeviceName(const String &input) {
  String name;
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if ((uint8_t)c < 0x20 || c == '"' || c == '\\') continue;
    name += c;
  }
  name.trim();
  if (name.length() > NAME_SIZE - 1) {
    name = name.substring(0, NAME_SIZE - 1);
    // Don't cut a UTF-8 character in half
    while (name.length() && ((uint8_t)name[name.length() - 1] & 0xC0) == 0x80) name.remove(name.length() - 1);
    if (name.length() && ((uint8_t)name[name.length() - 1] & 0xC0) == 0xC0) name.remove(name.length() - 1);
  }
  if (name.length() == 0) name = DEFAULT_DEVICE_NAME;
  strlcpy(deviceName, name.c_str(), NAME_SIZE);
}

void loadSettings() {
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC && s.version == SETTINGS_VERSION &&
      s.checksum == settingsChecksum(s) && validSchedule(s.schedule)) {
    schedule  = s.schedule;
    globalOff = s.globalOff ? HIGH : LOW;
    if (validRelayPin(s.relayPin)) relayPin = s.relayPin;
    relayActiveHigh = s.relayActiveHigh;
    s.deviceName[NAME_SIZE - 1] = '\0';
    setDeviceName(String(s.deviceName));
    Serial.println("Settings loaded from flash");
    return;
  }

  Serial.println("No saved settings, using defaults");
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));  // zero padding bytes so the checksum is stable
  s.magic     = SETTINGS_MAGIC;
  s.version   = SETTINGS_VERSION;
  s.schedule  = schedule;
  s.globalOff = globalOff == HIGH;
  s.relayPin  = relayPin;
  s.relayActiveHigh = relayActiveHigh;
  strlcpy(s.deviceName, deviceName, NAME_SIZE);
  s.checksum  = settingsChecksum(s);

  // Only write when something changed: flash wears out after ~10,000-100,000 writes
  Settings current;
  EEPROM.get(0, current);
  if (memcmp(&current, &s, sizeof(s)) == 0) return;

  EEPROM.put(0, s);
  if (EEPROM.commit()) Serial.println("Settings saved");
  else Serial.println("Failed to save settings");
}

// ---- Web page ----
// INDEX_HTML is the minified copy of web/index.html. After editing the page, run `python minify.py`
// to rebuild index_html.h. The browser loads the styles from the Tailwind CDN (it needs internet access)
// and reads the live state from /api/state as JSON.
#include "index_html.h"

const char *DAY_NAMES[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

void initLocalTime(const char* ntpServer, long gmtOffset_sec, int daylightOffset_sec) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
    } else {
        rtc.setTimeStruct(timeinfo);
        Serial.println("Initialized time");
    }
}

// Milliseconds left in a "Pause 5 minutes"; overflow-safe when millis() wraps
long pauseRemainingMs() {
  long left = (long)(disableUntil - millis());
  return left > 0 ? left : 0;
}

// True once NTP has set the clock. Until then it counts from 1 Jan 1970, which would
// run the schedule at the wrong time. configTime() keeps re-syncing every hour after that.
bool clockSet() {
  return time(nullptr) > 1600000000;  // any date after Sept 2020
}

// Milliseconds left before Force ON goes back to the schedule
long forceOnRemainingMs() {
  long left = (long)(forceOnUntil - millis());
  return left > 0 ? left : 0;
}

// Decide whether the relay should be on right now
bool shouldWater() {
  if (globalOn) return true;
  if (globalOff) return false;
  if (pauseRemainingMs() > 0) return false;
  if (!clockSet()) return false;

  int now   = rtc.getHour(true) * 60 + rtc.getMinute();
  int start = schedule.startHour * 60 + schedule.startMinute;
  int end   = schedule.endHour * 60 + schedule.endMinute;
  int today = rtc.getDayofWeek();

  if (start < end) return schedule.days[today] && now >= start && now < end;
  if (start == end) return false;
  // The run crosses midnight (e.g. 23:00 -> 01:00): the part after midnight
  // belongs to the run that started the day before
  int yesterday = (today + 6) % 7;
  return (schedule.days[today] && now >= start) || (schedule.days[yesterday] && now < end);
}

// Pin level that turns the relay on or off, depending on the relay module
uint8_t relayLevel(bool on) {
  return on == relayActiveHigh ? HIGH : LOW;
}

void updateRelay() {
  if (globalOn && forceOnRemainingMs() == 0) globalOn = LOW;  // Force ON ran out: back to the schedule
  relayOn = shouldWater();
  digitalWrite(relayPin, relayLevel(relayOn));
}

// Switch the relay to another pin: release the old one, start the new one switched off
void setRelayPin(uint8_t pin) {
  if (pin != relayPin) {
    digitalWrite(relayPin, relayLevel(false));
    pinMode(relayPin, INPUT);
    relayPin = pin;
  }
  digitalWrite(relayPin, relayLevel(false));
  pinMode(relayPin, OUTPUT);
}

void getNextRunTime(char *buf, size_t len) {
  if (!clockSet()) {
    snprintf(buf, len, "waiting for the clock");
    return;
  }
  int h = rtc.getHour(true);
  int m = rtc.getMinute();
  int today = rtc.getDayofWeek();

  // i == 7 covers "same weekday next week" when today's run already started
  for (int i = 0; i <= 7; i++) {
    int day = (today + i) % 7;
    if (!schedule.days[day]) continue;
    bool startPassed = h > schedule.startHour || (h == schedule.startHour && m >= schedule.startMinute);
    if (i == 0 && startPassed) continue;

    if (i == 0) snprintf(buf, len, "today at %02d:%02d", schedule.startHour, schedule.startMinute);
    else if (i == 1) snprintf(buf, len, "tomorrow at %02d:%02d", schedule.startHour, schedule.startMinute);
    else snprintf(buf, len, "%s at %02d:%02d", DAY_NAMES[day], schedule.startHour, schedule.startMinute);
    return;
  }
  snprintf(buf, len, "none scheduled");
}

// Send the current state as JSON (read by the web page)
void sendState() {
  updateRelay();  // reflect a change made by the request right away

  char next[32];
  getNextRunTime(next, sizeof(next));
  const char *mode = globalOn ? "on" : globalOff ? "off" : "auto";

  char json[384];
  snprintf(json, sizeof(json),
    "{\"mode\":\"%s\",\"relay\":%s,\"paused\":%ld,\"onLeft\":%ld,"
    "\"days\":[%d,%d,%d,%d,%d,%d,%d],"
    "\"start\":\"%02d:%02d\",\"end\":\"%02d:%02d\","
    "\"time\":\"%02d:%02d\",\"next\":\"%s\","
    "\"name\":\"%s\",\"pin\":%d,\"activeHigh\":%s}",
    mode, relayOn ? "true" : "false", pauseRemainingMs() / 1000, globalOn ? forceOnRemainingMs() / 1000 : 0L,
    schedule.days[0], schedule.days[1], schedule.days[2], schedule.days[3],
    schedule.days[4], schedule.days[5], schedule.days[6],
    schedule.startHour, schedule.startMinute, schedule.endHour, schedule.endMinute,
    rtc.getHour(true), rtc.getMinute(), next,
    deviceName, relayPin, relayActiveHigh ? "true" : "false");
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(LED_BUILTIN, OUTPUT);

  EEPROM.begin(sizeof(Settings));
  loadSettings();
  saveSettings();          // stores the defaults on the first start (no write otherwise)
  setRelayPin(relayPin);   // relay starts switched off on the saved pin

  Serial.println("Connecting to Wifi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("Connected to WiFi");

  initLocalTime(ntpServer, gmtOffset_sec, daylightOffset_sec);

  Serial.print("ESP8266 Web Server's IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/state", HTTP_GET, sendState);

  // Actions only accept POST, with form-encoded arguments in the body
  // Force ON for minutes=N (default 60, at most 24 h), then back to the schedule
  server.on("/relay1/on", HTTP_POST, []() {
    long minutes = server.hasArg("minutes") ? server.arg("minutes").toInt() : DEFAULT_ON_MINUTES;
    if (minutes < 1) minutes = DEFAULT_ON_MINUTES;
    if (minutes > MAX_ON_MINUTES) minutes = MAX_ON_MINUTES;
    forceOnUntil = millis() + minutes * 60UL * 1000;
    globalOff = LOW;
    globalOn = HIGH;
    saveSettings();  // only writes when leaving Force OFF
    sendState();
  });

  server.on("/relay1/off", HTTP_POST, []() {
    globalOn = LOW;
    globalOff = HIGH;
    saveSettings();
    sendState();
  });

  server.on("/relay1/clear", HTTP_POST, []() {
    globalOn = LOW;
    globalOff = LOW;
    saveSettings();
    sendState();
  });

  server.on("/setSchedule", HTTP_POST, []() {
    for (int i = 0; i < 7; i++) {
      schedule.days[i] = false;
    }
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "day") {
        int day = server.arg(i).toInt();
        if (day >= 0 && day < 7) schedule.days[day] = true;
      }
    }

    // Times are only changed when they are valid
    if (server.hasArg("startTime")) parseTime(server.arg("startTime"), schedule.startHour, schedule.startMinute);
    if (server.hasArg("endTime")) parseTime(server.arg("endTime"), schedule.endHour, schedule.endMinute);

    saveSettings();
    sendState();
  });

  // Device settings: POST /config with name=Garden&pin=13&activeHigh=0 (every argument is optional)
  server.on("/config", HTTP_POST, []() {
    if (server.hasArg("name")) setDeviceName(server.arg("name"));
    if (server.hasArg("pin")) {
      int pin = server.arg("pin").toInt();
      if (validRelayPin(pin)) setRelayPin(pin);
    }
    if (server.hasArg("activeHigh")) {
      String v = server.arg("activeHigh");
      relayActiveHigh = v == "1" || v == "true";
    }
    saveSettings();
    sendState();
  });

  server.on("/disable", HTTP_POST, []() {
    disableUntil = millis() + 5UL * 60 * 1000; // Disable for 5 minutes
    sendState();
  });

  server.begin();
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  server.handleClient();
  updateRelay();

  // Let the ESP8266 idle between passes: it draws much less current (v2.4 never paused),
  // which matters when the board runs from the 12 V supply through a regulator.
  // Web requests wait at most 100 ms.
  delay(100);
}
