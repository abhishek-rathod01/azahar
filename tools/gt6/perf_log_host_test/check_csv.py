#!/usr/bin/env python3
"""Independent parser/validator for the files written by perf_log.cpp."""
import csv, glob, math, sys

scenario, outdir = sys.argv[1], sys.argv[2]
files = glob.glob(f"{outdir}/perf_*.csv")
assert len(files) == 1, f"expected exactly 1 csv, got {files}"
raw = open(files[0]).read().splitlines()
comments = [l for l in raw if l.startswith("#")]
data = [l for l in raw if not l.startswith("#")]
assert data[0].startswith("t_ms,frame_ms,work_ms,sleep_ms,speed_pct,refresh_hz,mode_id,vsync_ms,cpu,thermal,headroom,batt_c,event"), data[0]
rows = list(csv.reader(data))
hdr, body = rows[0], rows[1:]
assert len(hdr) == 13, len(hdr)
bad = [r for r in body if len(r) != 13]
assert not bad, f"{len(bad)} lines without 13 fields, first: {bad[0]}"
ts = [float(r[0]) for r in body]
assert ts == sorted(ts), "t_ms not monotonic"
frames = [r for r in body if r[12] == ""]
events = [r for r in body if r[12] != ""]
dropped = [int(x.split("=")[1]) for l in comments for x in l.split() if x.startswith("dropped_rows=")]
print(f"[{scenario}] comments={len(comments)} frames={len(frames)} events={len(events)} dropped_rows={dropped}")

if scenario == "basic":
    assert comments[0].startswith("# azahar-gt6 performance log"), comments[0]
    assert any("graphics_api=Vulkan" in c for c in comments), "settings header missing"
    assert any("line with, comma" in c for c in comments), "comment lines may contain commas"
    # 5+120+30+30+10 = 195 FrameDone calls; the first call and the first one after the
    # pause-reset only prime the timer, so exactly 193 rows are expected
    assert len(frames) == 193, f"frames={len(frames)}"
    # the 500 ms pause must not appear as a frame interval
    f = [float(r[1]) for r in frames]
    assert max(f) < 100, f"pause leaked into frame times: max={max(f)}"
    # frame ~= work + sleep (2..3ms + 13..14ms, timer slack tolerated)
    for r in frames:
        fm, wk, sl = float(r[1]), float(r[2]), float(r[3])
        assert abs(fm - (wk + sl)) < 1.5, f"frame != work+sleep: {r}"
        assert sl >= 12.5, f"sleep too small {r}"
    # emulated speed ~ 16.715ms emu / ~16.x ms wall -> ~100%, never absurd
    sp = [float(r[4]) for r in frames]
    assert all(50 < s < 140 for s in sp), (min(sp), max(sp))
    # display state transitions are reflected on rows
    assert any(r[5] == "60.00" and r[6] == "2" for r in frames), "initial display state missing"
    assert any(r[5] == "120.00" and r[6] == "3" for r in frames), "display change missing"
    assert any(r[10] == "0.85" and r[9] == "1" and r[11] == "31.5" for r in frames), "thermal missing"
    assert any(r[9] == "" and r[10] == "" and r[11] == "" for r in frames), "unknown thermal should be empty"
    # cpu column is a plausible core id
    assert all(r[8] != "" and 0 <= int(r[8]) < 4096 for r in frames), "cpu column"
    names = [r[12] for r in events]
    for need in ("surface_changed", "MARK", "display_mode_changed", "emu_pause", "emu_unpause"):
        assert need in names, f"event {need} missing: {sorted(set(names))}"
    assert "worker;event;with junk" in names, "event sanitising failed"
    # event rows carry nothing but t_ms and the name
    assert all(all(c == "" for c in r[1:12]) for r in events), "event rows must have empty numeric fields"
    assert dropped == [0], dropped
elif scenario == "overflow":
    assert dropped and dropped[0] > 0, "expected drops when the producer outruns the writer"
    assert len(frames) + dropped[0] == 200000 - 1, (len(frames), dropped, "rows + dropped must equal frames - 1")
elif scenario == "noframes":
    assert any("no frame was rendered" in c for c in comments), comments
    assert [r[12] for r in events] == ["only_event"], events
print(f"[{scenario}] OK")
