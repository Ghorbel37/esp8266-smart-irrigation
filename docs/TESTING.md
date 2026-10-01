# Testing

This project is tested at five levels. The first four run on a computer with one command; the last one is a manual checklist on a real ESP8266.

| Suite | What it checks | Needs | Runs |
|---|---|---|---|
| **minify** | The minifier keeps the page working (strings, regular expressions, spaces between words) and `index_html.h` matches `web/index.html` | Python 3.10+ | always |
| **simulator** | The simulator's API behaves like the firmware: routes, GET/POST, validation, JSON | Python 3.10+ | always |
| **firmware** | The real sketch, compiled on the computer against fake Arduino libraries: schedule, modes, Force ON cutoff, clock, saved settings, JSON, served page | a C++17 compiler (g++ or clang++) | always |
| **browser** | The web page clicked through in a headless browser, against the simulator | Playwright | `--browser` |
| **compile** | The sketch builds for a real ESP8266 with no warnings | Arduino IDE 2 or arduino-cli, with the esp8266 core and ESP32Time | `--compile` |
| **hardware** | The board, relay, Wi-Fi, NTP and flash really work | an ESP8266 and a relay module | by hand, [checklist below](#hardware-checklist) |

## Running the Tests

From the project folder:

```bash
python tests/run_tests.py              # minify, simulator and firmware
python tests/run_tests.py --browser    # + browser
python tests/run_tests.py --compile    # + compile for the ESP8266
python tests/run_tests.py --all        # everything
```

Each test is listed with `ok` or `FAIL`, then a summary:

```
== Summary (23.2 s)
  PASSED   minify     14 passed
  PASSED   simulator  9 passed
  PASSED   firmware   14 passed
  PASSED   browser    9 passed
  PASSED   compile    1 passed

All suites passed
```

The exit code is 1 when a test failed, so it can be used in scripts or CI. A suite whose tool isn't installed is shown as `SKIPPED` with what's missing; that doesn't fail the run.

`--compile` builds for a NodeMCU by default. Use `--fqbn` for another board, for example `--fqbn esp8266:esp8266:d1_mini`.

## When to Run What

| Change | Run |
|---|---|
| Any change | `python tests/run_tests.py` |
| `web/index.html` | `python minify.py`, then `python tests/run_tests.py --browser` |
| The sketch | `python tests/run_tests.py --compile` |
| Before tagging a release | `python tests/run_tests.py --all`: every suite must be `PASSED`, none `SKIPPED`. Then the [hardware checklist](#hardware-checklist) when a board is available, and say in the release notes whether it was done |

## Installing the Tools

**C++ compiler** for the firmware tests:

- Windows: [w64devkit](https://github.com/skeeto/w64devkit/releases), [MSYS2](https://www.msys2.org/) (`pacman -S mingw-w64-ucrt-x86_64-gcc`) or the g++ that comes with [Strawberry Perl](https://strawberryperl.com/). Add its `bin` folder to `PATH`
- macOS: `xcode-select --install`
- Debian/Ubuntu: `sudo apt install g++`

The runner uses the compiler in the `CXX` environment variable, otherwise `g++`, otherwise `clang++`.

**Playwright** for the browser tests:

```bash
pip install playwright
playwright install chromium   # optional: without it, the Chrome or Edge already installed is used
```

The browser needs internet access to load the Tailwind styles, like the real page.

**Arduino** for the compile test: follow [Installation](../README.md#installation) in the README (esp8266 core and ESP32Time). The runner finds `arduino-cli` on `PATH`, or the one bundled with the Arduino IDE 2.

## What Each Suite Covers

### minify ([tests/test_minify.py](../tests/test_minify.py))

- JavaScript: comments and spaces removed; one space kept between words (`const x`) and between signs (`a - -b`); strings, template literals and regular expressions untouched; `/` as a division; errors on unterminated strings, comments and regular expressions
- HTML: comments removed, line breaks next to tags removed, other spaces kept as one; scripts minified as JavaScript; the C++ raw-string delimiter refused
- The real page: `index_html.h` is up to date, no comments or line breaks left, complete document with its 3 scripts, under 16 KB

### simulator ([tests/test_simulator.py](../tests/test_simulator.py))

Starts the simulator on a free port and checks: `/` serves the minified page; `/api/state` has every field; actions are POST only (GET returns 404 and changes nothing); modes and pause; Force ON duration (default, invalid values, 24 h cap) and its cutoff; schedule days and `HH:MM` validation; runs across midnight; device name cleaning, pin and signal level.

### firmware ([tests/firmware/](../tests/firmware/))

`test_firmware.cpp` includes the real `esp8266-smart-irrigation.ino` and compiles it on the computer. The headers in `stubs/` replace the Arduino libraries:

- **Pins**: `digitalWrite` and `pinMode` are recorded, so tests check the relay pin's level and mode
- **Flash**: `EEPROM` is a byte array that starts erased (all `0xFF`) and counts commits, so tests check what is saved and that flash isn't written needlessly
- **Time**: `millis()`, `time()` and the clock's day/hour/minute are variables the tests set
- **Web server**: routes are recorded with their method; tests call them directly and read the response

Each test runs in its own process, so it starts from a new board:

| Test | Checks |
|---|---|
| `fresh` | A new board starts with the defaults and the relay off on D7 |
| `settings` | Device name cleaning (quotes, control characters, UTF-8 cut, empty), allowed pins only, active HIGH/LOW, applied right away, reloaded after a restart, no flash write when nothing changed |
| `otherdata` | Saved data from another layout or with a bad checksum is ignored and the defaults are used |
| `schedule` | Chosen days, start/end, runs across midnight (including Saturday → Sunday), start = end never waters, invalid days and times ignored |
| `nextrun` | "Next run" text: none, today, tomorrow, a weekday, same day next week |
| `modes` | Force OFF (saved to flash), back to schedule, 5-minute pause |
| `forceon` | 60-minute default, custom duration, invalid values, 24 h cap, turns off by itself, never written to flash, works when `millis()` wraps around |
| `clock` | The schedule waits until NTP has set the clock; Force ON still works |
| `methods` | `/` and `/api/state` are GET, every action is POST |
| `json` | The longest possible state fits the 384-byte JSON buffer |
| `wifi` | Without Wi-Fi the board still starts; the connection is retried from scratch every 30 s, and the count restarts after each outage |
| `wifiwatering` | A Wi-Fi outage doesn't stop a scheduled run |
| `nokeepalive` | Every answer closes its connection, so one browser can't block the others |
| `page` | `/` serves exactly `INDEX_HTML` from `index_html.h` |

To run one test by hand:

```bash
g++ -std=c++17 -Wall -Itests/firmware/stubs tests/firmware/test_firmware.cpp -o test_firmware
./test_firmware --list
./test_firmware forceon
```

### browser ([tests/test_browser.py](../tests/test_browser.py))

Opens the page at phone size and checks: first load; the mode buttons and pause (and the POST requests they send); the Force ON duration is sent, remembered after a reload, and 0 is refused; saving the schedule; device settings (pin list, saved, title updated); the error message when the controller doesn't answer; a controller that stops answering (the request is dropped after 4 s, requests never pile up, and the page recovers by itself); the dark mode toggle and its memory. Any JavaScript error fails the test.

### compile

Builds the sketch with `arduino-cli compile --warnings all` into a temporary folder and fails on any error or warning. It prints the flash and RAM use.

## Adding a Test

- **Firmware**: add a block to `tests/firmware/test_firmware.cpp`; the runner finds it by itself:

  ```cpp
  TEST(name, "What it checks") {
    setup();
    req("/relay1/on", {{"minutes", "5"}});
    CHECK(relayOn);
  }
  ```

- **Python**: add a `test_...` method to a class in `tests/test_*.py` (standard `unittest`).

When the simulator and the firmware should behave the same, test both.

## Hardware Checklist

Things only a real board can show. Do them after uploading, with the Serial Monitor open at 115200 baud and the relay connected (a valve isn't needed: the relay clicks and its LED lights).

| # | Step | Expected |
|---|---|---|
| 1 | Erase the flash once (**Tools > Erase Flash: All Flash Contents**), upload | Serial: `No saved settings, using defaults`, `Settings saved`, dots, `Connected to WiFi`, `Initialized time`, the IP address. The relay stays off during boot |
| 2 | Open the IP address on a phone | The page loads with its styles, the clock shows the right local time |
| 3 | Force ON, then Force OFF, then Schedule | The relay clicks on, then off. The page follows within 5 seconds on a second phone |
| 4 | Schedule today from the next minute to the one after, save | The relay turns on at the start minute and off at the end minute |
| 5 | Force ON for 1 minute | The relay turns off by itself after a minute, the mode goes back to Schedule |
| 6 | Pause 5 minutes during a scheduled run | The relay turns off, and on again 5 minutes later if the run isn't over |
| 7 | Device settings: rename, then switch the relay signal to HIGH | The title changes; the relay's on/off are inverted right away |
| 8 | Unplug the board and plug it back in | Serial: `Settings loaded from flash`. Name, schedule and Force OFF are kept; Force ON and the pause are not |
| 9 | Upload the sketch again (Erase Flash back to **Only Sketch**) | Settings are still there |
| 10 | Restart the board with the router's internet unplugged (Wi-Fi still on) | The page says "waiting for the clock"; Force ON/OFF work; the schedule starts once internet is back (the ESP8266 retries NTP by itself) |
| 11 | Leave it running for a day | Still reachable, the clock is still right, scheduled runs happened |

Write down the date, the board and the results, and mention them in the release notes.
