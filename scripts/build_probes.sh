#!/usr/bin/env bash
# Build the on-device probe/test binaries used by the release gate.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NDK_VERSION="${NDK_VERSION:-r28c}"
API="${API:-35}"
NDK="${NDK:-$HERE/src/android-ndk-$NDK_VERSION}"
OUT="${OUT:-$HERE/build/probe-tools}"

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
for tool in a22probe a22test; do
    "$CC" --target="aarch64-linux-android$API" --sysroot="$SYSROOT" \
        -I "$HERE/tools/probe" -O2 -fPIE -pie \
        -o "$OUT/$tool" "$HERE/tools/probe/$tool.c" -ldl
    echo "built $OUT/$tool"
done
