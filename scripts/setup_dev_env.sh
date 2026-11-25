#!/usr/bin/env bash
# One-time developer environment setup for Turnip A22 (Ubuntu 24.04 / WSL2).
#
# Installs everything needed to build the driver, the probe tools and dEQP-VK,
# and to talk to the device:
#   * apt: compiler toolchain, bison/flex, cmake/ninja, python venv, zip, ...
#   * Python venv with meson, ninja, mako, pyyaml, packaging
#   * Android NDK (versions.env, default r28c) - includes simpleperf
#   * glslang (versions.env, default 16.6.0)
#   * an `adb` wrapper that forwards to Windows adb.exe under WSL (USB devices
#     are attached to Windows, not to the WSL VM), or Linux platform-tools on
#     bare-metal Linux
#
# Everything heavy lives in $DEV_ROOT (default ~/turnip-dev) on the Linux
# filesystem; building on /mnt/c is several times slower.
#
#   bash scripts/setup_dev_env.sh
#   source ~/turnip-dev/env.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEV_ROOT="${DEV_ROOT:-$HOME/turnip-dev}"

# shellcheck disable=SC1091
set -a; . "$HERE/versions.env"; set +a
NDK_VERSION="${NDK_VERSION:-r28c}"
GLSLANG_VERSION="${GLSLANG_VERSION:-16.6.0}"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

mkdir -p "$DEV_ROOT" "$HOME/.local/bin"

# ------------------------------------------------------------------- apt
log "installing system packages"
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential bison flex pkg-config git curl ca-certificates xz-utils \
    zip unzip python3 python3-venv python3-pip cmake ninja-build \
    libxml2-utils file binutils

# ------------------------------------------------------------- python
VENV="$DEV_ROOT/venv"
if [ ! -x "$VENV/bin/meson" ]; then
    log "creating python venv with meson/mako"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --upgrade pip
    "$VENV/bin/pip" install meson ninja mako pyyaml packaging lxml
fi

# ---------------------------------------------------------------- NDK
NDK="$DEV_ROOT/android-ndk-$NDK_VERSION"
if [ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
    log "downloading Android NDK $NDK_VERSION"
    curl -L --fail -o "$DEV_ROOT/ndk.zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
    unzip -q "$DEV_ROOT/ndk.zip" -d "$DEV_ROOT"
    rm -f "$DEV_ROOT/ndk.zip"
fi

# ------------------------------------------------------------- glslang
GLSLANG="$DEV_ROOT/glslang-$GLSLANG_VERSION"
if [ ! -x "$GLSLANG/bin/glslangValidator" ]; then
    log "downloading glslang $GLSLANG_VERSION"
    curl -L --fail -o "$DEV_ROOT/glslang.zip" \
        "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VERSION/glslang-$GLSLANG_VERSION-linux-x86_64-release.zip"
    mkdir -p "$GLSLANG"
    unzip -q -o "$DEV_ROOT/glslang.zip" -d "$GLSLANG"
    rm -f "$DEV_ROOT/glslang.zip"
fi

# ----------------------------------------------------------------- adb
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    log "WSL detected: installing adb wrapper for Windows adb.exe"
    cat > "$HOME/.local/bin/adb" <<'EOF'
#!/usr/bin/env bash
# Forward to Windows adb.exe; translate local Linux paths to Windows paths so
# `adb push/pull/install` work from the WSL filesystem.
ADB_EXE="${ADB_EXE:-$(command -v adb.exe)}"
[ -n "$ADB_EXE" ] || { echo "adb.exe not on PATH (winget install Google.PlatformTools)" >&2; exit 1; }
args=()
sub="" ; n=$#
for ((i=1; i<=n; i++)); do
    a="${!i}"
    [ -z "$sub" ] && case "$a" in push|pull|install|install-multiple|sync) sub="$a" ;; esac
    if [ -n "$sub" ] && [ "$a" != "$sub" ]; then
        if [ -e "$a" ]; then
            a="$(wslpath -w "$a")"
        elif [ "$sub" = pull ] && [ $i -eq $n ] && [ -d "$(dirname "$a")" ]; then
            a="$(wslpath -w "$(dirname "$a")")\\$(basename "$a")"
        fi
    fi
    args+=("$a")
done
exec "$ADB_EXE" "${args[@]}"
EOF
    chmod +x "$HOME/.local/bin/adb"
else
    PT="$DEV_ROOT/platform-tools"
    if [ ! -x "$PT/adb" ]; then
        log "downloading Linux platform-tools"
        curl -L --fail -o "$DEV_ROOT/pt.zip" \
            https://dl.google.com/android/repository/platform-tools-latest-linux.zip
        unzip -q "$DEV_ROOT/pt.zip" -d "$DEV_ROOT"
        rm -f "$DEV_ROOT/pt.zip"
    fi
    ln -sf "$PT/adb" "$HOME/.local/bin/adb"
fi

# ------------------------------------------------------------- env.sh
cat > "$DEV_ROOT/env.sh" <<EOF
# Source this before building: source $DEV_ROOT/env.sh
export DEV_ROOT="$DEV_ROOT"
export NDK="$NDK"
export WORK="$DEV_ROOT/build"
export MESA_SRC="$DEV_ROOT/src/mesa-\${MESA_VERSION:-$MESA_VERSION}"
export PATH="$VENV/bin:$GLSLANG/bin:\$HOME/.local/bin:\$PATH"
export SIMPLEPERF="$NDK/simpleperf"
export JOBS="\${JOBS:-\$(nproc)}"
EOF

log "done"
# shellcheck disable=SC1091
. "$DEV_ROOT/env.sh"
echo "  meson     $(meson --version)"
echo "  ninja     $(ninja --version)"
echo "  cmake     $(cmake --version | head -1)"
echo "  glslang   $(glslangValidator --version | head -1)"
echo "  clang     $("$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version | head -1)"
echo "  adb       $(adb version 2>/dev/null | head -1 || echo 'not reachable')"
echo
echo "Next: source $DEV_ROOT/env.sh && ./scripts/build_turnip_g2gen2.sh"
