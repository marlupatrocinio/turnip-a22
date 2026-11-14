#!/usr/bin/env bash
# Build Turnip (Mesa Vulkan) for the Snapdragon G2 Gen 2 / Adreno A22.
#
# Produces in dist/:
#   turnip-a22-<ver>.adpkg.zip    AdrenoTools package (meta.json + stripped HAL)
#   turnip-a22-<ver>.debug.zip    separate debug symbols
#   SHA256SUMS.txt                deterministic hashes
#
# Requirements: git, curl, xz, bison, flex, python3 (>=3.10), pip, zip,
#               glslangValidator >= 12.2, Android NDK r27c+ (r28c default).
#
# Usage:
#   ./scripts/build_turnip_g2gen2.sh
#   NDK=/opt/android-ndk-r28c ./scripts/build_turnip_g2gen2.sh
#   MESA_SRC=/src/mesa ./scripts/build_turnip_g2gen2.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Load pinned versions without overriding values already exported by the user.
if [ -f "$HERE/versions.env" ]; then
    while IFS='=' read -r key value; do
        case "$key" in ''|\#*) continue ;; esac
        [ -z "${!key:-}" ] && export "$key=$value"
    done < "$HERE/versions.env"
fi

MESA_VERSION="${MESA_VERSION:-26.3.0}"
MESA_URL="${MESA_URL:-https://archive.mesa3d.org/mesa-${MESA_VERSION}.tar.xz}"
NDK_VERSION="${NDK_VERSION:-r28c}"
NDK="${NDK:-$HERE/src/android-ndk-$NDK_VERSION}"
API="${API:-35}"
JOBS="${JOBS:-$(nproc)}"
WORK="${WORK:-$HERE/build}"
DIST="$HERE/dist"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m!!!\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$WORK" "$DIST" "$HERE/src"

# ---------------------------------------------------------------- toolchain
if ! command -v meson >/dev/null; then
    log "installing meson/ninja/pyyaml/packaging"
    python3 -m pip install --user meson ninja pyyaml packaging mako
    export PATH="$HOME/.local/bin:$PATH"
fi

if ! command -v glslangValidator >/dev/null; then
    if [ ! -x "$HERE/tools/glslang/bin/glslangValidator" ]; then
        log "downloading glslang 16.6.0"
        curl -L --fail -o "$WORK/glslang.zip" \
            https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.zip
        unzip -q "$WORK/glslang.zip" -d "$HERE/tools/glslang"
    fi
    export PATH="$HERE/tools/glslang/bin:$PATH"
fi

if [ ! -d "$NDK" ]; then
    log "downloading Android NDK $NDK_VERSION"
    curl -L --fail -o "$WORK/ndk.zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q "$WORK/ndk.zip" -d "$HERE/src"
fi
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -x "$TOOLCHAIN/clang" ] || die "NDK toolchain not found at $TOOLCHAIN"

# ------------------------------------------------------------------- source
MESA_SRC="${MESA_SRC:-$HERE/src/mesa-$MESA_VERSION}"
if [ ! -d "$MESA_SRC" ]; then
    log "fetching Mesa $MESA_VERSION"
    curl -L --fail -o "$WORK/mesa.tar.xz" "$MESA_URL"
    if [ -n "${MESA_SHA256:-}" ]; then
        echo "$MESA_SHA256  $WORK/mesa.tar.xz" | sha256sum -c -
    fi
    tar -C "$HERE/src" -xJf "$WORK/mesa.tar.xz"
fi
log "applying Turnip A22 patches"
for p in "$HERE"/patches/*.patch; do
    if git -C "$MESA_SRC" apply --check "$p" 2>/dev/null; then
        git -C "$MESA_SRC" apply "$p"
        echo "    applied $(basename "$p")"
    else
        echo "    skipped $(basename "$p") (already applied or not applicable)"
    fi
done

# ------------------------------------------------------------- cross file
CROSS="$WORK/cross-a22.txt"
cat > "$CROSS" <<EOF
[binaries]
c = ['$TOOLCHAIN/clang', '--target=aarch64-linux-android$API', '--sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot']
cpp = ['$TOOLCHAIN/clang++', '--target=aarch64-linux-android$API', '--sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot']
ar = '$TOOLCHAIN/llvm-ar'
strip = '$TOOLCHAIN/llvm-strip'
ranlib = '$TOOLCHAIN/llvm-ranlib'
ld = '$TOOLCHAIN/ld.lld'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-DANDROID', '-D__ANDROID_API__=$API', '-fPIC', '-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16', '-fomit-frame-pointer', '-fno-semantic-interposition']
cpp_args = ['-DANDROID', '-D__ANDROID_API__=$API', '-fPIC', '-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16', '-fomit-frame-pointer', '-fno-semantic-interposition']
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++']
c_std = 'c11'
cpp_std = 'c++17'
EOF

# -------------------------------------------------------------------- build
BDIR="$WORK/mesa-a22"
log "configuring"
rm -rf "$BDIR"
meson setup "$BDIR" "$MESA_SRC" \
    --cross-file "$CROSS" \
    --buildtype=release -Db_ndebug=true -Dstrip=false \
    -Dplatforms=android -Dplatform-sdk-version="$API" -Dandroid-stub=true \
    -Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled \
    -Dandroid-libbacktrace=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools= \
    -Dvalgrind=disabled -Dvulkan-beta=true -Dwerror=false

log "building"
ninja -C "$BDIR" -j "$JOBS"
SO="$BDIR/src/freedreno/vulkan/libvulkan_freedreno.so"
[ -f "$SO" ] || die "build produced no $SO"

log "packaging"
python3 "$HERE/scripts/package_driver.py" --so "$SO" \
    --version "$MESA_VERSION" --strip-tool "$TOOLCHAIN/llvm-strip" \
    --objcopy-tool "$TOOLCHAIN/llvm-objcopy" --out "$DIST"

log "done - artifacts in $DIST"
ls -lh "$DIST"/*.zip "$DIST/SHA256SUMS.txt"
