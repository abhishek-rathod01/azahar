#!/usr/bin/env python3
"""Tests for analyze_perf.py using a synthetic log whose contents are known by construction.

    python3 -I tools/gt6/analysis/test_analyze_perf.py
"""

import io
import os
import random
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze_perf as ap  # noqa: E402

HEADER = "t_ms,frame_ms,work_ms,sleep_ms,speed_pct,refresh_hz,mode_id,vsync_ms,cpu,thermal,headroom,batt_c,event"


def build_log(path):
    """Ground truth:
    * 0-120 s at ~60 fps (16.7 ms +- 0.3)
    * 20-21 s: a 1 s hiccup of 40 ms frames            -> NOT an episode
    * 70-76 s: alternating 16.7 / 45 ms frames          -> exactly one episode
    * 60-90 s: emulation thread on core 3, else core 7  -> 30 s on a mid core
    * from 60 s the display reports 120 Hz / mode 3     -> a refresh change at 60 s
    * 100-103 s: paused, no frames                      -> must not count as a stall
    """
    rng = random.Random(1234)
    rows, events = [], []
    t = 0.0
    flip = False
    while t < 120.0:
        if 100.0 <= t < 103.0:
            t = 103.0
            continue
        if 20.0 <= t < 21.0:
            dt = 40.0
        elif 70.0 <= t < 76.0:
            flip = not flip
            dt = 45.0 if flip else 16.7
        else:
            dt = 16.7 + rng.uniform(-0.3, 0.3)
        t += dt / 1000.0
        cpu = 3 if 60.0 <= t < 90.0 else 7
        hz, mode = (120.0, 3) if t >= 60.0 else (60.0, 2)
        rows.append((t * 1000.0, dt, dt * 0.3, dt * 0.7, 100.0, hz, mode, 16.7, cpu, 1, 0.8, 33.0, ""))
    events += [(60200.0, "display_mode_changed"), (80000.0, "MARK"), (99900.0, "fragment_pause"),
               (100000.0, "emu_pause"), (103000.0, "emu_unpause")]
    lines = [
        "# azahar-gt6 performance log, started 20261015_120000",
        "# graphics_api=1",
        "# use_vsync=false",
        "# device=Test Phone",
        HEADER,
    ]
    for r in rows:
        lines.append("{:.3f},{:.3f},{:.3f},{:.3f},{:.1f},{:.2f},{},{:.3f},{},{},{:.2f},{:.1f},".format(*r[:12]))
    for te, name in events:
        lines.append("{:.3f}".format(te) + "," * 12 + name)
    lines.sort(key=lambda s: (0 if s.startswith("#") or s.startswith("t_ms") else 1,
                              float(s.split(",")[0]) if not s.startswith("#") and not s.startswith("t_ms") else 0))
    lines.append("# dropped_rows=0 dropped_events=0")
    lines.append("99999.000,16.700,5.0,11.0")  # a line truncated by a crash must be ignored
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return rows


class AnalyzerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp(prefix="gt6_analyze_test_")
        cls.path = os.path.join(cls.dir, "perf_synthetic.csv")
        cls.rows = build_log(cls.path)
        cls.header, cls.frames, cls.events = ap.load(cls.path)

    def test_percentile(self):
        self.assertEqual(ap.pct([1, 2, 3, 4], 50), 2.5)
        self.assertEqual(ap.pct([5], 99), 5)
        self.assertEqual(ap.pct([1, 2, 3, 4, 5], 100), 5)
        self.assertEqual(ap.pct([1, 2, 3, 4, 5], 0), 1)
        self.assertTrue(ap.pct([], 50) != ap.pct([], 50))  # NaN

    def test_loading(self):
        self.assertEqual(len(self.frames), len(self.rows))  # truncated line ignored
        self.assertEqual(len(self.events), 5)
        self.assertEqual(self.header.get("graphics_api"), "1")
        self.assertEqual(self.header.get("use_vsync"), "false")
        self.assertEqual(self.header.get("dropped_rows"), "0")

    def test_clean_segment_is_60fps_and_smooth(self):
        seg = [f for f in self.frames if 30.0 <= f["t"] < 60.0]
        s = ap.summarize(seg)
        self.assertAlmostEqual(s["avg_fps"], 1000 / 16.7, delta=0.3)
        self.assertLess(s["p99_ms"], 17.2)
        self.assertEqual(s["stutters"], 0)
        self.assertAlmostEqual(s["low1_fps"], 1000 / 17.0, delta=0.8)

    def test_episode_segment_is_bad(self):
        seg = [f for f in self.frames if 70.0 <= f["t"] < 76.0]
        s = ap.summarize(seg, ref_median=16.7)
        self.assertGreater(s["p99_ms"], 40.0)
        # 6 s of alternating 16.7 / 45 ms frames = about 97 pairs, so about 97 long frames
        self.assertTrue(90 <= s["stutters"] <= 105, s["stutters"])
        # without the session reference the window's own median hides them (regression guard
        # for the design choice): the count must not be larger than with the reference
        self.assertLessEqual(ap.summarize(seg)["stutters"], s["stutters"])

    def test_exactly_one_episode_found(self):
        eps = ap.episodes(self.frames, self.events)
        self.assertEqual(len(eps), 1, eps)
        e = eps[0]
        self.assertTrue(69.0 <= e["start_s"] <= 71.0, e)
        self.assertTrue(76.0 <= e["end_s"] <= 78.0, e)

    def test_one_second_hiccup_and_pause_are_not_episodes(self):
        eps = ap.episodes(self.frames, self.events)
        for e in eps:
            self.assertFalse(e["start_s"] < 30.0, "1 s hiccup at 20 s wrongly reported: %s" % e)
            self.assertFalse(e["start_s"] < 103.0 and e["end_s"] > 100.0, "pause reported: %s" % e)

    def test_pause_without_event_would_be_a_stall(self):
        # Same frames, but the pause events removed: the 3 s gap must now be reported.
        events = [e for e in self.events if e["name"] not in ("emu_pause", "emu_unpause")]
        eps = ap.episodes(self.frames, events)
        self.assertTrue(any(e["start_s"] < 103.0 and e["end_s"] > 100.0 for e in eps), eps)

    def test_core_and_cluster_share(self):
        cores, groups = ap.cpu_share(self.frames, ap.parse_clusters("0-2:A520,3-6:A720,7:X4"))
        mid = sum(1 for f in self.frames if f["cpu"] == 3) * 100.0 / len(self.frames)
        self.assertAlmostEqual(cores[3], mid, delta=0.1)
        self.assertAlmostEqual(groups["A720"], mid, delta=0.1)
        self.assertAlmostEqual(groups["X4"] + groups["A720"], 100.0, delta=0.2)
        self.assertGreater(groups["A720"], 20.0)

    def test_display_changes_detected(self):
        hz = ap.changes(self.frames, "hz")
        self.assertEqual([v for _, v in hz], [60.0, 120.0])
        self.assertAlmostEqual(hz[1][0], 60.0, delta=0.1)
        self.assertEqual([v for _, v in ap.changes(self.frames, "mode")], [2.0, 3.0])

    def test_cli_report(self):
        out = subprocess.run([sys.executable, "-I", os.path.join(HERE, "analyze_perf.py"), self.path],
                             capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("Jitter episodes", out.stdout)
        self.assertIn("before MARK", out.stdout)
        self.assertIn("after emu_unpause", out.stdout)
        self.assertIn("A720", out.stdout)

    def test_report_function_runs(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            text, data = ap.report(self.path, 20.0, ap.parse_clusters("0-2:A520,3-6:A720,7:X4"))
        self.assertEqual(len(data["episodes"]), 1)
        self.assertEqual(data["events"], 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
