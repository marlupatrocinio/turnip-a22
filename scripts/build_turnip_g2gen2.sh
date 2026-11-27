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
DIST="${DIST:-$HERE/dist}"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m!!!\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$WORK" "$DIST" "$HERE/src"

# ------------------------------------------------------------------- host
# Linux (CI, WSL) or Windows Git Bash. On Windows, meson/python are native
# Windows programs, so every path handed to them goes through winpath().
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        HOST=windows; HOST_TAG=windows-x86_64; EXE=.exe
        GLSLANG_ASSET=windows-x86_64
        winpath() { cygpath -m "$1"; }
        # shellcheck disable=SC1091
        . "$HERE/scripts/win_env.sh"
        ;;
    *)
        HOST=linux; HOST_TAG=linux-x86_64; EXE=
        GLSLANG_ASSET=linux-x86_64
        winpath() { printf '%s' "$1"; }
        ;;
esac
GLSLANG_VERSION="${GLSLANG_VERSION:-16.6.0}"

# ---------------------------------------------------------------- toolchain
if ! command -v meson >/dev/null; then
    [ "$HOST" = windows ] && die "meson not found - run scripts/setup_win_env.sh"
    log "installing meson/ninja/pyyaml/packaging"
    python3 -m pip install --user meson ninja pyyaml packaging mako \
        || die "pip refused (PEP 668?) - run scripts/setup_dev_env.sh and source ~/turnip-dev/env.sh"
    export PATH="$HOME/.local/bin:$PATH"
fi

if ! command -v glslangValidator >/dev/null; then
    if [ ! -x "$HERE/tools/glslang/bin/glslangValidator$EXE" ]; then
        log "downloading glslang $GLSLANG_VERSION"
        curl -L --fail -o "$WORK/glslang.zip" \
            "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VERSION/glslang-$GLSLANG_VERSION-$GLSLANG_ASSET-release.zip"
        unzip -q -o "$WORK/glslang.zip" -d "$HERE/tools/glslang"
        # recent releases ship only `glslang`; Mesa looks for glslangValidator
        [ -e "$HERE/tools/glslang/bin/glslangValidator$EXE" ] || \
            cp "$HERE/tools/glslang/bin/glslang$EXE" "$HERE/tools/glslang/bin/glslangValidator$EXE"
    fi
    export PATH="$HERE/tools/glslang/bin:$PATH"
fi

if [ "$HOST" = windows ] && ! command -v flex >/dev/null; then
    # Mesa (with patch 0002) wants POSIX flex/bison for an Android target;
    # WinFlexBison without --wincompat is exactly that.
    WFB_SRC="$(dirname "$(command -v win_flex || die "win_flex not found - run scripts/setup_win_env.sh")")"
    WFB="$HERE/tools/winflexbison"
    if [ ! -x "$WFB/flex.exe" ]; then
        log "staging WinFlexBison as flex/bison in tools/winflexbison"
        mkdir -p "$WFB"
        cp -r "$WFB_SRC"/. "$WFB"/
        cp "$WFB/win_flex.exe" "$WFB/flex.exe"
        cp "$WFB/win_bison.exe" "$WFB/bison.exe"
    fi
    export PATH="$WFB:$PATH"
fi

if [ ! -d "$NDK" ]; then
    log "downloading Android NDK $NDK_VERSION"
    curl -L --fail -o "$WORK/ndk.zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-$HOST.zip"
    unzip -q "$WORK/ndk.zip" -d "$HERE/src"
fi
NDK_W="$(winpath "$NDK")"
TOOLCHAIN="$NDK_W/toolchains/llvm/prebuilt/$HOST_TAG/bin"
SYSROOT="$NDK_W/toolchains/llvm/prebuilt/$HOST_TAG/sysroot"
[ -x "$TOOLCHAIN/clang$EXE" ] || die "NDK toolchain not found at $TOOLCHAIN"

# ------------------------------------------------------------------- source
MESA_SRC="${MESA_SRC:-$HERE/src/mesa-$MESA_VERSION}"
if [ ! -d "$MESA_SRC" ]; then
    log "fetching Mesa $MESA_VERSION"
    curl -L --fail -o "$WORK/mesa.tar.xz" "$MESA_URL"
    if [ -n "${MESA_SHA256:-}" ]; then
        echo "$MESA_SHA256  $WORK/mesa.tar.xz" | sha256sum -c -
    fi
    mkdir -p "$(dirname "$MESA_SRC")"
    tar -C "$(dirname "$MESA_SRC")" -xJf "$WORK/mesa.tar.xz"
    [ -d "$MESA_SRC" ] || mv "$(dirname "$MESA_SRC")/mesa-$MESA_VERSION" "$MESA_SRC"
fi
log "applying Turnip A22 patches"
# The Mesa tree may sit inside this repo (src/); never let git treat it as part
# of the turnip-a22 repository. A patch that neither applies nor is already
# applied is fatal - a silently unpatched build is not an A22 build.
mesa_git() { (cd "$MESA_SRC" && GIT_CEILING_DIRECTORIES="$(dirname "$MESA_SRC")" git "$@"); }
# EXTRA_PATCHES: an extra patch directory (e.g. patches/diag) for diagnostic
# builds; never set it for a release, and give such builds their own MESA_SRC.
EXTRA=()
[ -n "${EXTRA_PATCHES:-}" ] && EXTRA=("$EXTRA_PATCHES"/*.patch)
for p in "$HERE"/patches/*.patch "${EXTRA[@]}"; do
    pw="$(winpath "$p")"
    if mesa_git apply --check "$pw" 2>/dev/null; then
        mesa_git apply "$pw"
        echo "    applied $(basename "$p")"
    elif mesa_git apply --check --reverse "$pw" 2>/dev/null; then
        echo "    already applied $(basename "$p")"
    else
        mesa_git apply --check "$pw" || true
        die "patch $(basename "$p") does not apply to $MESA_SRC"
    fi
done

# ------------------------------------------------------------- cross file
# EXTRA_CFLAGS: space-separated extra compiler flags for experiments
# (e.g. "-mtune=cortex-x4"); never use -mcpu=cortex-x4 / armv9 (implies SVE,
# which these cores lack -> SIGILL).
EXTRA_ARGS=""
for f in ${EXTRA_CFLAGS:-}; do EXTRA_ARGS="$EXTRA_ARGS, '$f'"; done
CROSS="$WORK/cross-a22.txt"
cat > "$CROSS" <<EOF
[binaries]
c = ['$TOOLCHAIN/clang$EXE', '--target=aarch64-linux-android$API', '--sysroot=$SYSROOT']
cpp = ['$TOOLCHAIN/clang++$EXE', '--target=aarch64-linux-android$API', '--sysroot=$SYSROOT']
ar = '$TOOLCHAIN/llvm-ar$EXE'
strip = '$TOOLCHAIN/llvm-strip$EXE'
ranlib = '$TOOLCHAIN/llvm-ranlib$EXE'
nm = '$TOOLCHAIN/llvm-nm$EXE'
ld = '$TOOLCHAIN/ld.lld$EXE'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-DANDROID', '-D__ANDROID_API__=$API', '-fPIC', '-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16', '-fomit-frame-pointer', '-fno-semantic-interposition'$EXTRA_ARGS]
cpp_args = ['-DANDROID', '-D__ANDROID_API__=$API', '-fPIC', '-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16', '-fomit-frame-pointer', '-fno-semantic-interposition'$EXTRA_ARGS]
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++']
c_std = 'c11'
cpp_std = 'c++17'
EOF

# -------------------------------------------------------------------- build
BDIR="$WORK/mesa-a22"
log "configuring"
rm -rf "$BDIR"
meson setup "$(winpath "$BDIR")" "$(winpath "$MESA_SRC")" \
    --cross-file "$(winpath "$CROSS")" \
    --buildtype=release -Db_ndebug=true -Dstrip=false \
    -Dplatforms=android -Dplatform-sdk-version="$API" -Dandroid-stub=true \
    -Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled \
    -Dandroid-libbacktrace=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools= \
    -Dvalgrind=disabled -Dvulkan-beta=true -Dwerror=false

log "building"
ninja -C "$(winpath "$BDIR")" -j "$JOBS"
SO="$BDIR/src/freedreno/vulkan/libvulkan_freedreno.so"
[ -f "$SO" ] || die "build produced no $SO"

log "packaging"
if [ "$HOST" = windows ]; then PY=python; else PY=python3; fi
PKG_ARGS=()
[ -n "${PKG_NAME:-}" ] && PKG_ARGS+=(--name "$PKG_NAME")
[ -n "${PKG_PREFIX:-}" ] && PKG_ARGS+=(--zip-prefix "$PKG_PREFIX")
[ -n "${PKG_DESCRIPTION:-}" ] && PKG_ARGS+=(--description "$PKG_DESCRIPTION")
"$PY" "$(winpath "$HERE/scripts/package_driver.py")" --so "$(winpath "$SO")" \
    --version "$MESA_VERSION" --strip-tool "$TOOLCHAIN/llvm-strip$EXE" \
    --objcopy-tool "$TOOLCHAIN/llvm-objcopy$EXE" --out "$(winpath "$DIST")" \
    "${PKG_ARGS[@]}"

log "done - artifacts in $DIST"
ls -lh "$DIST"/*.zip "$DIST/SHA256SUMS.txt"
