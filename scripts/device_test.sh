#!/usr/bin/env bash
# Release gate: install a packaged driver on a connected device and verify it.
#
#   ./scripts/device_test.sh <turnip-a22-*.adpkg.zip> [path/to/a22test]
#
# Exits non-zero unless the driver loads and both functional tests pass.
# Requires: adb (device authorised), unzip, the a22test binary for arm64
# (build it with scripts/build_probes.sh).
set -euo pipefail

ZIP="${1:?usage: device_test.sh <adpkg.zip> [a22test]}"
TEST_BIN="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/probe-tools/a22test}"
ADB="${ADB:-adb}"
SERIAL="${ANDROID_SERIAL:-}"

[ -f "$ZIP" ] || { echo "no such package: $ZIP" >&2; exit 1; }
[ -f "$TEST_BIN" ] || { echo "no a22test binary at $TEST_BIN (run scripts/build_probes.sh)" >&2; exit 1; }

adb_cmd() { if [ -n "$SERIAL" ]; then "$ADB" -s "$SERIAL" "$@"; else "$ADB" "$@"; fi; }

echo "== device =="
adb_cmd devices -l
adb_cmd shell getprop ro.soc.model

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
unzip -q "$ZIP" -d "$WORK"
SO="$WORK/libvulkan_freedreno.so"
[ -f "$SO" ] || { echo "package has no libvulkan_freedreno.so" >&2; exit 1; }
grep -q '"libraryName": "libvulkan_freedreno.so"' "$WORK/meta.json" \
    || { echo "meta.json libraryName mismatch" >&2; exit 1; }

echo "== pushing =="
adb_cmd push "$SO" /data/local/tmp/libvulkan_freedreno_gate.so >/dev/null
adb_cmd push "$TEST_BIN" /data/local/tmp/a22test_gate >/dev/null
adb_cmd shell chmod 755 /data/local/tmp/a22test_gate

echo "== running functional gate =="
set +e
OUT="$(adb_cmd shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22test_gate /data/local/tmp/libvulkan_freedreno_gate.so" 2>&1)"
RC=$?
set -e
echo "$OUT"
adb_cmd shell rm -f /data/local/tmp/libvulkan_freedreno_gate.so /data/local/tmp/a22test_gate >/dev/null 2>&1 || true

if [ $RC -ne 0 ]; then
    echo "FAIL: test binary exited $RC" >&2
    exit 1
fi
grep -q "compute dispatch: PASS" <<<"$OUT" || { echo "FAIL: compute dispatch" >&2; exit 1; }
grep -q "image clear+resolve: PASS" <<<"$OUT" || { echo "FAIL: image clear/resolve" >&2; exit 1; }
echo "PASS: driver gate satisfied"
