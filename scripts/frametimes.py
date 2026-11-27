#!/usr/bin/env python3
"""Capture in-game frame pacing from SurfaceFlinger plus GPU load/clock.

    python scripts/frametimes.py --pkg dev.eden.eden_emulator.nightly \
        --seconds 45 --label mesa27 --out results/games

Finds the app's SurfaceView layer, polls `dumpsys SurfaceFlinger --latency`
(128 presents per call) often enough not to miss frames, de-duplicates the
present timestamps, and samples KGSL gpu_busy_percentage / clock / temp.
Prints and writes (CSV of frame intervals + summary) average FPS, p50 / p95 /
p99 frame time, share of frames over 1.5x p50 and over 33.4 ms, GPU busy,
clock and temperature. No root needed.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys
import time

ADB = os.environ.get("ADB", "adb")
INT64_MAX = 9223372036854775807


def sh(cmd, timeout=20):
    r = subprocess.run([ADB, "shell", cmd], capture_output=True, text=True,
                       errors="replace", timeout=timeout)
    return r.stdout


def layer_has_frames(name):
    out = sh(f"dumpsys SurfaceFlinger --latency '{name}'")
    for line in out.splitlines()[1:]:
        f = line.split()
        if len(f) == 3 and 0 < int(f[1]) < INT64_MAX:
            return True
    return False


def find_layer(pkg):
    out = sh("dumpsys SurfaceFlinger --list")
    names = []
    for line in out.splitlines():
        s = line.strip()
        # Android 15: "RequestedLayerState{<name> parentId=... z=...}"
        m = re.match(r"RequestedLayerState\{(.*?)(?: parentId=| relativeParentId=| z=|\}$)", s)
        if m:
            s = m.group(1)
        if ("SurfaceView" in s and pkg in s and not s.startswith("Background"))                 or s.startswith("gndrm-scanout"):
            names.append(s)
    # the BLAST buffer layer carries the presents; fall back to any with data
    names.sort(key=lambda n: "(BLAST)" not in n)
    for n in names:
        if layer_has_frames(n):
            return n
    return names[0] if names else None


def read_sysfs(path):
    try:
        return sh(f"cat {path}").strip()
    except Exception:
        return ""


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    k = min(len(v) - 1, max(0, int(round(p / 100.0 * (len(v) - 1)))))
    return v[k]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--label", default="run")
    ap.add_argument("--out", default="results/games")
    ap.add_argument("--layer", help="exact layer name (e.g. GameNative's gndrm-scanout#NNNN)")
    ap.add_argument("--stop-file", help="end the capture early as soon as this local file exists")
    a = ap.parse_args()

    layer = a.layer or find_layer(a.pkg)
    if not layer:
        sys.exit(f"no SurfaceView layer for {a.pkg} - is the game on screen?")
    print(f"layer: {layer}")
    sh(f"dumpsys SurfaceFlinger --latency-clear '{layer}'")

    presents = set()
    busy, clocks, temps = [], [], []
    t_end = time.time() + a.seconds
    while time.time() < t_end:
        if a.stop_file and os.path.exists(a.stop_file):
            print("stop file found, ending capture")
            break
        out = sh(f"dumpsys SurfaceFlinger --latency '{layer}'")
        for line in out.splitlines()[1:]:
            f = line.split()
            if len(f) == 3:
                p = int(f[1])
                if 0 < p < INT64_MAX:
                    presents.add(p)
        b = read_sysfs("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage")
        c = read_sysfs("/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq")
        t = read_sysfs("/sys/class/kgsl/kgsl-3d0/temp")
        try:
            busy.append(int(b.split()[0]))
            clocks.append(int(c) / 1e6)
            temps.append(int(t) / 1000.0)
        except (ValueError, IndexError):
            pass
        time.sleep(1.0)

    ts = sorted(presents)
    iv = [(b - a_) / 1e6 for a_, b in zip(ts, ts[1:]) if 0 < (b - a_) < 1e9]
    if len(iv) < 10:
        sys.exit("too few frames captured")
    span = (ts[-1] - ts[0]) / 1e9
    p50 = statistics.median(iv)
    summary = {
        "label": a.label,
        "frames": len(iv),
        "seconds": round(span, 1),
        "avg_fps": round(len(iv) / span, 2),
        "p50_ms": round(p50, 2),
        "p95_ms": round(pct(iv, 95), 2),
        "p99_ms": round(pct(iv, 99), 2),
        "slow_pct": round(100.0 * sum(1 for x in iv if x > 1.5 * p50) / len(iv), 2),
        "over33_pct": round(100.0 * sum(1 for x in iv if x > 33.4) / len(iv), 2),
        "gpu_busy_pct": round(statistics.mean(busy), 1) if busy else None,
        "gpu_mhz": round(statistics.mean(clocks)) if clocks else None,
        "gpu_temp_c": round(statistics.mean(temps), 1) if temps else None,
    }
    os.makedirs(a.out, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    with open(os.path.join(a.out, f"{a.label}-{stamp}.csv"), "w") as f:
        f.write("interval_ms\n" + "\n".join(f"{x:.3f}" for x in iv) + "\n")
    line = "  ".join(f"{k}={v}" for k, v in summary.items())
    with open(os.path.join(a.out, "summary.txt"), "a") as f:
        f.write(f"{stamp}  {line}\n")
    print(line)


if __name__ == "__main__":
    main()
