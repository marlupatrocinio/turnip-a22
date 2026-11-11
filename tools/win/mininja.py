#!/usr/bin/env python3
"""Minimal ninja build executor.

The DSH Windows sandbox blocks ninja from spawning child processes (ninja
stays at 0% CPU forever), although CreateProcess itself works fine from
Python.  This script parses Meson's generated build.ninja, expands the
rules/variables exactly like ninja does for the subset Meson emits, writes
rspfiles, and runs the commands with a small parallel scheduler.

Usage:
    python tools/mininja.py <build-dir> [--target OUTPUT] [-j N] [--dry-run]
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys
import threading
import time

sys.setrecursionlimit(10000)


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------
def read_logical_lines(path):
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read().splitlines()
    i = 0
    while i < len(raw):
        line = raw[i]
        while line.endswith("$") and (len(line) - len(line.rstrip("$"))) % 2 == 1:
            i += 1
            if i >= len(raw):
                break
            nxt = raw[i]
            line = line[:-1] + nxt.lstrip()
        out.append(line)
        i += 1
    return out


def split_tokens(s):
    """Split a ninja path list on unescaped spaces, keeping $ / : escapes."""
    tokens = []
    cur = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "$" and i + 1 < len(s):
            n = s[i + 1]
            if n == "$":
                cur.append("$")
            elif n == " ":
                cur.append("\0SPACE\0")
            elif n == ":":
                cur.append("\0COLON\0")
            else:
                cur.append(c + n)
            i += 2
            continue
        if c == " " or c == "\t":
            if cur:
                tokens.append("".join(cur))
                cur = []
            i += 1
            continue
        cur.append(c)
        i += 1
    if cur:
        tokens.append("".join(cur))
    return tokens


class Edge:
    __slots__ = ("rule", "outputs", "inputs", "implicit", "order_only", "vars", "line")

    def __init__(self, rule, outputs, inputs, implicit, order_only, variables, line):
        self.rule = rule
        self.outputs = outputs
        self.inputs = inputs
        self.implicit = implicit
        self.order_only = order_only
        self.vars = variables
        self.line = line

    @property
    def all_inputs(self):
        return self.inputs + self.implicit + self.order_only


class Build:
    def __init__(self, builddir):
        self.builddir = os.path.abspath(builddir)
        self.rules = {}
        self.edges = []
        self.out_map = {}
        self.defaults = []
        self._parse(os.path.join(self.builddir, "build.ninja"))

    def _parse(self, path):
        lines = read_logical_lines(path)
        cur_rule = None
        cur_edge = None
        for line in lines:
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if line.startswith("rule "):
                name = line[5:].strip()
                cur_rule = name
                self.rules[cur_rule] = {}
                cur_edge = None
                continue
            if line.startswith("build "):
                body = line[6:]
                # outputs ':' rule inputs
                m = re.match(r"(.*?)\s*:\s*(\S+)\s*(.*)$", body)
                if not m:
                    continue
                outs_s, rule, rest = m.groups()
                outputs = [t.replace("\0SPACE\0", " ").replace("\0COLON\0", ":")
                           for t in split_tokens(outs_s)]
                inputs, implicit, order_only = [], [], []
                mode = 0
                for tok in split_tokens(rest):
                    tok = tok.replace("\0SPACE\0", " ").replace("\0COLON\0", ":")
                    if tok == "|":
                        mode = 1
                        continue
                    if tok == "||":
                        mode = 2
                        continue
                    if mode == 0:
                        inputs.append(tok)
                    elif mode == 1:
                        implicit.append(tok)
                    else:
                        order_only.append(tok)
                cur_edge = Edge(rule, outputs, inputs, implicit, order_only, {}, line)
                self.edges.append(cur_edge)
                for o in outputs:
                    self.out_map[o] = cur_edge
                continue
            if line.startswith("default "):
                self.defaults.extend(split_tokens(line[8:]))
                continue
            if line[0] in " \t" and "=" in line:
                k, v = line.strip().split("=", 1)
                k = k.strip()
                v = v.strip()
                if cur_edge is not None:
                    cur_edge.vars[k] = v
                elif cur_rule is not None:
                    self.rules[cur_rule][k] = v
                continue
            # other top-level (pool, etc.) -> ignore block contents
            if not line[0].isspace():
                cur_rule = None
                cur_edge = None

    # -- variable expansion ------------------------------------------------
    def expand(self, text, edge, extra=None, rule_vars=None, _depth=0):
        out = []
        i = 0
        lookup = {}
        if rule_vars:
            lookup.update(rule_vars)
        lookup.update(edge.vars)
        if extra:
            lookup.update(extra)
        if _depth > 20:
            return text

        def sub(name):
            val = lookup.get(name, "")
            if "$" in val:
                return self.expand(val, edge, extra, rule_vars, _depth + 1)
            return val

        while i < len(text):
            c = text[i]
            if c != "$":
                out.append(c)
                i += 1
                continue
            if i + 1 >= len(text):
                out.append("$")
                break
            n = text[i + 1]
            if n == "$":
                out.append("$")
                i += 2
                continue
            if n == " ":
                out.append(" ")
                i += 2
                continue
            if n == ":":
                out.append(":")
                i += 2
                continue
            if n == "\n":
                i += 2
                continue
            if n == "{":
                j = text.index("}", i + 2)
                name = text[i + 2:j]
                out.append(sub(name))
                i = j + 1
                continue
            # bare name
            m = re.match(r"[A-Za-z0-9_]+", text[i + 1:])
            if not m:
                out.append(n)
                i += 2
                continue
            name = m.group(0)
            out.append(sub(name))
            i += 1 + len(name)
        return "".join(out)

    def command_for(self, edge):
        rule = self.rules.get(edge.rule)
        if rule is None:
            return None, None, None
        if edge.rule == "phony":
            return None, None, None

        def shell_join(paths):
            if not paths:
                return ""
            return " ".join(
                ('"%s"' % p) if (" " in p or "\t" in p) else p for p in paths)

        extra = {
            "in": shell_join(edge.inputs),
            "in_newline": "\n".join(edge.inputs),
            "out": shell_join(edge.outputs),
        }
        cmd = self.expand(rule.get("command", ""), edge, extra, rule)
        rspfile = rule.get("rspfile")
        rspcontent = rule.get("rspfile_content")
        if rspfile:
            rspfile = self.expand(rspfile, edge, extra, rule)
            rspcontent = self.expand(rspcontent, edge, extra, rule) if rspcontent else ""
        return cmd, rspfile, rspcontent


# --------------------------------------------------------------------------
# execution
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("builddir")
    ap.add_argument("--target", action="append", default=[])
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--keep-going", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    build = Build(args.builddir)
    print(f"mininja: {len(build.edges)} edges, {len(build.rules)} rules")

    edges = [e for e in build.edges if e.rule != "phony"]

    if args.target:
        selected = set()
        stack = list(args.target)
        while stack:
            t = stack.pop()
            e = build.out_map.get(t)
            if e is None or id(e) in selected:
                continue
            selected.add(id(e))
            stack.extend(e.all_inputs)
        edges = [e for e in edges if id(e) in selected]

    # dependency graph
    deps = {}
    dependents = {}
    for e in edges:
        d = set()
        for inp in e.all_inputs:
            pe = build.out_map.get(inp)
            if pe is not None and pe.rule != "phony" and pe is not e:
                d.add(id(pe))
        deps[id(e)] = d
        for pid in d:
            dependents.setdefault(pid, []).append(id(e))

    by_id = {id(e): e for e in edges}
    pending = {eid: len(d) for eid, d in deps.items()}
    done = set()
    failed = []
    lock = threading.Lock()
    tpool = concurrent.futures.ThreadPoolExecutor(max_workers=args.j)
    futures = {}

    def up_to_date(edge):
        outs = [os.path.join(build.builddir, o) for o in edge.outputs]
        ins = [os.path.join(build.builddir, i) for i in edge.all_inputs]
        if not all(os.path.exists(o) for o in outs):
            return False
        # A zero-byte output is a failed generation, not an up-to-date result.
        if any(os.path.getsize(o) == 0 for o in outs):
            return False
        try:
            newest_out = max(os.path.getmtime(o) for o in outs)
            newest_in = max((os.path.getmtime(i) for i in ins if os.path.exists(i)), default=0)
        except OSError:
            return False
        return newest_out >= newest_in

    def run_edge(eid):
        e = by_id[eid]
        if up_to_date(e):
            return eid, True, "up-to-date"
        cmd, rspfile, rspcontent = build.command_for(e)
        if cmd is None:
            return eid, True, "phony"
        if args.dry_run:
            return eid, True, "DRY: " + cmd[:200]
        if rspfile:
            p = os.path.join(build.builddir, rspfile)
            os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(rspcontent)
        r = subprocess.run(cmd, shell=True, cwd=build.builddir,
                           capture_output=True, text=True, errors="replace")
        ok = r.returncode == 0
        return eid, ok, r.stdout + r.stderr

    ready = [eid for eid, n in pending.items() if n == 0]
    total = len(pending)
    completed = 0
    lock = threading.Lock()

    def submit(eid):
        fut = tpool.submit(run_edge, eid)
        futures[fut] = eid

    for eid in ready:
        submit(eid)

    try:
        while futures:
            done_set, _ = concurrent.futures.wait(
                list(futures.keys()), return_when=concurrent.futures.FIRST_COMPLETED)
            for fut in done_set:
                eid = futures.pop(fut)
                try:
                    _, ok, output = fut.result()
                except Exception as ex:  # noqa: BLE001
                    ok, output = False, f"internal error: {ex}"
                with lock:
                    completed += 1
                    if not ok:
                        failed.append((by_id[eid], output))
                        print(f"FAILED ({completed}/{total}) {by_id[eid].outputs[0]}")
                        print(output[-4000:])
                        if not args.keep_going:
                            raise SystemExit(1)
                    else:
                        if completed % 25 == 0 or completed == total:
                            print(f"[{completed}/{total}] {time.time()-t0:.0f}s "
                                  f"{by_id[eid].outputs[0]}")
                    for did in dependents.get(eid, []):
                        pending[did] -= 1
                        if pending[did] == 0:
                            submit(did)
    finally:
        tpool.shutdown(wait=False, cancel_futures=True)

    print(f"mininja: done in {time.time()-t0:.0f}s, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())


