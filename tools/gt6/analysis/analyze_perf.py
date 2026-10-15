#!/usr/bin/env python3
"""Analyse an Azahar GT6 performance log (log/perf_<time>.csv).

    analyze_perf.py perf_20261015_120000.csv [--window 20] [--clusters 0-2:A520,3-6:A720,7:X4]

Standard library only. Prints a Markdown report; --json also writes machine-readable results.

Definitions (stated so numbers are reproducible):
  frames        rows with an empty event column
  avg FPS       frames / sum(frame_ms)   (pauses are excluded: the frame after a resume is dropped
                by the logger)
  p99 frame     99th percentile of frame_ms, linear interpolation
  1% low FPS    1000 / mean of the slowest 1 % of frames (at least one frame)
  stutter       frame_ms > 1.5 * median frame_ms
  jitter episode  the log is cut into non-overlapping 1 s bins; a bin is bad when its p99
                frame_ms > 25 ms, or when it holds fewer than 5 frames (a stall). More than 2
                consecutive bad bins (i.e. 3 or more) form an episode, matching the brief's
                "p99 > 25 ms for longer than 2 s". Time between emu_pause and emu_unpause is
                ignored. Bins quantise the edges, so an episode can be up to ~1 s longer than
                the real misbehaviour; the per-second p99 values are also printed.
"""

import argparse
import json
import math
import sys

COLUMNS = [
    "t_ms", "frame_ms", "work_ms", "sleep_ms", "speed_pct", "refresh_hz", "mode_id",
    "vsync_ms", "cpu", "thermal", "headroom", "batt_c", "event",
]


def pct(sorted_vals, p):
    """Percentile (0-100) with linear interpolation; sorted_vals must be sorted ascending."""
    if not sorted_vals:
        return float("nan")
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    k = (len(sorted_vals) - 1) * p / 100.0
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return sorted_vals[int(k)]
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def fnum(text):
    if text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def load(path):
    header, frames, events = {}, [], []
    with open(path, newline="") as fh:
        lines = fh.read().splitlines()
    rows = []
    for line in lines:
        if line.startswith("#"):
            body = line[1:].strip()
            if body.count("=") == 1:
                # "key=value": the value may contain spaces (device name, ...)
                key, _, value = body.partition("=")
                if " " not in key:
                    header[key] = value.strip()
            else:
                # "a=1 b=2": several short tokens on one line (e.g. the dropped_* summary)
                for token in body.split():
                    if "=" in token:
                        key, _, value = token.partition("=")
                        header[key] = value
            continue
        rows.append(line)
    if not rows:
        raise SystemExit("no data rows in " + path)
    if not rows[0].startswith("t_ms,"):
        raise SystemExit("unexpected CSV header: " + rows[0][:80])
    for line in rows[1:]:
        f = line.split(",")
        if len(f) != len(COLUMNS):
            continue  # a truncated last line after a crash
        t = fnum(f[0])
        if t is None:
            continue
        if f[12] != "":
            events.append({"t": t / 1000.0, "name": f[12]})
            continue
        cpu = fnum(f[8])
        frames.append({
            "t": t / 1000.0,
            "frame": fnum(f[1]), "work": fnum(f[2]), "sleep": fnum(f[3]), "speed": fnum(f[4]),
            "hz": fnum(f[5]), "mode": fnum(f[6]), "vsync": fnum(f[7]),
            "cpu": int(cpu) if cpu is not None else None,
            "thermal": fnum(f[9]), "headroom": fnum(f[10]), "batt": fnum(f[11]),
        })
    return header, frames, events


def summarize(frames, ref_median=None):
    ft = sorted(f["frame"] for f in frames if f["frame"] is not None)
    if not ft:
        return {"frames": 0}
    n = len(ft)
    active_s = sum(ft) / 1000.0
    median = pct(ft, 50)
    # Stutter threshold is relative to the whole session's median when given: inside a bad
    # window the window's own median rises and would hide the very stutters being counted.
    ref = ref_median if ref_median is not None else median
    worst = ft[-max(1, n // 100):]
    work = sorted(f["work"] for f in frames if f["work"] is not None)
    slp = sorted(f["sleep"] for f in frames if f["sleep"] is not None)
    spd = [f["speed"] for f in frames if f["speed"] is not None]
    return {
        "frames": n,
        "active_s": round(active_s, 2),
        "avg_fps": round(n / active_s, 2) if active_s else None,
        "mean_ms": round(sum(ft) / n, 3),
        "median_ms": round(median, 3),
        "p95_ms": round(pct(ft, 95), 3),
        "p99_ms": round(pct(ft, 99), 3),
        "p999_ms": round(pct(ft, 99.9), 3),
        "max_ms": round(ft[-1], 3),
        "low1_fps": round(1000.0 / (sum(worst) / len(worst)), 2),
        "stutters": sum(1 for v in ft if v > 1.5 * ref),
        "work_mean_ms": round(sum(work) / len(work), 3) if work else None,
        "work_p99_ms": round(pct(work, 99), 3) if work else None,
        "sleep_mean_ms": round(sum(slp) / len(slp), 3) if slp else None,
        "sleep_p1_ms": round(pct(slp, 1), 3) if slp else None,
        "speed_mean_pct": round(sum(spd) / len(spd), 2) if spd else None,
    }


def paused_spans(events):
    """(start, end) seconds between emu_pause and the following emu_unpause."""
    spans, start = [], None
    for e in events:
        if e["name"] == "emu_pause" and start is None:
            start = e["t"]
        elif e["name"] == "emu_unpause" and start is not None:
            spans.append((start, e["t"] + 1.0))  # +1 s: the logger drops the first frame back
            start = None
    if start is not None:
        spans.append((start, float("inf")))
    return spans


def bin_p99(frames, events=(), bin_s=1.0):
    """[(bin_start_s, frame_count, p99_ms or None, paused)] for each full bin."""
    if not frames:
        return []
    t0 = frames[0]["t"]
    nbins = int((frames[-1]["t"] - t0) // bin_s)
    bins = [[] for _ in range(nbins)]
    for f in frames:
        i = int((f["t"] - t0) // bin_s)
        if i < nbins and f["frame"] is not None:
            bins[i].append(f["frame"])
    spans = paused_spans(events)
    out = []
    for i, vals in enumerate(bins):
        a = t0 + i * bin_s
        paused = any(a < y and a + bin_s > x for x, y in spans)
        vals.sort()
        out.append((a, len(vals), pct(vals, 99) if vals else None, paused))
    return out


def episodes(frames, events=(), limit_ms=25.0, min_bins=3):
    """Jitter episodes per the brief (see module docstring for the exact rule)."""
    out, cur = [], None
    for a, n, p99, paused in bin_p99(frames, events):
        bad = (not paused) and (n < 5 or (p99 is not None and p99 > limit_ms))
        if bad:
            cur = [a, a + 1.0, 1] if cur is None else [cur[0], a + 1.0, cur[2] + 1]
        else:
            if cur is not None and cur[2] >= min_bins:
                out.append(cur)
            cur = None
    if cur is not None and cur[2] >= min_bins:
        out.append(cur)
    return [{"start_s": round(a, 2), "end_s": round(b, 2), "length_s": round(b - a, 2)}
            for a, b, _ in out]


def parse_clusters(text):
    mapping = {}
    for part in text.split(","):
        ids, _, name = part.partition(":")
        lo, _, hi = ids.partition("-")
        for c in range(int(lo), int(hi or lo) + 1):
            mapping[c] = name
    return mapping


def cpu_share(frames, clusters):
    cores, groups = {}, {}
    total = 0
    for f in frames:
        if f["cpu"] is None:
            continue
        total += 1
        cores[f["cpu"]] = cores.get(f["cpu"], 0) + 1
        g = clusters.get(f["cpu"], "?")
        groups[g] = groups.get(g, 0) + 1
    if not total:
        return {}, {}
    return ({c: round(100.0 * v / total, 1) for c, v in sorted(cores.items())},
            {g: round(100.0 * v / total, 1) for g, v in sorted(groups.items())})


def changes(frames, key):
    """Values of a column with the time they first appeared (shows mode/refresh drift)."""
    out, last = [], object()
    for f in frames:
        v = f[key]
        if v != last:
            out.append((round(f["t"], 1), v))
            last = v
    return out


def fmt_row(label, s):
    if s.get("frames", 0) == 0:
        return f"| {label} | 0 | | | | | | |"
    return (f"| {label} | {s['frames']} | {s['avg_fps']} | {s['p99_ms']} | {s['low1_fps']} | "
            f"{s['stutters']} | {s['work_mean_ms']} | {s['sleep_mean_ms']} |")


def around(frames, t0, t1):
    return [f for f in frames if t0 <= f["t"] < t1]


def report(path, window, clusters):
    header, frames, events = load(path)
    out = []
    out.append(f"# Performance log report: `{path}`\n")
    if header:
        keep = ["app_version", "git_hash", "application_id", "device", "android_release",
                "graphics_api", "resolution_factor", "frame_limit", "use_vsync",
                "use_skip_duplicate_frames", "async_presentation", "async_shader_compilation",
                "shaders_accurate_mul", "use_hw_shader", "cpu_clock_percentage",
                "dropped_rows", "dropped_events"]
        out.append("Settings in effect: " + ", ".join(
            f"{k}={header[k]}" for k in keep if k in header) + "\n")
    overall = summarize(frames)
    ref = overall.get("median_ms")  # session median: the stutter reference for every window
    out.append("## Overall\n")
    out.append("| span | frames | avg FPS | p99 ms | 1% low FPS | stutters | work ms | sleep ms |")
    out.append("|---|---|---|---|---|---|---|---|")
    out.append(fmt_row("all", overall))
    out.append("")
    if overall.get("frames"):
        out.append(f"median {overall['median_ms']} ms, p95 {overall['p95_ms']} ms, "
                   f"p99.9 {overall['p999_ms']} ms, max {overall['max_ms']} ms, "
                   f"mean emulation speed {overall['speed_mean_pct']} %, "
                   f"sleep p1 {overall['sleep_p1_ms']} ms (near 0 = no headroom)\n")

    eps = episodes(frames, events)
    out.append("## Jitter episodes (p99 > 25 ms for > 2 s)\n")
    out.append("none\n" if not eps else "\n".join(
        f"* {e['start_s']} s to {e['end_s']} s ({e['length_s']} s)" for e in eps) + "\n")

    out.append("## Per minute\n")
    out.append("| minute | frames | avg FPS | p99 ms | 1% low FPS | stutters | work ms | sleep ms |")
    out.append("|---|---|---|---|---|---|---|---|")
    if frames:
        last_min = int(frames[-1]["t"] // 60)
        for m in range(last_min + 1):
            out.append(fmt_row(str(m), summarize(around(frames, m * 60, (m + 1) * 60), ref)))
    out.append("")

    cores, groups = cpu_share(frames, clusters)
    out.append("## Emulation thread placement (share of frames)\n")
    out.append(f"per core: {cores}\n\nper cluster: {groups}\n")

    out.append("## Display state changes (time s, value)\n")
    for key, label in (("hz", "refresh_hz"), ("mode", "mode_id"), ("thermal", "thermal status")):
        ch = changes(frames, key)
        out.append(f"* {label}: {ch[:12]}{' ...' if len(ch) > 12 else ''}")
    vs = sorted(f["vsync"] for f in frames if f["vsync"] is not None)
    if vs:
        out.append(f"* measured vsync interval: median {pct(vs, 50):.3f} ms "
                   f"(min {vs[0]:.3f}, max {vs[-1]:.3f}) -> about {1000.0 / pct(vs, 50):.1f} Hz")
    hr = [f["headroom"] for f in frames if f["headroom"] is not None]
    bt = [f["batt"] for f in frames if f["batt"] is not None]
    if hr:
        out.append(f"* thermal headroom: {min(hr):.2f} to {max(hr):.2f}")
    if bt:
        out.append(f"* battery temperature: {min(bt):.1f} to {max(bt):.1f} C")
    out.append("")

    out.append("## Events\n")
    if not events:
        out.append("none\n")
    else:
        counts = {}
        for e in events:
            counts[e["name"].split(" ")[0]] = counts.get(e["name"].split(" ")[0], 0) + 1
        out.append(f"counts: {counts}\n")
        for e in events[:80]:
            out.append(f"* {e['t']:.1f} s  {e['name']}")
        if len(events) > 80:
            out.append(f"* ... {len(events) - 80} more")
        out.append("")

    marks = [e for e in events if e["name"] == "MARK"]
    resumes = [e for e in events if e["name"] == "emu_unpause"]
    out.append(f"## Windows of {window} s around key events\n")
    out.append("| what | when s | frames | avg FPS | p99 ms | 1% low FPS | stutters | work ms | sleep ms |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for e in marks:
        out.append(fmt_row("before MARK", summarize(around(frames, e["t"] - window, e["t"]), ref))
                   .replace("| before MARK |", f"| before MARK | {e['t']:.1f} |", 1))
    for e in resumes:
        out.append(fmt_row("after emu_unpause", summarize(around(frames, e["t"], e["t"] + window), ref))
                   .replace("| after emu_unpause |", f"| after emu_unpause | {e['t']:.1f} |", 1))
    out.append("")
    return "\n".join(out), {
        "overall": overall, "episodes": eps, "cpu_share": cores, "cluster_share": groups,
        "events": len(events), "header": header,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("csv")
    ap.add_argument("--window", type=float, default=20.0, help="seconds around events")
    ap.add_argument("--clusters", default="0-2:A520,3-6:A720,7:X4",
                    help="cpu id -> cluster name (assumed layout, verify with policy* in cpufreq.log)")
    ap.add_argument("--json", help="also write results as JSON to this path")
    args = ap.parse_args()
    text, data = report(args.csv, args.window, parse_clusters(args.clusters))
    print(text)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(data, fh, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
