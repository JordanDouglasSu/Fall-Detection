#!/usr/bin/env python3
"""
Automated tests for the fall-detection package.

    python tests/run_tests.py            # algorithm + tools (needs a C++ compiler)
    python tests/run_tests.py --mqtt     # also MQTT round trip (needs Mosquitto on localhost:1883)

What is tested
  * every synthetic scenario gives the expected outcome (alert / no alert)
  * the right reason is given (descent, long lie, struggling...)
  * stumble and recovery events appear when they should
  * the same scene at 4 fps and 8 fps gives the same decision at nearly the same time
  * the PIPER answer classifier (ok / help / no answer)
  * optionally: virtual node <-> monitor over a real broker, incl. closed-loop commands

Synthetic scenes test the LOGIC. They do not measure real accuracy — use
recorded sessions and tools/evaluate.py for that.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import evaluate  # noqa: E402
import synth  # noqa: E402

MQTT_PORT = int(os.environ.get("PIPER_TEST_MQTT_PORT", "1883"))


class SyntheticScenarios(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="piper_fall_test_")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "synth.py"), "--out", cls.dir],
                       check=True, capture_output=True)
        files = evaluate.collect_files([cls.dir])
        cls.results = {os.path.basename(r["file"])[:-4]: r for r in evaluate.run_replay(files)}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir, ignore_errors=True)

    def events(self, name):
        return [e["e"] for e in self.results[name]["events"]]

    def test_every_scenario_has_expected_outcome(self):
        wrong = [f"{n}: expected {r['expect']}, got {'alert' if r['alert'] else 'none'}"
                 for n, r in sorted(self.results.items()) if r["alert"] != (r["expect"] == "alert")]
        self.assertEqual(wrong, [], "\n" + "\n".join(wrong))

    def test_all_scenarios_generated(self):
        expected = len(synth.SCENARIOS) + len(synth.ALSO_8FPS)
        self.assertEqual(len(self.results), expected)

    def test_fall_directions_detected_via_descent(self):
        for n in ["fall_sideways_4fps", "fall_toward_camera_4fps", "fall_away_from_camera_4fps"]:
            self.assertEqual(self.results[n].get("reason"), "descent_then_still", n)

    def test_struggling_person_still_alerts(self):
        self.assertEqual(self.results["fall_struggling_4fps"].get("reason"), "descent_then_on_floor")

    def test_slow_slide_caught_by_long_lie(self):
        self.assertEqual(self.results["slow_slide_4fps"].get("reason"), "long_lie")
        self.assertNotIn("descent", self.events("slow_slide_4fps"))

    def test_stumble_logged_without_alert(self):
        ev = self.events("stumble_get_up_4fps")
        self.assertIn("stumble", ev)
        self.assertNotIn("fall", ev)

    def test_alert_ends_when_person_gets_up(self):
        ev = self.events("fall_then_recover_4fps")
        self.assertIn("fall", ev)
        self.assertIn("recovered", ev)
        self.assertLess(ev.index("fall"), ev.index("recovered"))

    def test_alert_does_not_time_out_while_on_floor(self):
        # person stays down to the end: the alert must still be active (no auto-clear timer)
        ev = self.events("fall_sideways_4fps")
        self.assertNotIn("recovered", ev)

    def test_heater_never_becomes_a_person_on_the_floor(self):
        self.assertFalse(self.results["heater_on_floor_4fps"]["alert"])

    def test_bed_is_not_floor(self):
        self.assertNotIn("on_floor", self.events("lie_on_bed_4fps"))

    def test_pet_lying_on_floor_is_not_a_fall(self):
        self.assertFalse(self.results["cat_on_floor_4fps"]["alert"])

    def test_residual_heat_after_person_leaves_is_not_a_fall(self):
        self.assertFalse(self.results["residual_heat_4fps"]["alert"])

    def test_fall_out_of_bed_caught_by_long_lie(self):
        self.assertEqual(self.results["fall_out_of_bed_4fps"].get("reason"), "long_lie")

    def test_ceiling_mount(self):
        self.assertTrue(self.results["ceiling_fall_4fps"]["alert"])
        for n in ["ceiling_walk_4fps", "ceiling_sit_chair_4fps", "ceiling_bed_4fps"]:
            self.assertFalse(self.results[n]["alert"], n)

    def test_frame_rate_independent(self):
        for n in synth.ALSO_8FPS:
            a, b = self.results[f"{n}_4fps"], self.results[f"{n}_8fps"]
            self.assertEqual(a["alert"], b["alert"], n)
            if a["alert"]:
                self.assertLess(abs(a["first_alert_ms"] - b["first_alert_ms"]), 1500, n)

    def test_latency_reasonable(self):
        m = evaluate.metrics(list(self.results.values()))
        self.assertLess(m["latency_median_s"], 15.0)

    def test_parameter_override_changes_behaviour(self):
        f = os.path.join(self.dir, "fall_sideways_4fps.csv")
        base = evaluate.run_replay([f])[0]["first_alert_ms"]
        slower = evaluate.run_replay([f], ["stillConfirmMs=12000"])[0]["first_alert_ms"]
        self.assertGreater(slower, base + 4000)

    def test_original_algorithm_runs(self):
        res = evaluate.run_replay([os.path.join(self.dir, "fall_sideways_4fps.csv")], algo="legacy")
        self.assertEqual(len(res), 1)


class AnswerClassifier(unittest.TestCase):
    def test_answers(self):
        from fall_monitor import FallResponder
        r = FallResponder(None, None, None, None)
        cases = {
            "I'm fine": "ok", "yes, okay": "ok", "I'm not hurt": "ok", "no help needed": "ok",
            "I'm not okay": "help", "help": "help", "I can't get up": "help", "my hip is in pain": "help",
            None: "no_answer", "": "no_answer", "what?": "unclear",
        }
        for text, want in cases.items():
            self.assertEqual(r.classify(text), want, repr(text))


class MqttRoundTrip(unittest.TestCase):
    def setUp(self):
        if os.environ.get("PIPER_TEST_MQTT") != "1":
            self.skipTest("MQTT tests need --mqtt and a broker on localhost")

    def test_virtual_node_events_and_commands(self):
        tools = os.path.join(ROOT, "tools")
        node = subprocess.Popen([sys.executable, os.path.join(tools, "virtual_fall_node.py"),
                                 "--port", str(MQTT_PORT), "--script", "1.5:fall", "--linger", "6"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            w = subprocess.run([sys.executable, os.path.join(tools, "fall_monitor.py"), "--port", str(MQTT_PORT),
                                "--wait-for", "fall", "--timeout", "8"], capture_output=True, text=True)
            self.assertEqual(w.returncode, 0, w.stdout + w.stderr)
            c = subprocess.run([sys.executable, os.path.join(tools, "fall_monitor.py"), "--port", str(MQTT_PORT),
                                "--ok", "--timeout", "4"], capture_output=True, text=True)
            self.assertEqual(c.returncode, 0, c.stdout + c.stderr)
            self.assertIn('"reason": "user_ok"', c.stdout)
        finally:
            node.wait(timeout=15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mqtt", action="store_true", help="include MQTT tests (broker on localhost)")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    if a.mqtt:
        os.environ["PIPER_TEST_MQTT"] = "1"
    t0 = time.time()
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=2 if a.v else 1).run(suite)
    print(f"\n{res.testsRun} tests in {time.time() - t0:.1f} s")
    return 0 if res.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
