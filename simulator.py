"""Try the irrigation web interface on your PC, without the ESP8266.

Serves the same page that is stored in ESP8266-SmartIrrigationSystem.ino and fakes
the controller's API (schedule, modes, pause, relay state) using your PC's clock.

    python simulator.py            # then open http://localhost:8000
    python simulator.py 8080       # use another port

Open it from your phone too: http://<your-pc-ip>:8000 (same Wi-Fi).
Needs internet access in the browser for the Tailwind styles, like the real device.
Nothing is saved: restarting the script resets the settings.
"""
import json
import re
import sys
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SKETCH = Path(__file__).with_name("ESP8266-SmartIrrigationSystem.ino")
DAY_NAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
DEFAULT_NAME = "Irrigation ESP"
RELAY_PINS = {5, 4, 14, 12, 13}  # D1, D2, D5, D6, D7 as GPIO numbers, like the firmware
NAME_SIZE = 32

state = {
    "global_on": False,
    "global_off": False,
    "pause_until": 0.0,
    "days": [False] * 7,
    "start": (0, 0),
    "end": (0, 0),
    "name": DEFAULT_NAME,
    "pin": 13,  # D7
    "active_high": False,
}


def clean_name(value):
    """Same rules as setDeviceName() in the firmware."""
    name = "".join(c for c in value if ord(c) >= 0x20 and c not in '"\\').strip()
    while len(name.encode()) > NAME_SIZE - 1:
        name = name[:-1]
    return name or DEFAULT_NAME


def load_page():
    match = re.search(r'R"rawliteral\((.*?)\)rawliteral"', SKETCH.read_text(encoding="utf-8"), re.S)
    if not match:
        sys.exit(f"Could not find the web page in {SKETCH.name}")
    return match.group(1).encode()


def now():
    t = datetime.now()
    return t.hour, t.minute, (t.weekday() + 1) % 7  # Sunday = 0, like the ESP


def paused_seconds():
    return max(0, int(state["pause_until"] - time.time()))


def should_water():
    if state["global_on"]:
        return True
    if state["global_off"] or paused_seconds() > 0:
        return False
    h, m, day = now()
    start, end = state["start"], state["end"]
    if start < end:
        return state["days"][day] and start <= (h, m) < end
    if start == end:
        return False
    # Crosses midnight: the part after midnight belongs to the previous day's run
    yesterday = (day + 6) % 7
    return (state["days"][day] and (h, m) >= start) or (state["days"][yesterday] and (h, m) < end)


def next_run():
    h, m, today = now()
    sh, sm = state["start"]
    for i in range(8):
        day = (today + i) % 7
        if not state["days"][day] or (i == 0 and (h, m) >= (sh, sm)):
            continue
        when = "today" if i == 0 else "tomorrow" if i == 1 else DAY_NAMES[day]
        return f"{when} at {sh:02d}:{sm:02d}"
    return "none scheduled"


def state_json():
    h, m, _ = now()
    mode = "on" if state["global_on"] else "off" if state["global_off"] else "auto"
    return {
        "mode": mode,
        "relay": should_water(),
        "paused": paused_seconds(),
        "days": [int(d) for d in state["days"]],
        "start": "%02d:%02d" % state["start"],
        "end": "%02d:%02d" % state["end"],
        "time": f"{h:02d}:{m:02d}",
        "next": next_run(),
        "name": state["name"],
        "pin": state["pin"],
        "activeHigh": state["active_high"],
    }


def parse_time(value):
    try:
        h, m = int(value[:2]), int(value[3:5])
    except ValueError:
        return None
    return (h, m) if 0 <= h < 24 and 0 <= m < 60 else None


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlparse(self.path)
        args = parse_qs(url.query)

        if url.path == "/":
            return self.reply(200, "text/html", load_page())
        if url.path == "/relay1/on":
            state.update(global_on=True, global_off=False)
        elif url.path == "/relay1/off":
            state.update(global_on=False, global_off=True)
        elif url.path == "/relay1/clear":
            state.update(global_on=False, global_off=False)
        elif url.path == "/disable":
            state["pause_until"] = time.time() + 5 * 60
        elif url.path == "/setSchedule":
            days = {int(d) for d in args.get("day", []) if d.isdigit()}
            state["days"] = [i in days for i in range(7)]
            for key, field in (("startTime", "start"), ("endTime", "end")):
                parsed = parse_time(args.get(key, [""])[0])
                if parsed:
                    state[field] = parsed
        elif url.path == "/config":
            if "name" in args:
                state["name"] = clean_name(args["name"][0])
            pin = args.get("pin", [""])[0]
            if pin.isdigit() and int(pin) in RELAY_PINS:
                state["pin"] = int(pin)
            if "activeHigh" in args:
                state["active_high"] = args["activeHigh"][0] in ("1", "true")
        elif url.path != "/api/state":
            return self.reply(404, "text/plain", b"Not found")

        self.reply(200, "application/json", json.dumps(state_json()).encode())

    def reply(self, code, content_type, body):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        if "/api/state" not in self.path:  # hide the 5-second polling
            print(f"{self.command} {self.path}")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    load_page()  # fail early if the page can't be found
    print(f"Simulator running: http://localhost:{port}  (Ctrl+C to stop)")
    try:
        ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
    except KeyboardInterrupt:
        pass
