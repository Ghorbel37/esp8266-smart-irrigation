#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "time.h"
#include <ESP32Time.h>

#define RELAY_PIN D7  // The ESP8266 pin connected to Relay

const char *ssid = "REPLACE_WITH_SSID";     // CHANGE IT
const char *password = "REPLACE_WITH_PASSWORD";  // CHANGE IT

ESP32Time rtc;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;
struct tm timeinfo;

ESP8266WebServer server(80); // Web server on port 80

int RELAY_state = HIGH;
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
#define SETTINGS_VERSION 1

struct Settings {
  uint32_t magic;
  uint8_t  version;
  Schedule schedule;
  uint8_t  globalOn;
  uint8_t  globalOff;
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

void loadSettings() {
  Settings s;
  EEPROM.get(0, s);
  if (s.magic != SETTINGS_MAGIC || s.version != SETTINGS_VERSION ||
      s.checksum != settingsChecksum(s) ||
      !validTime(s.schedule.startHour, s.schedule.startMinute) ||
      !validTime(s.schedule.endHour, s.schedule.endMinute)) {
    Serial.println("No saved settings, using defaults");
    return;  // keep the zero-initialized defaults
  }
  schedule  = s.schedule;
  globalOn  = s.globalOn ? HIGH : LOW;
  globalOff = s.globalOff ? HIGH : LOW;
  Serial.println("Settings loaded from flash");
}

void saveSettings() {
  Settings s;
  memset(&s, 0, sizeof(s));  // zero padding bytes so the checksum is stable
  s.magic     = SETTINGS_MAGIC;
  s.version   = SETTINGS_VERSION;
  s.schedule  = schedule;
  s.globalOn  = globalOn == HIGH;
  s.globalOff = globalOff == HIGH;
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
<title>Irrigation</title><link rel="icon" href="data:,">
<script src="https://cdn.tailwindcss.com"></script></head>
<body class="bg-slate-100 text-slate-800 min-h-screen">
<main class="max-w-4xl mx-auto p-4 sm:p-6 space-y-4">
<header class="flex items-center justify-between gap-2">
<h1 class="text-xl sm:text-2xl font-bold">Irrigation System 3000</h1>
<span id="clock" class="text-sm text-slate-500 tabular-nums">--:--</span></header>
<p id="err" class="hidden rounded-xl bg-red-50 text-red-700 text-sm p-3"></p>
<div class="grid gap-4 md:grid-cols-2">
<section class="bg-white rounded-2xl shadow-sm p-5 space-y-5">
<div class="flex items-center gap-3"><span id="dot" class="h-3 w-3 shrink-0 rounded-full bg-slate-300"></span>
<div><p id="relay" class="text-lg font-semibold">Loading...</p><p id="next" class="text-sm text-slate-500"></p></div></div>
<div><p class="text-xs font-medium uppercase tracking-wide text-slate-500 mb-2">Mode</p>
<div class="grid grid-cols-3 gap-2">
<button data-m="on">Force ON</button><button data-m="clear">Schedule</button><button data-m="off">Force OFF</button></div></div>
<button id="pause" class="w-full rounded-xl border border-slate-300 py-2.5 font-medium hover:bg-slate-50">Pause 5 minutes</button>
</section>
<form id="sched" class="bg-white rounded-2xl shadow-sm p-5 space-y-5">
<h2 class="font-semibold">Watering schedule</h2>
<div id="days" class="grid grid-cols-7 gap-1.5"></div>
<div class="grid grid-cols-2 gap-3">
<label class="text-sm text-slate-600">Start<input id="start" type="time" required class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800"></label>
<label class="text-sm text-slate-600">End<input id="end" type="time" required class="mt-1 w-full rounded-lg border border-slate-300 px-3 py-2 text-slate-800"></label></div>
<div class="flex items-center gap-3"><button class="rounded-xl bg-sky-600 text-white px-5 py-2.5 font-medium hover:bg-sky-700">Save schedule</button>
<span id="saved" class="text-sm text-emerald-600"></span></div>
</form></div>
</main>
<script>
const $=i=>document.getElementById(i),days=$('days'),N=['Su','Mo','Tu','We','Th','Fr','Sa'];let dirty=0;
function paint(b){b.className='aspect-square rounded-lg text-sm font-semibold '+(b.dataset.on=='1'?'bg-sky-600 text-white':'bg-slate-100 text-slate-500 hover:bg-slate-200')}
N.forEach((d,i)=>{const b=document.createElement('button');b.type='button';b.textContent=d;b.dataset.on='0';
b.onclick=()=>{b.dataset.on=b.dataset.on=='1'?'0':'1';paint(b);dirty=1};paint(b);days.append(b)});
$('start').oninput=$('end').oninput=()=>dirty=1;
function render(s){
$('clock').textContent=s.time;
$('relay').textContent=s.relay?'Watering':'Not watering';
$('dot').className='h-3 w-3 shrink-0 rounded-full '+(s.relay?'bg-emerald-500 animate-pulse':'bg-slate-300');
const m=Math.ceil(s.paused/60);
$('next').textContent=s.mode=='on'?'Forced on':s.mode=='off'?'Forced off':s.paused>0?'Paused, '+m+' min left':'Next run: '+s.next;
document.querySelectorAll('[data-m]').forEach(b=>{const a=b.dataset.m==(s.mode=='auto'?'clear':s.mode);
b.className='rounded-xl py-2.5 text-sm font-medium '+(a?'bg-slate-900 text-white':'bg-slate-100 hover:bg-slate-200')});
$('pause').textContent=s.paused>0?'Paused ('+m+' min left)':'Pause 5 minutes';
if(!dirty){[...days.children].forEach((b,i)=>{b.dataset.on=s.days[i]?'1':'0';paint(b)});$('start').value=s.start;$('end').value=s.end}}
async function call(u){try{const r=await fetch(u);if(!r.ok)throw 0;const s=await r.json();$('err').classList.add('hidden');render(s);return s}
catch(e){$('err').textContent='Could not reach the controller. Check that it is powered and on Wi-Fi.';$('err').classList.remove('hidden')}}
document.querySelectorAll('[data-m]').forEach(b=>b.onclick=()=>call('/relay1/'+b.dataset.m));
$('pause').onclick=()=>call('/disable');
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

  int h = rtc.getHour(true);
  int m = rtc.getMinute();
  bool isScheduledDay = schedule.days[rtc.getDayofWeek()];
  bool isWithinTime = (h > schedule.startHour || (h == schedule.startHour && m >= schedule.startMinute)) &&
                      (h < schedule.endHour || (h == schedule.endHour && m < schedule.endMinute));
  return isScheduledDay && isWithinTime;
}

void updateRelay() {
  RELAY_state = shouldWater() ? LOW : HIGH;  // relay is active LOW
  digitalWrite(RELAY_PIN, RELAY_state);
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

  char json[256];
  snprintf(json, sizeof(json),
    "{\"mode\":\"%s\",\"relay\":%s,\"paused\":%ld,"
    "\"days\":[%d,%d,%d,%d,%d,%d,%d],"
    "\"start\":\"%02d:%02d\",\"end\":\"%02d:%02d\","
    "\"time\":\"%02d:%02d\",\"next\":\"%s\"}",
    mode, RELAY_state == LOW ? "true" : "false", pauseRemainingMs() / 1000,
    schedule.days[0], schedule.days[1], schedule.days[2], schedule.days[3],
    schedule.days[4], schedule.days[5], schedule.days[6],
    schedule.startHour, schedule.startMinute, schedule.endHour, schedule.endMinute,
    rtc.getHour(true), rtc.getMinute(), next);
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_state);
  pinMode(RELAY_PIN, OUTPUT);

  EEPROM.begin(sizeof(Settings));
  loadSettings();

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
