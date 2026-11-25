#!/usr/bin/env python3
"""Run dEQP-VK on the device against a Turnip build, surviving crashes.

    python3 scripts/run_deqp.py --driver dist/turnip-a22-26.3.0.adpkg.zip \
        --filter 'dEQP-VK.renderpass.*' --env TU_DEBUG=gmem --out results/gmem

The driver may be a .so or an .adpkg.zip. deqp-vk is pushed once to
/data/local/tmp/deqp (from $DEQP_OUT, see scripts/build_deqp.sh). The case list
is expanded on the device, then executed in chunks; when the process dies the
case that was running is recorded as Crash and the run resumes after it.

Outputs in --out: results.csv (case,status,detail), summary.txt, and the raw
device logs. Compare two runs with --baseline <other results.csv>.
"""
import argparse
import csv
import fnmatch
import os
import re
import subprocess
import sys
import tempfile
import time
import zipfile

DEV_DIR = "/data/local/tmp/deqp"
CASE_RE = re.compile(r"^Test case '([^']+)'\.\.")
STATUS_RE = re.compile(
    r"^\s+(Pass|Fail|NotSupported|QualityWarning|CompatibilityWarning|"
    r"InternalError|ResourceError|Crash|Timeout|Waiver)\s*\((.*)\)\s*$")
FAILING = {"Fail", "Crash", "Timeout", "InternalError", "ResourceError"}


def adb(*args, check=True, capture=True, timeout=None):
    cmd = [os.environ.get("ADB", "adb")] + list(args)
    r = subprocess.run(cmd, capture_output=capture, text=True, timeout=timeout,
                       errors="replace")
    if check and r.returncode != 0:
        sys.exit(f"adb {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
    return r


def push_driver(path, shim):
    # Turnip's Android build exports only HMI; deqp-vk needs vkGetInstanceProcAddr,
    # which tools/probe/vkhal_shim.c provides (it loads libvulkan_test.so next to it).
    if not os.path.isfile(shim):
        sys.exit(f"no shim at {shim} (run scripts/build_probes.sh)")
    adb("push", shim, f"{DEV_DIR}/libvkhal_shim.so")
    with tempfile.TemporaryDirectory() as tmp:
        so = path
        if path.endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                z.extract("libvulkan_freedreno.so", tmp)
            so = os.path.join(tmp, "libvulkan_freedreno.so")
        adb("push", so, f"{DEV_DIR}/libvulkan_test.so")


def push_deqp(deqp_out, force):
    have = adb("shell", f"test -x {DEV_DIR}/deqp-vk && echo yes", check=False).stdout
    if "yes" in have and not force:
        return
    print(f"pushing dEQP from {deqp_out} (one-time, large)...")
    adb("shell", f"rm -rf {DEV_DIR} && mkdir -p {DEV_DIR}")
    adb("push", os.path.join(deqp_out, "deqp-vk"), f"{DEV_DIR}/")
    adb("push", os.path.join(deqp_out, "vulkan"), f"{DEV_DIR}/")
    adb("shell", f"chmod 755 {DEV_DIR}/deqp-vk")


def env_prefix(envs):
    return " ".join(f"{e}" for e in envs)


def deqp_cmd(envs, extra):
    return (f"cd {DEV_DIR} && {env_prefix(envs)} ./deqp-vk "
            f"--deqp-vk-library-path={DEV_DIR}/libvkhal_shim.so "
            f"--deqp-archive-dir={DEV_DIR} --deqp-log-images=disable "
            f"--deqp-log-shader-sources=disable --deqp-log-flush=disable "
            f"--deqp-log-filename=/dev/null {extra}")


def list_cases(envs, pattern, mustpass):
    if mustpass:
        with open(mustpass) as f:
            cases = [l.strip() for l in f if l.strip() and not l.startswith("#")]
        return [c for c in cases if fnmatch.fnmatchcase(c, pattern)]
    adb("shell", deqp_cmd(envs, f"--deqp-runmode=txt-caselist "
                                f"--deqp-case='{pattern}' >/dev/null 2>&1"),
        check=False)
    out = adb("shell", f"cat {DEV_DIR}/dEQP-VK-cases.txt").stdout
    return [l[6:].strip() for l in out.splitlines() if l.startswith("TEST: ")]


def run_chunk(envs, cases, timeout, raw):
    adb("shell", f"rm -f {DEV_DIR}/caselist.txt")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(cases) + "\n")
        local = f.name
    adb("push", local, f"{DEV_DIR}/caselist.txt")
    os.unlink(local)
    try:
        r = adb("shell", deqp_cmd(envs, f"--deqp-caselist-file={DEV_DIR}/caselist.txt") + " 2>&1",
                check=False, timeout=timeout)
        out, timed_out = r.stdout, False
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        timed_out = True
        adb("shell", "pkill -9 deqp-vk", check=False)
    raw.write(out)
    results, current = {}, None
    for line in out.splitlines():
        m = CASE_RE.match(line)
        if m:
            current = m.group(1)
            continue
        m = STATUS_RE.match(line)
        if m and current:
            results[current] = (m.group(1), m.group(2))
            current = None
    if current:  # process died (or timed out) inside this case
        results[current] = ("Timeout" if timed_out else "Crash", "process died")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", required=True, help=".so or .adpkg.zip")
    ap.add_argument("--filter", default="dEQP-VK.*", help="glob over case names")
    ap.add_argument("--mustpass", help="take cases from a mustpass txt instead of the device")
    ap.add_argument("--env", action="append", default=[], help="VAR=value, repeatable")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chunk", type=int, default=500)
    ap.add_argument("--timeout", type=int, default=600, help="seconds per chunk")
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    default_deqp = (os.path.join(repo, "build", "deqp") if os.name == "nt"
                    else os.path.expanduser("~/turnip-dev/deqp"))
    ap.add_argument("--deqp-out", default=os.environ.get("DEQP_OUT", default_deqp))
    ap.add_argument("--shim", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "build", "probe-tools", "libvkhal_shim.so"))
    ap.add_argument("--repush", action="store_true", help="re-push deqp-vk and data")
    ap.add_argument("--baseline", help="results.csv of a previous run to diff against")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    push_deqp(a.deqp_out, a.repush)
    push_driver(a.driver, a.shim)
    for e in a.env:
        if "=" not in e:
            sys.exit(f"--env expects VAR=value, got {e}")

    cases = list_cases(a.env, a.filter, a.mustpass)
    if not cases:
        sys.exit("no cases matched")
    print(f"{len(cases)} cases, env: {' '.join(a.env) or '(none)'}")

    results = {}
    t0 = time.time()
    with open(os.path.join(a.out, "raw.log"), "w") as raw:
        pending = list(cases)
        while pending:
            chunk = pending[:a.chunk]
            got = run_chunk(a.env, chunk, a.timeout, raw)
            results.update(got)
            if not got:  # nothing ran at all: mark first as crash to make progress
                results[chunk[0]] = ("Crash", "no output")
            done_upto = max(chunk.index(c) for c in got) + 1 if got else 1
            pending = chunk[done_upto:] + pending[a.chunk:]
            n = len(results)
            fails = sum(1 for s, _ in results.values() if s in FAILING)
            print(f"  {n}/{len(cases)}  failing={fails}  {time.time()-t0:.0f}s", flush=True)

    for c in cases:
        results.setdefault(c, ("Missing", "not reported"))
    with open(os.path.join(a.out, "results.csv"), "w", newline="") as f:
        w = csv.writer(f)
        for c in cases:
            w.writerow([c, *results[c]])

    counts = {}
    for s, _ in results.values():
        counts[s] = counts.get(s, 0) + 1
    lines = [f"driver: {a.driver}", f"filter: {a.filter}", f"env: {' '.join(a.env)}",
             f"time: {time.time()-t0:.0f}s", ""] + \
            [f"{k:22s} {v}" for k, v in sorted(counts.items())] + [""] + \
            [f"{s:14s} {c}" for c in cases for s in [results[c][0]] if s in FAILING]

    if a.baseline:
        base = {}
        with open(a.baseline) as f:
            for row in csv.reader(f):
                base[row[0]] = row[1]
        regress = [c for c in cases if results[c][0] in FAILING and base.get(c) not in FAILING | {None}]
        fixed = [c for c in cases if results[c][0] not in FAILING and base.get(c) in FAILING]
        lines += ["", f"vs baseline {a.baseline}: {len(regress)} new failures, {len(fixed)} fixed"]
        lines += [f"  NEW  {results[c][0]:10s} {c}" for c in regress]
        lines += [f"  FIX  {c}" for c in fixed]

    summary = "\n".join(lines) + "\n"
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write(summary)
    print(summary)


if __name__ == "__main__":
    main()
