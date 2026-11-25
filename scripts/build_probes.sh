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
# Recent glslang releases ship only `glslang`; older ones `glslangValidator`.
[ -d "$HERE/tools/glslang/bin" ] && PATH="$HERE/tools/glslang/bin:$PATH"
GLSLANG="$(command -v glslangValidator || command -v glslang || true)"
if [ -z "$GLSLANG" ]; then
    echo "glslang/glslangValidator not found; required to build the test shaders" >&2
    exit 1
fi
"$GLSLANG" -V --vn a22_comp_spv -o "$PROBE/a22_comp_spv.h" "$PROBE/a22_compute.comp" >/dev/null
"$GLSLANG" -V --vn bench_vert_spv -o "$PROBE/bench_vert_spv.h" "$PROBE/bench.vert" >/dev/null
"$GLSLANG" -V --vn bench_frag_spv -o "$PROBE/bench_frag_spv.h" "$PROBE/bench.frag" >/dev/null
"$GLSLANG" -V --vn gmem_vert_spv -o "$PROBE/gmem_vert_spv.h" "$PROBE/gmem.vert" >/dev/null
"$GLSLANG" -V --vn gmem_geom_frag_spv -o "$PROBE/gmem_geom_frag_spv.h" "$PROBE/gmem_geom.frag" >/dev/null
"$GLSLANG" -V --vn gmem_light_frag_spv -o "$PROBE/gmem_light_frag_spv.h" "$PROBE/gmem_light.frag" >/dev/null
"$GLSLANG" -V --vn gmem_light_tex_frag_spv -o "$PROBE/gmem_light_tex_frag_spv.h" "$PROBE/gmem_light_tex.frag" >/dev/null

for tool in a22probe a22test a22bench a22gmem; do
    "$CC" --target="aarch64-linux-android$API" --sysroot="$SYSROOT" \
        -I "$PROBE" -O2 -fPIE -pie \
        -o "$OUT/$tool" "$PROBE/$tool.c" -ldl
    echo "built $OUT/$tool"
done

# HMI -> vkGetInstanceProcAddr adapter for dEQP-VK and other by-path loaders.
"$CC" --target="aarch64-linux-android$API" --sysroot="$SYSROOT" \
    -I "$PROBE" -O2 -fPIC -shared -fvisibility=hidden \
    -o "$OUT/libvkhal_shim.so" "$PROBE/vkhal_shim.c" -ldl
echo "built $OUT/libvkhal_shim.so"
