#!/usr/bin/env python3
"""Summarise a Turnip u_trace "print" capture by render-pass shape.

    python scripts/tu_trace_summary.py trace.txt [--top 40] [--csv out.csv]

Capture on Android without root (the app writes the file itself):

    adb shell setprop debug.mesa.gpu.traces print
    adb shell setprop debug.mesa.gpu.tracefile /sdcard/Android/data/<pkg>/files/tu.txt
    # (re)start the app, play, then clear both properties

Each render pass (start_render_pass .. end_render_pass) is keyed by its
shape: framebuffer size, attachment count, samples, clear/load/store bytes
per pixel, depth, UBWC flags, bin layout and the formats of its GMEM
loads/stores/clears. For every shape the table shows how often it occurs,
how often it was rendered tiled (GMEM), the render-mode reasons and draws.
"""
import argparse
import collections
import csv
import re
import sys

KV = re.compile(r"(\w+)=([^,]*)")
NAME = re.compile(r"^\s*\d+\s+\+\d+:\s+(\w+)\s*(.*)$")

SHAPE_KEYS = ("width", "height", "attachment_count", "maxSamples", "clearCPP",
              "loadCPP", "storeCPP", "hasDepth", "ubwc", "numberOfBins",
              "binWidth", "binHeight")


def parse(path):
    passes = []
    cur = None
    with open(path, errors="replace") as f:
        for line in f:
            m = NAME.match(line)
            if not m:
                continue
            name, rest = m.group(1), m.group(2)
            if name == "start_render_pass":
                cur = dict(KV.findall(rest))
                cur["loads"], cur["stores"], cur["clears"] = [], [], []
            elif cur is None:
                continue
            elif name == "start_gmem_load":
                kv = dict(KV.findall(rest))
                cur["loads"].append(kv.get("format", "?"))
            elif name == "start_gmem_store":
                kv = dict(KV.findall(rest))
                fmt = kv.get("format", "?")
                if kv.get("unaligned") == "1":
                    fmt += "(unaligned)"
                cur["stores"].append(fmt)
            elif name in ("start_gmem_clear", "start_sysmem_clear"):
                kv = dict(KV.findall(rest))
                cur["clears"].append(kv.get("format", "?"))
            elif name == "end_render_pass":
                cur.update(dict(KV.findall(rest)))
                passes.append(cur)
                cur = None
    return passes


def shape_of(p):
    base = tuple(p.get(k, "?") for k in SHAPE_KEYS)
    fmts = ("L:" + "+".join(sorted(set(p["loads"]))),
            "S:" + "+".join(sorted(set(p["stores"]))),
            "C:" + "+".join(sorted(set(p["clears"]))))
    return base + fmts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--csv")
    a = ap.parse_args()

    passes = parse(a.trace)
    if not passes:
        sys.exit("no render passes found")
    groups = collections.OrderedDict()
    for p in passes:
        g = groups.setdefault(shape_of(p), {"n": 0, "tiled": 0, "draws": 0,
                                             "reasons": collections.Counter()})
        g["n"] += 1
        g["tiled"] += p.get("tiledRender") == "true"
        g["draws"] += int(p.get("drawCount", "0") or 0)
        g["reasons"][p.get("forceRenderModeReason", "") or "(autotune)"] += 1

    tiled = sum(1 for p in passes if p.get("tiledRender") == "true")
    print(f"{len(passes)} render passes, {tiled} tiled (GMEM), "
          f"{len(passes) - tiled} sysmem, {len(groups)} distinct shapes\n")
    rows = sorted(groups.items(), key=lambda kv: -kv[1]["n"])
    hdr = (f"{'count':>6} {'gmem':>6} {'draws/rp':>8}  {'WxH':>10} att smp "
           f"clr/ld/st dep ubwc        bins(w x h)    formats / reasons")
    print(hdr)
    print("-" * len(hdr))
    for shape, g in rows[:a.top]:
        (w, h, att, smp, ccpp, lcpp, scpp, dep, ubwc, nb, bw, bh, L, S, C) = shape
        reasons = "; ".join(f"{r} x{n}" for r, n in g["reasons"].most_common(2))
        print(f"{g['n']:6d} {g['tiled']:6d} {g['draws'] / g['n']:8.1f}  "
              f"{w + 'x' + h:>10} {att:>3} {smp:>3} {ccpp:>3}/{lcpp:>2}/{scpp:<2} "
              f"{dep[:1]:>3} {ubwc:<11} {nb:>4}({bw}x{bh})  {L} {S} {C}  [{reasons}]")

    if a.csv:
        with open(a.csv, "w", newline="") as f:
            wr = csv.writer(f)
            wr.writerow(list(SHAPE_KEYS) + ["loads", "stores", "clears", "count",
                                             "tiled", "draws", "reasons"])
            for shape, g in rows:
                wr.writerow(list(shape) + [g["n"], g["tiled"], g["draws"],
                                           "; ".join(f"{r} x{n}" for r, n in
                                                     g["reasons"].most_common())])


if __name__ == "__main__":
    main()
