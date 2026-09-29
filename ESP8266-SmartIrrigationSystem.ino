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
// Bump SETTINGS_VERSION whenever the Settings layout changes, so old data is ignored.
#define SETTINGS_MAGIC   0x49525247  // "IRRG"
#define SETTINGS_VERSION 2

struct Settings {
  uint32_t magic;
  uint8_t  version;
  Schedule schedule;
  uint8_t  globalOn;
  uint8_t  globalOff;
  uint8_t  relayPin;
  uint8_t  relayActiveHigh;
  char     deviceName[NAME_SIZE];
  uint8_t  checksum;
};

// Layout saved by version 1 (before the device settings), read once to migrate
struct SettingsV1 {
  uint32_t magic;
  uint8_t  version;
  Schedule schedule;
  uint8_t  globalOn;
  uint8_t  globalOff;
  uint8_t  checksum;
};

uint8_t xorChecksum(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) sum ^= p[i];
  return sum;
}

uint8_t settingsChecksum(const Settings &s) {
  return xorChecksum(&s, offsetof(Settings, checksum));
}

bool validTime(int h, int m) {
  return h >= 0 && h < 24 && m >= 0 && m < 60;
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
    globalOn  = s.globalOn ? HIGH : LOW;
    globalOff = s.globalOff ? HIGH : LOW;
    if (validRelayPin(s.relayPin)) relayPin = s.relayPin;
    relayActiveHigh = s.relayActiveHigh;
    s.deviceName[NAME_SIZE - 1] = '\0';
    setDeviceName(String(s.deviceName));
    Serial.println("Settings loaded from flash");
    return;
  }

  // Settings saved by version 1: keep the schedule and mode, use defaults for the rest
  SettingsV1 v1;
  EEPROM.get(0, v1);
  if (v1.magic == SETTINGS_MAGIC && v1.version == 1 &&
      v1.checksum == xorChecksum(&v1, offsetof(SettingsV1, checksum)) && validSchedule(v1.schedule)) {
    schedule  = v1.schedule;
    globalOn  = v1.globalOn ? HIGH : LOW;
    globalOff = v1.globalOff ? HIGH : LOW;
    Serial.println("Settings migrated from version 1");
    return;  // saved in the new layout by setup()
  }

  Serial.println("No saved settings, using defaults");
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));  // zero padding bytes so the checksum is stable
  s.magic     = SETTINGS_MAGIC;
  s.version   = SETTINGS_VERSION;
  s.schedule  = schedule;
  s.globalOn  = globalOn == HIGH;
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
// Stored in flash (PROGMEM) and sent as-is, so it uses no RAM.
// Styling comes from the Tailwind CDN, loaded by the browser (the phone/PC needs internet access).
// The page reads the live state from /api/state as JSON.
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Irrigation ESP</title><link rel="icon" href="https://upload.wikimedia.org/wikipedia/commons/1/1c/Circle-icons-water.svg">
<script src="https://cdn.tailwindcss.com"></script>
<script>
tailwind.config={darkMode:'class'};
// Follow the device theme unless the user picked one with the toggle (remembered in localStorage)
const dq=matchMedia('(prefers-color-scheme: dark)');
function saved(){try{return localStorage.theme}catch(e){}}
function applyTheme(){const t=saved();document.documentElement.classList.toggle('dark',t?t=='dark':dq.matches)}
applyTheme();dq.addEventListener('change',applyTheme);
</script></head>
<body class="bg-slate-100 text-slate-800 min-h-screen dark:bg-slate-950 dark:text-slate-100 dark:[color-scheme:dark]">
<main class="max-w-4xl mx-auto p-4 sm:p-6 space-y-4">
<header class="flex items-center justify-between gap-2">
<h1 id="title" class="text-xl sm:text-2xl font-bold truncate"></h1>
<div class="flex items-center gap-1 shrink-0">
<span id="clock" class="text-sm text-slate-500 dark:text-slate-400 tabular-nums mr-1">--:--</span>
<button id="cfgBtn" type="button" aria-label="Device settings" title="Device settings" class="rounded-lg p-2 text-slate-500 hover:bg-slate-200 dark:text-slate-400 dark:hover:bg-slate-800">
<svg class="h-5 w-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg>
</button>
<button id="theme" type="button" aria-label="Toggle dark mode" title="Toggle dark mode" class="rounded-lg p-2 text-slate-500 hover:bg-slate-200 dark:text-slate-400 dark:hover:bg-slate-800">
<svg class="h-5 w-5 dark:hidden" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>
<svg class="hidden h-5 w-5 dark:block" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>
</button></div></header>
<p id="err" class="hidden rounded-xl bg-red-50 text-red-700 text-sm p-3 dark:bg-red-950 dark:text-red-300"></p>
<form id="cfg" class="hidden bg-white rounded-2xl shadow-sm p-5 space-y-4 dark:bg-slate-900">
<h2 class="font-semibold">Device settings</h2>
<div class="grid gap-4 sm:grid-cols-2">
<label class="text-sm text-slate-600 dark:text-slate-400">Device name<input id="cfgName" type="text" maxlength="31" required class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800 dark:bg-slate-800 dark:border-slate-700 dark:text-slate-100"></label>
<label class="text-sm text-slate-600 dark:text-slate-400">Relay pin<select id="cfgPin" class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800 bg-white dark:bg-slate-800 dark:border-slate-700 dark:text-slate-100"></select></label></div>
<label class="flex items-center gap-3 text-sm"><input id="cfgHigh" type="checkbox" class="h-4 w-4 accent-sky-600">Relay turns on when the pin is HIGH</label>
<p class="text-xs text-slate-500 dark:text-slate-400">Leave it unchecked for relay modules that switch on with a LOW signal (most common).</p>
<div class="flex items-center gap-3"><button class="rounded-xl bg-sky-600 text-white px-5 py-2.5 font-medium hover:bg-sky-700">Save settings</button>
<button id="cfgCancel" type="button" class="rounded-xl px-4 py-2.5 font-medium text-slate-600 hover:bg-slate-100 dark:text-slate-300 dark:hover:bg-slate-800">Cancel</button></div>
</form>
<div class="grid gap-4 md:grid-cols-2">
<section class="bg-white rounded-2xl shadow-sm p-5 space-y-5 dark:bg-slate-900">
<div class="flex items-center gap-3"><span id="dot" class="h-3 w-3 shrink-0 rounded-full bg-slate-300 dark:bg-slate-600"></span>
<div><p id="relay" class="text-lg font-semibold">Loading...</p><p id="next" class="text-sm text-slate-500 dark:text-slate-400"></p></div></div>
<div><p class="text-xs font-medium uppercase tracking-wide text-slate-500 dark:text-slate-400 mb-2">Mode</p>
<div class="grid grid-cols-3 gap-2">
<button data-m="on">Force ON</button><button data-m="clear">Schedule</button><button data-m="off">Force OFF</button></div></div>
<button id="pause" class="w-full rounded-xl border border-slate-300 py-2.5 font-medium hover:bg-slate-50 dark:border-slate-700 dark:hover:bg-slate-800">Pause 5 minutes</button>
</section>
<form id="sched" class="bg-white rounded-2xl shadow-sm p-5 space-y-5 dark:bg-slate-900">
<h2 class="font-semibold">Watering schedule</h2>
<div id="days" class="grid grid-cols-7 gap-1.5"></div>
<div class="grid grid-cols-2 gap-3">
<label class="text-sm text-slate-600 dark:text-slate-400">Start<input id="start" type="time" required class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800 dark:bg-slate-800 dark:border-slate-700 dark:text-slate-100"></label>
<label class="text-sm text-slate-600 dark:text-slate-400">End<input id="end" type="time" required class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800 dark:bg-slate-800 dark:border-slate-700 dark:text-slate-100"></label></div>
<div class="flex items-center gap-3"><button class="rounded-xl bg-sky-600 text-white px-5 py-2.5 font-medium hover:bg-sky-700">Save schedule</button>
<span id="saved" class="text-sm text-emerald-600 dark:text-emerald-400"></span></div>
</form></div>
</main>
<script>
const $=i=>document.getElementById(i),days=$('days'),N=['Su','Mo','Tu','We','Th','Fr','Sa'];let dirty=0;
function paint(b){b.className='aspect-square rounded-lg text-sm font-semibold '+(b.dataset.on=='1'?'bg-sky-600 text-white':'bg-slate-100 text-slate-500 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-400 dark:hover:bg-slate-700')}
N.forEach((d,i)=>{const b=document.createElement('button');b.type='button';b.textContent=d;b.dataset.on='0';
b.onclick=()=>{b.dataset.on=b.dataset.on=='1'?'0':'1';paint(b);dirty=1};paint(b);days.append(b)});
$('start').oninput=$('end').oninput=()=>dirty=1;
let last={};
// Relay pin choices (board label, GPIO number): only the pins the firmware accepts
[['D1',5],['D2',4],['D5',14],['D6',12],['D7',13]].forEach(([d,g])=>{
const o=document.createElement('option');o.value=g;o.textContent=d+' (GPIO'+g+')';$('cfgPin').append(o)});
function openCfg(){$('cfgName').value=last.name||'';$('cfgPin').value=last.pin;$('cfgHigh').checked=!!last.activeHigh;$('cfg').classList.remove('hidden');$('cfgName').focus()}
$('cfgBtn').onclick=()=>$('cfg').classList.contains('hidden')?openCfg():$('cfg').classList.add('hidden');
$('cfgCancel').onclick=()=>$('cfg').classList.add('hidden');
$('cfg').onsubmit=async e=>{e.preventDefault();
const q=new URLSearchParams({name:$('cfgName').value,pin:$('cfgPin').value,activeHigh:$('cfgHigh').checked?1:0});
if(await call('/config?'+q))$('cfg').classList.add('hidden')};
function render(s){
last=s;$('title').textContent=s.name;document.title=s.name;
$('clock').textContent=s.time;
$('relay').textContent=s.relay?'Watering':'Not watering';
$('dot').className='h-3 w-3 shrink-0 rounded-full '+(s.relay?'bg-emerald-500 animate-pulse':'bg-slate-300 dark:bg-slate-600');
const m=Math.ceil(s.paused/60);
$('next').textContent=s.mode=='on'?'Forced on':s.mode=='off'?'Forced off':s.paused>0?'Paused, '+m+' min left':'Next run: '+s.next;
document.querySelectorAll('[data-m]').forEach(b=>{const a=b.dataset.m==(s.mode=='auto'?'clear':s.mode);
b.className='rounded-xl py-2.5 text-sm font-medium '+(a?'bg-slate-900 text-white dark:bg-sky-600':'bg-slate-100 hover:bg-slate-200 dark:bg-slate-800 dark:hover:bg-slate-700')});
$('pause').textContent=s.paused>0?'Paused ('+m+' min left)':'Pause 5 minutes';
if(!dirty){[...days.children].forEach((b,i)=>{b.dataset.on=s.days[i]?'1':'0';paint(b)});$('start').value=s.start;$('end').value=s.end}}
async function call(u){try{const r=await fetch(u);if(!r.ok)throw 0;const s=await r.json();$('err').classList.add('hidden');render(s);return s}
catch(e){$('err').textContent='Could not reach the controller. Check that it is powered and on Wi-Fi.';$('err').classList.remove('hidden')}}
document.querySelectorAll('[data-m]').forEach(b=>b.onclick=()=>call('/relay1/'+b.dataset.m));
$('pause').onclick=()=>call('/disable');
// Toggle the theme; picking the same theme as the device goes back to following the device
$('theme').onclick=()=>{const d=!document.documentElement.classList.contains('dark');
try{d==dq.matches?localStorage.removeItem('theme'):localStorage.theme=d?'dark':'light'}catch(e){}
document.documentElement.classList.toggle('dark',d)};
$('sched').onsubmit=async e=>{e.preventDefault();const q=new URLSearchParams();
[...days.children].forEach((b,i)=>{if(b.dataset.on=='1')q.append('day',i)});
q.append('startTime',$('start').value);q.append('endTime',$('end').value);
dirty=0;if(await call('/setSchedule?'+q)){$('saved').textContent='Saved';setTimeout(()=>$('saved').textContent='',2000)}};
call('/api/state');setInterval(()=>call('/api/state'),5000);
</script></body></html>)rawliteral";

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

// Decide whether the relay should be on right now
bool shouldWater() {
  if (globalOn) return true;
  if (globalOff) return false;
  if (pauseRemainingMs() > 0) return false;

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
    "{\"mode\":\"%s\",\"relay\":%s,\"paused\":%ld,"
    "\"days\":[%d,%d,%d,%d,%d,%d,%d],"
    "\"start\":\"%02d:%02d\",\"end\":\"%02d:%02d\","
    "\"time\":\"%02d:%02d\",\"next\":\"%s\","
    "\"name\":\"%s\",\"pin\":%d,\"activeHigh\":%s}",
    mode, relayOn ? "true" : "false", pauseRemainingMs() / 1000,
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
  saveSettings();          // stores migrated or default settings in the current layout
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

  server.on("/relay1/on", HTTP_GET, []() {
    globalOff = LOW;
    globalOn = HIGH;
    saveSettings();
    sendState();
  });

  server.on("/relay1/off", HTTP_GET, []() {
    globalOn = LOW;
    globalOff = HIGH;
    saveSettings();
    sendState();
  });

  server.on("/relay1/clear", HTTP_GET, []() {
    globalOn = LOW;
    globalOff = LOW;
    saveSettings();
    sendState();
  });

  server.on("/setSchedule", HTTP_GET, []() {
    for (int i = 0; i < 7; i++) {
      schedule.days[i] = false;
    }
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "day") {
        int day = server.arg(i).toInt();
        if (day >= 0 && day < 7) schedule.days[day] = true;
      }
    }

    if (server.hasArg("startTime")) {
      String startTime = server.arg("startTime");
      int h = startTime.substring(0, 2).toInt();
      int m = startTime.substring(3).toInt();
      if (validTime(h, m)) { schedule.startHour = h; schedule.startMinute = m; }
    }

    if (server.hasArg("endTime")) {
      String endTime = server.arg("endTime");
      int h = endTime.substring(0, 2).toInt();
      int m = endTime.substring(3).toInt();
      if (validTime(h, m)) { schedule.endHour = h; schedule.endMinute = m; }
    }

    saveSettings();
    sendState();
  });

  // Device settings: /config?name=Garden&pin=13&activeHigh=0 (every argument is optional)
  server.on("/config", HTTP_GET, []() {
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

  server.on("/disable", HTTP_GET, []() {
    disableUntil = millis() + 5UL * 60 * 1000; // Disable for 5 minutes
    sendState();
  });

  server.begin();
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  server.handleClient();

  // Update the relay once per second without blocking the web server
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();
    updateRelay();
  }
}
