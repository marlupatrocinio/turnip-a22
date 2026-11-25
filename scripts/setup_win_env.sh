#!/usr/bin/env bash
# One-time Windows (Git Bash) developer setup for Turnip A22. No WSL, no admin.
#
#   bash scripts/setup_win_env.sh
#   source scripts/win_env.sh        # in every new shell
#
# Installs (user scope, winget): Python 3.12, CMake, Ninja, WinFlexBison,
# Android platform-tools. Creates tools/py (meson, ninja, mako, pyyaml,
# packaging, lxml), downloads NDK into src/ and glslang into tools/glslang/.
# Everything lands in gitignored folders of this repo.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
. "$HERE/versions.env"
NDK_VERSION="${NDK_VERSION:-r28c}"
GLSLANG_VERSION="${GLSLANG_VERSION:-16.6.0}"

log() { printf '\033[1;32m==>\033[0m %s\n' "$*"; }

for id in Python.Python.3.12 Kitware.CMake Ninja-build.Ninja \
          WinFlexBison.win_flex_bison Google.PlatformTools; do
    if ! winget list --id "$id" -e >/dev/null 2>&1; then
        log "winget install $id"
        winget install --id "$id" -e --scope user --accept-source-agreements \
            --accept-package-agreements --disable-interactivity
    fi
done

# shellcheck disable=SC1091
. "$HERE/scripts/win_env.sh"

if [ ! -x "$HERE/tools/py/Scripts/meson.exe" ]; then
    log "creating tools/py venv"
    python -m venv "$(cygpath -m "$HERE/tools/py")"
    "$HERE/tools/py/Scripts/python" -m pip install -q --upgrade pip
    "$HERE/tools/py/Scripts/pip" install -q meson ninja mako pyyaml packaging lxml
fi

if [ ! -d "$HERE/src/android-ndk-$NDK_VERSION" ]; then
    log "downloading NDK $NDK_VERSION (Windows)"
    mkdir -p "$HERE/src"
    curl -L --fail -o "$HERE/src/ndk.zip" \
        "https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-windows.zip"
    unzip -q "$HERE/src/ndk.zip" -d "$HERE/src" && rm "$HERE/src/ndk.zip"
fi

G="$HERE/tools/glslang/bin"
if [ ! -x "$G/glslangValidator.exe" ]; then
    log "downloading glslang $GLSLANG_VERSION (Windows)"
    curl -L --fail -o "$HERE/src/glslang.zip" \
        "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VERSION/glslang-$GLSLANG_VERSION-windows-x86_64-release.zip"
    unzip -q -o "$HERE/src/glslang.zip" -d "$HERE/tools/glslang" && rm "$HERE/src/glslang.zip"
    [ -e "$G/glslangValidator.exe" ] || cp "$G/glslang.exe" "$G/glslangValidator.exe"
fi

# shellcheck disable=SC1091
. "$HERE/scripts/win_env.sh"
log "toolchain"
echo "  python   $(python --version)"
echo "  meson    $(meson --version)"
echo "  ninja    $(ninja --version)"
echo "  cmake    $(cmake --version | head -1)"
echo "  flex     $(win_flex --version)"
echo "  glslang  $(glslangValidator --version | head -1)"
echo "  adb      $(adb version | head -1)"
echo "  ndk      $NDK"
