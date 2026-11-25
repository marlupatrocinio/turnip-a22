#!/usr/bin/env bash
# Build dEQP-VK (Khronos Vulkan CTS) as a native Android arm64 executable.
#
# The binary runs from /data/local/tmp as the shell user and loads the Turnip
# library through tools/probe/vkhal_shim.c with --deqp-vk-library-path, so no
# app, loader layer or root is involved - the same model as a22test.
#
# Linux/WSL:   source ~/turnip-dev/env.sh && bash scripts/build_deqp.sh
# Windows:     bash scripts/build_deqp.sh          (Git Bash, see DEV_SETUP.md)
#   -> $DEQP_OUT/deqp-vk, $DEQP_OUT/vulkan/ (data), $DEQP_OUT/mustpass/
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
set -a; . "$HERE/versions.env"; set +a

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        HOST_TAG=windows-x86_64; EXE=.exe; PY=python
        winpath() { cygpath -m "$1"; }
        # shellcheck disable=SC1091
        . "$HERE/scripts/win_env.sh"
        # short roots: MAX_PATH (260) applies, CTS object paths are deep
        DEV_ROOT="${DEV_ROOT:-$HERE}"
        CTS_SRC="${CTS_SRC:-$DEV_ROOT/src/cts}"
        CTS_BUILD="${CTS_BUILD:-$DEV_ROOT/build/cts}"
        DEQP_OUT="${DEQP_OUT:-$DEV_ROOT/build/deqp}"
        ;;
    *)
        HOST_TAG=linux-x86_64; EXE=; PY=python3
        winpath() { printf '%s' "$1"; }
        DEV_ROOT="${DEV_ROOT:-$HOME/turnip-dev}"
        CTS_SRC="${CTS_SRC:-$DEV_ROOT/src/cts}"
        CTS_BUILD="${CTS_BUILD:-$DEV_ROOT/build/cts}"
        DEQP_OUT="${DEQP_OUT:-$DEV_ROOT/deqp}"
        ;;
esac

CTS_TAG="${CTS_TAG:-vulkan-cts-1.4.7.1}"
API="${API:-35}"
NDK="${NDK:-$HERE/src/android-ndk-${NDK_VERSION:-r28c}}"
[ -d "$NDK" ] || { echo "NDK not found at $NDK" >&2; exit 1; }
JOBS="${JOBS:-$(nproc)}"
MARCH="-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

if [ ! -d "$CTS_SRC/.git" ]; then
    log "cloning VK-GL-CTS $CTS_TAG"
    git -c core.longpaths=true clone --depth 1 --branch "$CTS_TAG" \
        https://github.com/KhronosGroup/VK-GL-CTS.git "$CTS_SRC"
fi
if [ ! -f "$CTS_SRC/.fetched" ]; then
    log "fetching CTS external sources (glslang, spirv-tools, ...)"
    (cd "$CTS_SRC" && "$PY" external/fetch_sources.py)
    touch "$CTS_SRC/.fetched"
fi

log "configuring"
# CTS's own toolchain (targets/android/ndk-modern.cmake) wraps the NDK's and
# sets DE_OS_ANDROID / DE_CPU_ARM_64; with the bare NDK toolchain file CTS
# falls back to host detection (DE_OS_UNIX, DE_CPU_X86_64) and fails to build.
cmake -S "$(winpath "$CTS_SRC")" -B "$(winpath "$CTS_BUILD")" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDEQP_TARGET=android -DDEQP_TARGET_TOOLCHAIN=ndk-modern \
    -DANDROID_NDK_PATH="$(winpath "$NDK")" -DANDROID_ABI=arm64-v8a \
    -DDE_ANDROID_API="$API" -DDEQP_ANDROID_EXE=ON \
    -DCMAKE_C_FLAGS="$MARCH" -DCMAKE_CXX_FLAGS="$MARCH"

log "building deqp-vk"
cmake --build "$(winpath "$CTS_BUILD")" -j "$JOBS" --target deqp-vk

log "staging to $DEQP_OUT"
rm -rf "$DEQP_OUT"
mkdir -p "$DEQP_OUT/mustpass"
VKMOD="$CTS_BUILD/external/vulkancts/modules/vulkan"
"$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/llvm-strip$EXE" \
    -o "$(winpath "$DEQP_OUT/deqp-vk")" "$(winpath "$VKMOD/deqp-vk")"
cp -r "$VKMOD/vulkan" "$DEQP_OUT/vulkan"
cp -r "$CTS_SRC/external/vulkancts/mustpass/main/." "$DEQP_OUT/mustpass/" 2>/dev/null || true
ls -lh "$DEQP_OUT/deqp-vk"
du -sh "$DEQP_OUT"
