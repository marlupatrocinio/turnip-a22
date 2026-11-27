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
            elif name == "start_draw":
                kv = dict(KV.findall(rest))
                cur.setdefault("draws", []).append(
                    tuple(kv.get(k, "") for k in ("vs_sha1", "tcs_sha1", "tes_sha1",
                                                   "gs_sha1", "fs_sha1")))
            elif name.startswith("start_") and name not in (
                    "start_draw_ib_sysmem", "start_draw_ib_gmem", "start_binning_ib",
                    "start_concurrent_binning_ib", "start_concurrent_binning_barrier"):
                cur.setdefault("ops", collections.Counter())[name[6:]] += 1
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


def draw_report(passes, groups, top):
    """Per-draw view (needs TU_GPU_TRACEPOINT=+draw): pipelines by stage set,
    and what the multi-bin passes contain."""
    stage_names = ("vs", "tcs", "tes", "gs", "fs")
    stages_all = collections.Counter()
    multi = [p for p in passes if int(p.get("numberOfBins", "1") or 1) > 1]
    print(f"\n== draw level: {sum(len(p.get('draws', [])) for p in passes)} draws in "
          f"{len(passes)} passes; {len(multi)} multi-bin passes")
    for p in passes:
        for d in p.get("draws", []):
            stages_all["+".join(n for n, s in zip(stage_names, d) if s)] += 1
    print("stage sets over all draws:", dict(stages_all.most_common()))

    by_shape = collections.OrderedDict()
    for p in multi:
        s = shape_of(p)
        e = by_shape.setdefault(s, {"n": 0, "draws": 0, "stages": collections.Counter(),
                                    "pipes": collections.Counter(),
                                    "ops": collections.Counter()})
        e["n"] += 1
        for d in p.get("draws", []):
            e["draws"] += 1
            e["stages"]["+".join(n for n, x in zip(stage_names, d) if x)] += 1
            e["pipes"][d] += 1
        e["ops"].update(p.get("ops", {}))
    for s, e in sorted(by_shape.items(), key=lambda kv: -kv[1]["draws"])[:top]:
        w, h, att, smp = s[0], s[1], s[2], s[3]
        print(f"\n  {w}x{h} att={att} samples={smp} bins={s[9]} ubwc={s[8]}  "
              f"passes={e['n']} draws={e['draws']} distinct pipelines={len(e['pipes'])}")
        print(f"    stages: {dict(e['stages'].most_common())}")
        if e["ops"]:
            print(f"    in-pass ops: {dict(e['ops'].most_common())}")
        gs = [d for d in e["pipes"] if d[3]]
        if gs:
            print(f"    geometry-shader pipelines: {len(gs)}, e.g. gs={gs[0][3][:16]}")


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

    if any("draws" in p for p in passes):
        draw_report(passes, groups, a.top)

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
