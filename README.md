# ESP8266 Smart Irrigation System

A WiFi-enabled smart irrigation controller built on the ESP8266 microcontroller. This system allows you to remotely control your irrigation system through a web interface, schedule watering times, and manage your garden or lawn watering automatically.

## Features

✨ **Smart Scheduling**
- Set custom watering schedules for specific days of the week
- Define start and end times for automated irrigation
- View next scheduled run time at a glance

🌐 **Web Interface**
- Responsive page that works on phones (one column) and computers (two columns)
- Dark mode that follows the device theme, with a toggle button that remembers your choice on each device
- Status refreshes every 5 seconds: watering or not, current mode, pause time left, next run
- Manual override controls (Force ON/OFF)
- Schedule configuration via web form

⚙️ **Device Settings**
- Set the device name (shown at the top of the page and in the browser tab), the relay pin, and whether the relay turns on with a HIGH or a LOW signal
- Changes apply right away, without restarting the board

💾 **Settings Survive Restarts**
- The schedule, the mode and the device settings are saved in flash (EEPROM emulation) and restored at boot
- Flash is only written when a value actually changes
- The 5-minute pause is not saved

🔌 **JSON API**
- Every action returns the controller's state as JSON, so it can be scripted or used by other apps

⏰ **Time Management**
- NTP (Network Time Protocol) synchronization at boot for accurate timekeeping
- Configurable timezone and daylight saving offset

🎛️ **Flexible Control Modes**
- **Automatic Mode**: Follow the configured schedule
- **Force ON**: Override schedule to turn irrigation ON
- **Force OFF**: Override schedule to turn irrigation OFF
- **Temporary Disable**: Pause for 5 minutes (useful during maintenance)

## Hardware Requirements

- **ESP8266 Development Board** (NodeMCU, Wemos D1 Mini, or similar)
- **Relay Module** (5V relay recommended)
- **Power Supply** (5V for ESP8266, appropriate voltage for your irrigation system)
- **Solenoid Valve/Pump** (compatible with your relay)
- Jumper wires and breadboard (for prototyping)

### Pin Configuration

| Component | ESP8266 Pin |
|-----------|-------------|
| Relay Control | D7 (GPIO13) by default; D1, D2, D5, D6 or D7 can be chosen in the device settings |
| Built-in LED | `LED_BUILTIN` (lights up while connecting to WiFi) |

By default the relay is driven as **active LOW** (it turns on when the pin is LOW), which matches most relay modules. If yours turns on with a HIGH signal, tick "Relay turns on when the pin is HIGH" in the device settings.

## Software Requirements

### Arduino Libraries
- `ESP8266WiFi` - WiFi connectivity
- `ESP8266WebServer` - Web server functionality
- `EEPROM` - Saving settings to flash (included with the ESP8266 core)
- `ESP32Time` - RTC (Real-Time Clock) management
- `time.h` - Time utilities

The web page uses Tailwind CSS from its CDN, so the browser opening it needs internet access for the styling.

### Installation
Install the required libraries through the Arduino Library Manager or manually download them.

## Getting Started

### 1. Configuration

Open `ESP8266-SmartIrrigationSystem.ino` and modify the following settings:

```cpp
const char *ssid = "REPLACE_WITH_SSID";         // Your WiFi network name
const char *password = "REPLACE_WITH_PASSWORD"; // Your WiFi password

const long gmtOffset_sec = 3600;             // GMT offset in seconds (3600 = GMT+1)
const int daylightOffset_sec = 0;            // Daylight saving offset (0 or 3600)
```

### 2. Upload

1. Connect your ESP8266 board to your computer
2. Select the correct board and port in Arduino IDE
3. Upload the sketch

### 3. Find Your Device

After uploading, open the Serial Monitor (115200 baud) to see the IP address assigned to your ESP8266:

```
Connecting to WiFi...
Connected to WiFi
ESP8266 Web Server's IP address: 192.168.x.x
```

### 4. Access the Web Interface

Open a web browser and navigate to the IP address shown in the Serial Monitor (e.g., `http://192.168.1.100`)

## Usage

### Setting Up a Schedule

1. Navigate to the web interface
2. Select the days of the week you want watering to occur
3. Set the start time (when irrigation begins)
4. Set the end time (when irrigation stops)
5. Click "Save schedule"

**Example Schedule:**
- Days: Monday, Wednesday, Friday
- Start Time: 06:00 AM
- End Time: 06:30 AM

This will water your garden for 30 minutes on Monday, Wednesday, and Friday mornings.

### Manual Control

- **Force ON**: Immediately activates irrigation, ignoring the schedule
- **Force OFF**: Immediately deactivates irrigation, ignoring the schedule
- **Schedule**: Returns to automatic schedule mode
- **Pause 5 minutes**: Temporarily pauses scheduled irrigation for maintenance

### JSON API

| Endpoint | Action |
|---|---|
| `GET /api/state` | Current state |
| `GET /relay1/on` | Force ON |
| `GET /relay1/off` | Force OFF |
| `GET /relay1/clear` | Back to the schedule |
| `GET /disable` | Pause for 5 minutes |
| `GET /setSchedule?day=1&day=3&startTime=06:00&endTime=06:30` | Save the schedule (days: 0 = Sunday … 6 = Saturday) |
| `GET /config?name=Garden&pin=13&activeHigh=0` | Save the device settings (each argument is optional; `pin` is a GPIO number: 5, 4, 14, 12 or 13) |

Every endpoint returns the same JSON:

```json
{"mode": "auto", "relay": false, "paused": 0, "days": [0,1,0,1,0,0,0], "start": "06:00", "end": "06:30", "time": "14:05", "next": "tomorrow at 06:00", "name": "Irrigation ESP", "pin": 13, "activeHigh": false}
```

## Try It Without the Board

`simulator.py` serves the same web page from the `.ino` file and fakes the controller using your computer's clock. It only needs Python 3:

```bash
python simulator.py        # then open http://localhost:8000
python simulator.py 8080   # use another port
```

You can open it from your phone too at `http://<your-pc-ip>:8000` (same Wi-Fi). Nothing is saved: restarting the simulator resets the settings.

## Wiring Diagram

```
ESP8266 (D7) -----> Relay IN     (or the pin chosen in the device settings)
ESP8266 (GND) ----> Relay GND
ESP8266 (3V3/5V) -> Relay VCC

Relay COM --------> Power Supply (+)
Relay NO ----------> Irrigation Valve (+)
Valve (-) ---------> Power Supply (-)
```

⚠️ **Warning**: Ensure your relay can handle the voltage and current requirements of your irrigation system. Use appropriate isolation and safety measures when working with mains voltage.

## Troubleshooting

### ESP8266 Won't Connect to WiFi
- Verify SSID and password are correct
- Check WiFi signal strength
- Ensure your router supports 2.4GHz (ESP8266 doesn't support 5GHz)

### Relay Not Switching
- Check wiring connections
- Verify relay is compatible with 3.3V logic or use a level shifter
- Test relay independently with a simple sketch

### Web Interface Not Loading
- Ping the ESP8266 IP address to verify network connectivity
- Check that port 80 is not blocked by firewall
- Try accessing from the same subnet

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is open-source and available for personal and educational use.

---

**Made with 💧 for smarter irrigation and protecting the planet**
