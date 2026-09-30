"""Run the project's tests. See docs/TESTING.md for what each suite checks.

    python tests/run_tests.py              # minify, simulator and firmware tests
    python tests/run_tests.py --browser    # + the page in a headless browser (needs Playwright)
    python tests/run_tests.py --compile    # + compile for a real ESP8266 (needs arduino-cli or the Arduino IDE)
    python tests/run_tests.py --all        # everything

Exit code 0 when nothing failed. A suite whose tool is missing is reported as SKIPPED.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
FIRMWARE = TESTS / "firmware"
SKETCH = ROOT / "esp8266-smart-irrigation.ino"

sys.path.insert(0, str(ROOT))
sys.dont_write_bytecode = True  # no __pycache__ folders in the project


class Suite:
    def __init__(self, name):
        self.name, self.passed, self.failed, self.skipped, self.note = name, 0, 0, 0, ""

    def ok(self, label):
        self.passed += 1
        print(f"  ok    {label}")

    def fail(self, label, details=""):
        self.failed += 1
        print(f"  FAIL  {label}")
        for line in details.rstrip().splitlines():
            print(f"        {line}")

    def skip(self, why):
        self.skipped += 1
        self.note = why
        print(f"  SKIPPED: {why}")

    @property
    def status(self):
        return "FAILED" if self.failed else "SKIPPED" if self.skipped and not self.passed else "PASSED"


# ---- Python suites (unittest) ----

class Reporter(unittest.TestResult):
    def __init__(self, suite):
        super().__init__()
        self.suite = suite

    @staticmethod
    def label(test):
        doc = test.shortDescription()
        return f"{type(test).__name__}.{test._testMethodName}" + (f": {doc}" if doc else "")

    def addSuccess(self, test):
        self.suite.ok(self.label(test))

    def addFailure(self, test, err):
        self.suite.fail(self.label(test), self._exc_info_to_string(err, test))

    def addError(self, test, err):
        self.suite.fail(self.label(test), self._exc_info_to_string(err, test))

    def addSubTest(self, test, subtest, err):
        if err is not None:
            self.suite.fail(subtest.id().split(".", 3)[-1], self._exc_info_to_string(err, test))

    def addSkip(self, test, reason):
        if not self.suite.skipped:
            self.suite.skip(reason)
        else:
            self.suite.skipped += 1


def run_python(suite, module):
    tests = unittest.defaultTestLoader.loadTestsFromName(f"tests.{module}")
    tests.run(Reporter(suite))


# ---- Firmware suite (C++) ----

def find_compiler():
    for name in (os.environ.get("CXX"), "g++", "clang++"):
        if name and shutil.which(name):
            return shutil.which(name)
    return None


def run_firmware(suite):
    compiler = find_compiler()
    if not compiler:
        suite.skip("no C++ compiler found (install g++ or clang++, or set CXX); see docs/TESTING.md")
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ("test_firmware.exe" if os.name == "nt" else "test_firmware")
        build = subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(FIRMWARE / "stubs"),
             str(FIRMWARE / "test_firmware.cpp"), "-o", str(exe)],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        warnings = [line for line in build.stderr.splitlines() if "warning:" in line]
        if build.returncode or warnings:
            suite.fail("build the sketch on the PC (errors or warnings)", build.stderr)
            return
        listing = subprocess.run([str(exe), "--list"], capture_output=True, text=True, encoding="utf-8", errors="replace", check=True).stdout
        for line in listing.strip().splitlines():
            name, what = line.split("\t", 1)
            result = subprocess.run([str(exe), name], capture_output=True, text=True, encoding="utf-8", errors="replace")
            label = f"{name}: {what}"
            if result.returncode == 0:
                suite.ok(label)
                for info in result.stdout.strip().splitlines():
                    print(f"        {info.strip()}")
            else:
                suite.fail(label, result.stdout + result.stderr)


# ---- Compile for the ESP8266 ----

def find_arduino_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    bundled = Path("resources/app/lib/backend/resources")
    candidates = {
        "Windows": Path(os.environ.get("LOCALAPPDATA", "")) / "Programs/Arduino IDE" / bundled / "arduino-cli.exe",
        "Darwin": Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"),
        "Linux": Path.home() / ".local/share/arduino-ide" / bundled / "arduino-cli",
    }
    path = candidates.get(platform.system())
    return str(path) if path and path.exists() else None


def run_compile(suite, fqbn):
    cli = find_arduino_cli()
    if not cli:
        suite.skip("arduino-cli not found (install it or the Arduino IDE 2); see docs/TESTING.md")
        return
    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            [cli, "compile", "--fqbn", fqbn, "--warnings", "all", "--build-path", tmp, str(ROOT)],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
    output = result.stdout + result.stderr
    warnings = [line for line in output.splitlines() if "warning:" in line]
    label = f"compile for {fqbn} with no warnings"
    if result.returncode or warnings:
        suite.fail(label, output)
        return
    suite.ok(label)
    for line in output.splitlines():
        if "Variables and constants in RAM" in line or "Code in flash" in line:
            print(f"        {line.strip(' .')}")


# ---- Main ----

def main():
    parser = argparse.ArgumentParser(description="Run the project's tests (see docs/TESTING.md).")
    parser.add_argument("--browser", action="store_true", help="also test the page in a headless browser")
    parser.add_argument("--compile", action="store_true", help="also compile the sketch for a real ESP8266")
    parser.add_argument("--all", action="store_true", help="run every suite")
    parser.add_argument("--fqbn", default="esp8266:esp8266:nodemcuv2", help="board for --compile")
    args = parser.parse_args()

    plan = [
        ("minify", lambda s: run_python(s, "test_minify")),
        ("simulator", lambda s: run_python(s, "test_simulator")),
        ("firmware", run_firmware),
    ]
    if args.browser or args.all:
        plan.append(("browser", lambda s: run_python(s, "test_browser")))
    if args.compile or args.all:
        plan.append(("compile", lambda s: run_compile(s, args.fqbn)))

    suites, start = [], time.time()
    for name, run in plan:
        print(f"\n== {name}")
        suite = Suite(name)
        run(suite)
        suites.append(suite)

    print(f"\n== Summary ({time.time() - start:.1f} s)")
    for s in suites:
        counts = f"{s.passed} passed" + (f", {s.failed} failed" if s.failed else "")
        print(f"  {s.status:8} {s.name:10} {counts}" + (f"  ({s.note})" if s.status == "SKIPPED" else ""))
    failed = any(s.failed for s in suites)
    print("\nFAILED" if failed else "\nAll suites passed" if all(s.status == "PASSED" for s in suites)
          else "\nNo failures, but some suites were skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
