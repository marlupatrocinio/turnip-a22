#!/usr/bin/env bash
# Build the on-device probe/test/benchmark binaries used by the release gate.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_VERSION="${NDK_VERSION:-r28c}"
API="${API:-35}"
NDK="${NDK:-$HERE/src/android-ndk-$NDK_VERSION}"
OUT="${OUT:-$HERE/build/probe-tools}"
PROBE="$HERE/tools/probe"

case "$(uname -s)" in
    Linux*)  HOST_TAG=linux-x86_64 ;;
    Darwin*) HOST_TAG=darwin-x86_64 ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
    *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

BIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$BIN/clang"
[ -x "$CC" ] || CC="$CC.exe"
SYSROOT="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/sysroot"
[ -x "$CC" ] || { echo "clang not found under $BIN" >&2; exit 1; }

mkdir -p "$OUT"

# SPIR-V headers are generated, not committed. They are needed by a22test
# (compute shader) and a22bench (vertex/fragment pair).
if ! command -v glslangValidator >/dev/null; then
    echo "glslangValidator not found; required to build the test shaders" >&2
    exit 1
fi
glslangValidator -V --vn a22_comp_spv -o "$PROBE/a22_comp_spv.h" "$PROBE/a22_compute.comp" >/dev/null
glslangValidator -V --vn bench_vert_spv -o "$PROBE/bench_vert_spv.h" "$PROBE/bench.vert" >/dev/null
glslangValidator -V --vn bench_frag_spv -o "$PROBE/bench_frag_spv.h" "$PROBE/bench.frag" >/dev/null

for tool in a22probe a22test a22bench; do
    "$CC" --target="aarch64-linux-android$API" --sysroot="$SYSROOT" \
        -I "$PROBE" -O2 -fPIE -pie \
        -o "$OUT/$tool" "$PROBE/$tool.c" -ldl
    echo "built $OUT/$tool"
done
