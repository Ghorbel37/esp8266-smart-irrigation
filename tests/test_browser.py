"""Browser tests: the web page clicked through in a headless browser, against the simulator.

    pip install playwright                   # once
    python -m unittest tests/test_browser.py -v

Uses Playwright's Chromium if installed (`playwright install chromium`), otherwise the Chrome or Edge
already on the computer. Skipped when Playwright isn't installed. The browser needs internet access
for the Tailwind styles, like the real page.
"""
import copy
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import simulator  # noqa: E402

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

INITIAL_STATE = copy.deepcopy(simulator.state)


def launch(playwright):
    errors = []
    for channel in (None, "chrome", "msedge"):
        try:
            return playwright.chromium.launch(headless=True, **({"channel": channel} if channel else {}))
        except Exception as e:  # browser not installed: try the next one
            errors.append(str(e).splitlines()[0])
    raise unittest.SkipTest("No browser found for Playwright: " + " / ".join(errors))


@unittest.skipIf(sync_playwright is None, "Playwright isn't installed (pip install playwright)")
class Page(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        simulator.Handler.log_message = lambda *args: None
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), simulator.Handler)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.playwright = sync_playwright().start()
        try:
            cls.browser = launch(cls.playwright)
        except unittest.SkipTest:
            cls.playwright.stop()
            cls.server.shutdown()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        simulator.state.clear()
        simulator.state.update(copy.deepcopy(INITIAL_STATE))
        self.context = self.browser.new_context(viewport={"width": 390, "height": 844})
        self.page = self.context.new_page()
        self.errors, self.posts = [], []
        self.page.on("pageerror", lambda e: self.errors.append(str(e)))
        self.page.on("request", lambda r: self.posts.append((r.url.replace(self.base, ""), r.post_data or ""))
                     if r.method == "POST" else None)
        self.page.goto(self.base)
        self.page.wait_for_function("document.getElementById('relay').textContent != 'Loading...'")

    def tearDown(self):
        self.context.close()
        self.assertEqual(self.errors, [], "JavaScript errors on the page")

    def text(self, selector):
        return self.page.inner_text(selector)

    def click_and_wait(self, selector):
        with self.page.expect_response(lambda r: r.request.method == "POST"):
            self.page.click(selector)

    def test_initial_state(self):
        self.assertEqual(self.text("#title"), "Irrigation ESP")
        self.assertEqual(self.page.title(), "Irrigation ESP")
        self.assertEqual(self.text("#relay"), "Not watering")
        self.assertEqual(self.text("#next"), "Next run: none scheduled")
        self.assertEqual(self.page.input_value("#onMin"), "60")
        self.assertTrue(self.page.is_hidden("#err"))

    def test_modes(self):
        self.click_and_wait("[data-m=on]")
        self.assertEqual((self.text("#relay"), self.text("#next")), ("Watering", "Forced on, 60 min left"))
        self.click_and_wait("[data-m=off]")
        self.assertEqual((self.text("#relay"), self.text("#next")), ("Not watering", "Forced off"))
        self.click_and_wait("[data-m=clear]")
        self.click_and_wait("#pause")
        self.assertEqual(self.text("#pause"), "Paused (5 min left)")
        self.assertEqual([p for p, _ in self.posts], ["/relay1/on", "/relay1/off", "/relay1/clear", "/disable"])
        self.assertEqual(self.posts[0][1], "minutes=60")

    def test_force_on_duration_is_sent_and_remembered(self):
        self.page.fill("#onMin", "15")
        self.page.dispatch_event("#onMin", "change")
        self.click_and_wait("[data-m=on]")
        self.assertEqual(self.text("#next"), "Forced on, 15 min left")
        self.assertEqual(self.posts[-1], ("/relay1/on", "minutes=15"))
        self.page.reload()
        self.assertEqual(self.page.input_value("#onMin"), "15")  # remembered by this browser

    def test_invalid_force_on_duration_is_not_sent(self):
        self.page.fill("#onMin", "0")
        self.page.click("[data-m=on]")
        self.page.wait_for_timeout(300)
        self.assertEqual(self.posts, [])

    def test_save_schedule(self):
        for day in ("Mo", "We"):
            self.page.click(f"#days button:text-is('{day}')")
        self.page.fill("#start", "23:00")
        self.page.fill("#end", "01:00")
        self.click_and_wait("text=Save schedule")
        self.assertEqual(self.posts[-1], ("/setSchedule", "day=1&day=3&startTime=23%3A00&endTime=01%3A00"))
        self.page.wait_for_function("document.getElementById('saved').textContent == 'Saved'")
        state = simulator.state_json()
        self.assertEqual((state["days"], state["start"], state["end"]), ([0, 1, 0, 1, 0, 0, 0], "23:00", "01:00"))

    def test_device_settings(self):
        self.page.click("#cfgBtn")
        self.assertEqual(self.page.input_value("#cfgName"), "Irrigation ESP")
        self.assertEqual(self.page.locator("#cfgPin option").all_inner_texts(),
                         ["D1 (GPIO5)", "D2 (GPIO4)", "D5 (GPIO14)", "D6 (GPIO12)", "D7 (GPIO13)"])
        self.page.fill("#cfgName", "Garden")
        self.page.select_option("#cfgPin", "14")
        self.page.check("#cfgHigh")
        self.click_and_wait("text=Save settings")
        self.assertEqual(self.text("#title"), "Garden")
        self.assertEqual(self.page.title(), "Garden")
        self.assertTrue(self.page.is_hidden("#cfg"))
        self.assertEqual(self.posts[-1], ("/config", "name=Garden&pin=14&activeHigh=1"))

    def test_error_when_the_controller_is_unreachable(self):
        self.page.route("**/api/state", lambda route: route.abort())
        self.page.evaluate("call('/api/state')")
        self.page.wait_for_selector("#err", state="visible")
        self.assertIn("Could not reach the controller", self.text("#err"))

    def test_recovers_when_the_controller_stops_answering(self):
        """A request that never gets an answer is dropped after 10 s, and the page recovers by itself."""
        hung, open_requests, peak = [], [0], [0]

        def track(delta):
            open_requests[0] += delta
            peak[0] = max(peak[0], open_requests[0])

        self.page.on("request", lambda r: track(1) if "/api/state" in r.url else None)
        self.page.on("requestfinished", lambda r: track(-1) if "/api/state" in r.url else None)
        self.page.on("requestfailed", lambda r: track(-1) if "/api/state" in r.url else None)
        self.page.route("**/api/state", lambda route: hung.append(route))  # never answers
        self.page.wait_for_selector("#err", state="visible", timeout=25000)
        self.assertIn("Retrying", self.text("#err"))
        self.page.wait_for_timeout(12000)  # more polls while it still doesn't answer
        self.assertLessEqual(peak[0], 1, "state requests piled up")
        self.page.unroute_all(behavior="ignoreErrors")  # let the held requests go quietly
        self.page.wait_for_selector("#err", state="hidden", timeout=25000)
        self.assertEqual(self.text("#relay"), "Not watering")

    def test_dark_mode_toggle(self):
        dark = "document.documentElement.classList.contains('dark')"
        self.page.emulate_media(color_scheme="light")
        self.page.reload()
        self.assertFalse(self.page.evaluate(dark))
        self.page.click("#theme")
        self.assertTrue(self.page.evaluate(dark))
        self.page.reload()
        self.assertTrue(self.page.evaluate(dark))   # choice remembered
        self.page.click("#theme")                   # back to the device theme
        self.assertFalse(self.page.evaluate(dark))


if __name__ == "__main__":
    unittest.main()
