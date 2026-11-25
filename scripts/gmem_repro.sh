#!/usr/bin/env bash
# GMEM vs SYSMEM reproducer sweep on the device (tools/probe/a22gmem.c).
#
#   bash scripts/gmem_repro.sh <driver.so|adpkg.zip> [W H] [-- extra env ...]
#
# For each case it renders:
#   sysmem twice      -> must be identical (determinism check)
#   gmem              -> compared with sysmem (the bug check)
#   default autotune  -> compared with sysmem
# and reports new KGSL kernel messages (GPU faults) per case.
#
# Cases come from $GM_CASES (one "FORMATS DEPTH DRAWS" per line) or the
# built-in list below. Extra env (e.g. TU_DEBUG flags to combine) via
# GM_EXTRA_DEBUG, appended to every TU_DEBUG value.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export MSYS_NO_PATHCONV=1
    localpath() { cygpath -w "$1"; } ;; *) localpath() { printf '%s' "$1"; } ;; esac
ADB="${ADB:-adb}"
DRV="${1:?usage: gmem_repro.sh <driver.so|adpkg.zip> [W H]}"
W="${2:-1920}"; H="${3:-1080}"
BIN="$HERE/build/probe-tools/a22gmem"
DEV=/data/local/tmp/gmem
X="${GM_EXTRA_DEBUG:+,$GM_EXTRA_DEBUG}"

[ -f "$BIN" ] || { echo "build it first: bash scripts/build_probes.sh" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
SO="$DRV"
if [[ "$DRV" == *.zip ]]; then unzip -q -o "$DRV" libvulkan_freedreno.so -d "$TMP"; SO="$TMP/libvulkan_freedreno.so"; fi

"$ADB" shell "mkdir -p $DEV" >/dev/null
"$ADB" push "$(localpath "$SO")" $DEV/lib.so >/dev/null
"$ADB" push "$(localpath "$BIN")" $DEV/a22gmem >/dev/null
"$ADB" shell "chmod 755 $DEV/a22gmem"

CASES="${GM_CASES:-rgba16f,r11g11b10,rgba8,rgb10a2 d32s8 300
rgba8,rgba8,rgba8,rgba8 d32s8 300
rgba16f,rgba16f,rgba16f,rgba16f d32s8 300
rgba32f,rgba32f,rgba32f,rgba32f d32s8 300
rgba16f,r11g11b10,rgba8,rgb10a2 d24s8 300
rgba16f,r11g11b10,rgba8,rgb10a2 d32s8 2000}"

kmsg() { "$ADB" shell -n "dmesg 2>/dev/null | grep -c kgsl" | tr -d '\r'; }

echo "driver $DRV  frame ${W}x${H}  mode ${GM_MODE:-subpass}  area ${GM_AREA:-default}  post ${GM_POST:-1}  extra TU_DEBUG '${GM_EXTRA_DEBUG:-}'"
fails=0
while read -r fmts depth draws; do
    [ -z "$fmts" ] && continue
    env="GM_FORMATS=$fmts GM_DEPTH=$depth GM_DRAWS=$draws GM_MODE=${GM_MODE:-subpass}"
    [ -n "${GM_AREA:-}" ] && env="$env GM_AREA=$GM_AREA"
    [ -n "${GM_POST:-}" ] && env="$env GM_POST=$GM_POST"
    [ -n "${GM_MUTABLE:-}" ] && env="$env GM_MUTABLE=$GM_MUTABLE"
    [ -n "${GM_REPEAT:-}" ] && env="$env GM_REPEAT=$GM_REPEAT"
    run() { "$ADB" shell -n "cd $DEV && $env TU_DEBUG=$1 ./a22gmem ./lib.so $2 $W $H" | grep -E "FAILED|missing|not a colour|wrote" || true; }
    cmp() { "$ADB" shell -n "cd $DEV && ./a22gmem --compare $1 $2" || true; }
    k0=$(kmsg)
    run "sysmem$X" sys1.bin >/dev/null
    run "sysmem$X" sys2.bin >/dev/null
    run "gmem$X" gmem.bin >/dev/null
    run "${GM_EXTRA_DEBUG:-}" auto.bin >/dev/null
    k1=$(kmsg)
    det=$(cmp sys1.bin sys2.bin | tail -1)
    g=$(cmp sys1.bin gmem.bin)
    a=$(cmp sys1.bin auto.bin | tail -1)
    echo "== $fmts $depth draws=$draws   kgsl msgs +$((k1 - k0))"
    echo "   determinism (sysmem vs sysmem): $det"
    echo "   gmem vs sysmem:                 $(tail -1 <<<"$g")"
    grep -E "DIFF| a=" <<<"$g" | sed 's/^/     /' || true
    echo "   autotune vs sysmem:             $a"
    grep -q MISMATCH <<<"$g" && fails=$((fails + 1))
done <<<"$CASES"
"$ADB" shell "rm -f $DEV/*.bin" >/dev/null
echo "cases with gmem mismatch: $fails"
