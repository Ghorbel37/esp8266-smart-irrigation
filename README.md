# ESP8266 Smart Irrigation System

A WiFi-enabled smart irrigation controller built on the ESP8266 microcontroller. This system allows you to remotely control your irrigation system through a web interface, schedule watering times, and manage your garden or lawn watering automatically.

## Features

✨ **Smart Scheduling**
- Set custom watering schedules for specific days of the week
- Define start and end times for automated irrigation
- View next scheduled run time at a glance

🌐 **Web Interface**
- Clean, accessible web UI for remote control
- Real-time status monitoring
- Manual override controls (Force ON/OFF)
- Schedule configuration via web form

⏰ **Time Management**
- NTP (Network Time Protocol) synchronization for accurate timekeeping
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
| Relay Control | D7 (GPIO13) |
| Built-in LED | D0 (GPIO16) |

## Software Requirements

### Arduino Libraries
- `ESP8266WiFi` - WiFi connectivity
- `ESP8266WebServer` - Web server functionality
- `ESP32Time` - RTC (Real-Time Clock) management
- `time.h` - Time utilities

### Installation
Install the required libraries through the Arduino Library Manager or manually download them.

## Getting Started

### 1. Configuration

Open `ESP8266-SmartIrrigationSystem.ino` and modify the following settings:

```cpp
const char *ssid = "your-wifi-ssid";         // Your WiFi network name
const char *password = "your-wifi-password"; // Your WiFi password

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
5. Click "Set Schedule"

**Example Schedule:**
- Days: Monday, Wednesday, Friday
- Start Time: 06:00 AM
- End Time: 06:30 AM

This will water your garden for 30 minutes on Monday, Wednesday, and Friday mornings.

### Manual Control

- **Turn ON**: Immediately activates irrigation, ignoring the schedule
- **Turn OFF**: Immediately deactivates irrigation, ignoring the schedule
- **Turn with Timer**: Returns to automatic schedule mode
- **Disable for 5 Minutes**: Temporarily pauses all irrigation for maintenance

### Time Synchronization

Click "Fetch NTP Time and Calculate Difference" to:
- Update the system time from NTP servers
- View the difference between RTC and NTP time
- Ensure accurate scheduling

## Wiring Diagram

```
ESP8266 (D7) -----> Relay IN
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
