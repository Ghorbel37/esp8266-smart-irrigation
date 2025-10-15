#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
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

void initLocalTime(const char* ntpServer, long gmtOffset_sec, int daylightOffset_sec) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
    } else {
        rtc.setTimeStruct(timeinfo);
        Serial.println("Initialized time");
    }
}

String getHTML() {
  String html = "<!DOCTYPE HTML>";
  html += "<html>";
  html += "<head>";
  html += "<link rel='icon' href='https://upload.wikimedia.org/wikipedia/commons/1/1c/Circle-icons-water.svg'>";
  html += "</head>";
  html += "<body>";
  html += "<h1>ESP8266 Irrigation System 3000</h1>";
  html += "<h3>Relay state:";
  if (globalOn == HIGH)
    html += "<span style='color: green;'>FORCED ON";
  else if(globalOff == HIGH)
    html += "<span style='color: red;'>FORCED OFF";
  else
    html += "<span style='color: blue;'>USING TIMER";

  html += "</span></h3>";

  html += "<h3>Relay powered:";
  if (RELAY_state == HIGH)
    html += "<span style='color: red;'>OFF";
  else
    html += "<span style='color: green;'>ON";
  
  html += "</span></h3>";
  html += "<a href='/relay1/on'>Turn ON</a>";
  html += "<br><br>";
  html += "<a href='/relay1/off'>Turn OFF</a>";
  html += "<br><br>";
  html += "<a href='/relay1/clear'>Turn with timer</a>";
  html += "<br><br>";

  // Form for schedule
  html += "<form action='/setSchedule' method='GET'>";
  html += "Select Days:<br>";
  html += "<input type='checkbox' name='day' value='0'" + String(schedule.days[0] ? " checked" : "") + "> Sunday<br>";
  html += "<input type='checkbox' name='day' value='1'" + String(schedule.days[1] ? " checked" : "") + "> Monday<br>";
  html += "<input type='checkbox' name='day' value='2'" + String(schedule.days[2] ? " checked" : "") + "> Tuesday<br>";
  html += "<input type='checkbox' name='day' value='3'" + String(schedule.days[3] ? " checked" : "") + "> Wednesday<br>";
  html += "<input type='checkbox' name='day' value='4'" + String(schedule.days[4] ? " checked" : "") + "> Thursday<br>";
  html += "<input type='checkbox' name='day' value='5'" + String(schedule.days[5] ? " checked" : "") + "> Friday<br>";
  html += "<input type='checkbox' name='day' value='6'" + String(schedule.days[6] ? " checked" : "") + "> Saturday<br><br>";
  html += "Start Time:<br>";
  html += "<input type='time' name='startTime' value='" + String(schedule.startHour < 10 ? "0" : "") + String(schedule.startHour) + ":" + String(schedule.startMinute < 10 ? "0" : "") + String(schedule.startMinute) + "'><br><br>";
  html += "End Time:<br>";
  html += "<input type='time' name='endTime' value='" + String(schedule.endHour < 10 ? "0" : "") + String(schedule.endHour) + ":" + String(schedule.endMinute < 10 ? "0" : "") + String(schedule.endMinute) + "'><br><br>";
  html += "<input type='submit' value='Set Schedule'>";
  html += "</form>";
  
  html += "<br><br>";

  // Disable button
  html += "<a href='/disable'>Disable for 5 Minutes</a>";
  
  html += "<br><br>";

  // Fetch time button
  html += "<a href='/fetchTime'>Fetch NTP Time and Calculate Difference</a>";
  html += "<br><br>";

  // Display next run time
  html += "<p>Next Run Time: " + getNextRunTime() + "</p>";

  html += "</body>";
  html += "</html>";

  return html;
}

String getNextRunTime() {
  int currentHour = rtc.getHour(true);
  int currentMinute = rtc.getMinute();
  int currentSecond = rtc.getSecond();
  int currentDay = rtc.getDayofWeek();

  for (int i = 0; i < 7; i++) {
    int day = (currentDay + i) % 7;
    if (schedule.days[day]) {
      if (i == 0 && (currentHour < schedule.startHour || (currentHour == schedule.startHour && currentMinute < schedule.startMinute))) {
        return "Today at " + String(schedule.startHour) + ":" + String(schedule.startMinute < 10 ? "0" : "") + String(schedule.startMinute);
      } else if (i > 0) {
        return "In " + String(i) + " days at " + String(schedule.startHour) + ":" + String(schedule.startMinute < 10 ? "0" : "") + String(schedule.startMinute);
      }
    }
  }

  return "No scheduled run time";
}

void setup() {
  Serial.begin(115200);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_state);
  pinMode(RELAY_PIN, OUTPUT);

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
    server.send(200, "text/html", getHTML());
  });

  server.on("/relay1/on", HTTP_GET, []() {
    globalOff = LOW;
    globalOn = HIGH;
    // digitalWrite(RELAY_PIN, RELAY_state);
    server.send(200, "text/html", getHTML());
  });

  server.on("/relay1/off", HTTP_GET, []() {
    globalOn = LOW;
    globalOff = HIGH;
    // digitalWrite(RELAY_PIN, RELAY_state);
    server.send(200, "text/html", getHTML());
  });

    server.on("/relay1/clear", HTTP_GET, []() {
    globalOn = LOW;
    globalOff = LOW;
    // digitalWrite(RELAY_PIN, RELAY_state);
    server.send(200, "text/html", getHTML());
  });

  server.on("/setSchedule", HTTP_GET, []() {
    for (int i = 0; i < 7; i++) {
      schedule.days[i] = false;
    }
    for (uint8_t i = 0; i < server.args(); i++) {
      if (server.argName(i) == "day") {
        int day = server.arg(i).toInt();
        schedule.days[day] = true;
      }
    }

    if (server.hasArg("startTime")) {
      String startTime = server.arg("startTime");
      schedule.startHour = startTime.substring(0, 2).toInt();
      schedule.startMinute = startTime.substring(3).toInt();
    }

    if (server.hasArg("endTime")) {
      String endTime = server.arg("endTime");
      schedule.endHour = endTime.substring(0, 2).toInt();
      schedule.endMinute = endTime.substring(3).toInt();
    }

    server.send(200, "text/html", getHTML());
  });

  server.on("/disable", HTTP_GET, []() {
    disableUntil = millis() + 5 * 60 * 1000; // Disable for 5 minutes
    server.send(200, "text/html", getHTML());
  });

  server.on("/fetchTime", HTTP_GET, []() {
    initLocalTime(ntpServer, gmtOffset_sec, daylightOffset_sec);

    // Get the current RTC time
    int rtcHour = rtc.getHour(true);
    int rtcMinute = rtc.getMinute();
    int rtcSecond = rtc.getSecond();

    // Get the current NTP time
    if (getLocalTime(&timeinfo)) {
      int ntpHour = timeinfo.tm_hour;
      int ntpMinute = timeinfo.tm_min;
      int ntpSecond = timeinfo.tm_sec;

      // Calculate the difference in seconds
      int rtcTotalSeconds = rtcHour * 3600 + rtcMinute * 60 + rtcSecond;
      int ntpTotalSeconds = ntpHour * 3600 + ntpMinute * 60 + ntpSecond;
      int diffSeconds = ntpTotalSeconds - rtcTotalSeconds;
      if (diffSeconds < 0) diffSeconds += 86400; // Adjust for negative differences

      // Convert difference to hours, minutes, and seconds
      int diffHours = diffSeconds / 3600;
      diffSeconds %= 3600;
      int diffMinutes = diffSeconds / 60;
      diffSeconds %= 60;

      // Generate HTML to display the times and difference
      String html = "<!DOCTYPE HTML>";
      html += "<html>";
      html += "<head>";
      html += "<link rel='icon' href='data:,'>";
      html += "</head>";
      html += "<body>";
      html += "<h1>Time Difference</h1>";
      html += "<p>RTC Time: " + String(rtcHour) + ":" + String(rtcMinute < 10 ? "0" : "") + String(rtcMinute) + ":" + String(rtcSecond < 10 ? "0" : "") + String(rtcSecond) + "</p>";
      html += "<p>NTP Time: " + String(ntpHour) + ":" + String(ntpMinute < 10 ? "0" : "") + String(ntpMinute) + ":" + String(ntpSecond < 10 ? "0" : "") + String(ntpSecond) + "</p>";
      html += "<p>Difference: " + String(diffHours) + " hours " + String(diffMinutes) + " minutes " + String(diffSeconds) + " seconds</p>";
      html += "<a href='/'>Back to Home</a>";
      html += "</body>";
      html += "</html>";

      server.send(200, "text/html", html);
    } else {
      server.send(500, "text/html", "Failed to obtain NTP time");
    }
  });

  server.begin();
  digitalWrite(LED_BUILTIN, HIGH);
}

void loop() {
  server.handleClient();

  int currentHour = rtc.getHour(true);
  int currentMinute = rtc.getMinute();
  int currentSecond = rtc.getSecond();
  int currentDay = rtc.getDayofWeek();

  bool isScheduledDay = schedule.days[currentDay];
  bool isWithinTime = (currentHour > schedule.startHour || (currentHour == schedule.startHour && currentMinute >= schedule.startMinute)) &&
                      (currentHour < schedule.endHour || (currentHour == schedule.endHour && currentMinute < schedule.endMinute));

  if(globalOn){
    RELAY_state = LOW; //Enabled
  } else if(globalOff){
    RELAY_state = HIGH; //Disabled
  } else{                    
  if (millis() < disableUntil) {
    RELAY_state = HIGH; //Disabled
  } else {
    if (isScheduledDay && isWithinTime) {
      RELAY_state = LOW; //Enabled
    } else {
      RELAY_state = HIGH;
    }
  }}
  digitalWrite(RELAY_PIN, RELAY_state);
  delay(1000);
}
