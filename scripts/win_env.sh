# Windows (Git Bash) build environment for Turnip A22.
#
#   source scripts/win_env.sh
#
# Finds the winget-installed tools (Python 3.12, CMake, Ninja, WinFlexBison,
# platform-tools) even when winget's Links shims are not visible to Git Bash,
# activates the project venv (tools/py: meson, ninja, mako, ...) and points
# NDK at src/android-ndk-<ver>. Create everything once with:
#
#   bash scripts/setup_win_env.sh

_here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
_pkgs="$(cygpath "$LOCALAPPDATA")/Microsoft/WinGet/Packages"

_add() { [ -d "$1" ] && case ":$PATH:" in *":$1:"*) ;; *) PATH="$1:$PATH" ;; esac; }
for _d in "$_pkgs"/Kitware.CMake_*/cmake-*/bin \
          "$_pkgs"/Ninja-build.Ninja_* \
          "$_pkgs"/WinFlexBison.win_flex_bison_* \
          "$_pkgs"/Google.PlatformTools_*/platform-tools \
          "$(cygpath "$LOCALAPPDATA")/Programs/Python/Python312" \
          "$_here/tools/glslang/bin"; do
    _add "$_d"
done
[ -d "$_here/tools/py/Scripts" ] && PATH="$_here/tools/py/Scripts:$PATH"
export PATH

# Read only NDK_VERSION from versions.env: sourcing the whole file here would
# clobber overrides such as MESA_VERSION passed to the build script.
_ndkv="$(sed -n 's/^NDK_VERSION=//p' "$_here/versions.env")"
export NDK="${NDK:-$_here/src/android-ndk-${NDK_VERSION:-${_ndkv:-r28c}}}"
unset _ndkv
export MSYS_NO_PATHCONV=1       # keep /data/... adb arguments intact
export PYTHONUTF8=1
unset _here _pkgs _d
