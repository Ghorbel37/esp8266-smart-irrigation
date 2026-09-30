"""Tests for simulator.py: its API behaves like the firmware's (routes, methods, validation, JSON).

    python -m unittest tests/test_simulator.py -v

Starts the simulator on a free port for the duration of the tests.
"""
import copy
import json
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import minify  # noqa: E402
import simulator  # noqa: E402

INITIAL_STATE = copy.deepcopy(simulator.state)
STATE_KEYS = {"mode", "relay", "paused", "onLeft", "days", "start", "end", "time", "next", "name", "pin", "activeHigh"}


class Simulator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        simulator.Handler.log_message = lambda *args: None  # keep the test output clean
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), simulator.Handler)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        simulator.state.clear()
        simulator.state.update(copy.deepcopy(INITIAL_STATE))

    def get(self, path):
        try:
            with urllib.request.urlopen(self.base + path) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.read()

    def post(self, path, body=""):
        with urllib.request.urlopen(urllib.request.Request(self.base + path, data=body.encode())) as r:
            self.assertEqual(r.status, 200)
            return json.loads(r.read())

    def test_page_is_the_minified_page(self):
        status, body = self.get("/")
        self.assertEqual(status, 200)
        self.assertEqual(body.decode(), minify.build()[0])

    def test_state(self):
        status, body = self.get("/api/state")
        self.assertEqual(status, 200)
        state = json.loads(body)
        self.assertEqual(set(state), STATE_KEYS)
        self.assertEqual(state["mode"], "auto")
        self.assertEqual(state["name"], "Irrigation ESP")

    def test_actions_are_post_only(self):
        for path in ("/relay1/on", "/relay1/off", "/relay1/clear", "/disable", "/setSchedule", "/config?name=x"):
            with self.subTest(path=path):
                self.assertEqual(self.get(path)[0], 404)
        self.assertEqual(json.loads(self.get("/api/state")[1])["mode"], "auto")  # nothing changed

    def test_modes(self):
        self.assertEqual(self.post("/relay1/off")["mode"], "off")
        self.assertEqual(self.post("/relay1/clear")["mode"], "auto")
        self.assertIn(self.post("/disable")["paused"], (299, 300))  # rounded down after a few ms

    def test_force_on_duration(self):
        for body, seconds in (("", 3600), ("minutes=5", 300), ("minutes=0", 3600), ("minutes=abc", 3600),
                              ("minutes=5000", 1440 * 60)):
            with self.subTest(body=body):
                state = self.post("/relay1/on", body)
                self.assertEqual(state["mode"], "on")
                self.assertIn(state["onLeft"], (seconds, seconds - 1))

    def test_force_on_runs_out(self):
        now = time.time()
        with mock.patch.object(simulator.time, "time", return_value=now):
            self.post("/relay1/on", "minutes=1")
        with mock.patch.object(simulator.time, "time", return_value=now + 61):
            state = simulator.state_json()
        self.assertEqual((state["mode"], state["onLeft"]), ("auto", 0))

    def test_schedule(self):
        state = self.post("/setSchedule", "day=1&day=3&startTime=23:00&endTime=01:00")
        self.assertEqual((state["days"], state["start"], state["end"]), ([0, 1, 0, 1, 0, 0, 0], "23:00", "01:00"))
        for bad in ("9:00", "24:00", "12:60", "ab:cd", "12:3x", "12:30:00", "１２:００"):
            with self.subTest(time=bad):
                self.assertEqual(self.post("/setSchedule", "startTime=" + urllib.request.quote(bad))["start"], "23:00")

    def test_schedule_across_midnight(self):
        self.post("/setSchedule", "day=1&startTime=23:00&endTime=01:00")  # Monday night
        for when, expected in (((23, 0, 1), True), ((0, 30, 2), True), ((1, 0, 2), False),
                               ((0, 30, 1), False), ((22, 59, 1), False)):
            with self.subTest(when=when), mock.patch.object(simulator, "now", return_value=when):
                self.assertEqual(simulator.should_water(), expected)

    def test_config(self):
        state = self.post("/config", "name=%20%20My%20%22Garden%22%20&pin=5&activeHigh=1")
        self.assertEqual((state["name"], state["pin"], state["activeHigh"]), ("My Garden", 5, True))
        self.assertEqual(self.post("/config", "pin=0")["pin"], 5)   # D3: not allowed
        self.assertEqual(self.post("/config", "name=%20")["name"], "Irrigation ESP")
        self.assertLessEqual(len(self.post("/config", "name=" + "é" * 40)["name"].encode()), 31)


if __name__ == "__main__":
    unittest.main()
