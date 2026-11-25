# Building Turnip A22

## Quick start

```bash
./scripts/build_turnip_g2gen2.sh
# -> dist/turnip-a22-26.3.0.adpkg.zip
#    dist/turnip-a22-26.3.0.debug.zip
#    dist/SHA256SUMS.txt
```

Prerequisites: `git curl xz bison flex python3>=3.10 pip zip unzip`, plus
`glslangValidator >= 12.2` (Turnip compiles BVH SPIR-V at build time).

The script reads pinned versions from `versions.env` and:

1. installs `meson`, `ninja`, `mako`, `pyyaml`, `packaging` if missing;
2. downloads glslang 16.6.0 and Android NDK r28c if absent;
3. downloads Mesa 26.3.0 and applies `patches/*.patch`;
4. cross-compiles Turnip with the ARMv8.6-A flags below;
5. strips the library, emits debug symbols and writes deterministic archives.

Environment overrides: `NDK`, `NDK_VERSION`, `MESA_VERSION`, `MESA_SRC`,
`MESA_URL`, `API`, `JOBS`, `WORK`.

## Meson configuration

```bash
meson setup build/mesa-a22 "$MESA_SRC" \
  --cross-file build/cross-a22.txt \
  --buildtype=release -Db_ndebug=true -Dstrip=false \
  -Dplatforms=android -Dplatform-sdk-version=35 -Dandroid-stub=true \
  -Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled \
  -Dandroid-libbacktrace=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools= \
  -Dvalgrind=disabled -Dvulkan-beta=true -Dwerror=false
ninja -C build/mesa-a22 -j"$(nproc)"
```

Notes:

* `-Dandroid-stub=true` avoids `libcutils`/`libhardware` pkg-config at build
  time; the HAL resolves those symbols on device.
* `-Dplatforms=android` produces an Android Vulkan **HAL** (`HMI` export), not a
  desktop ICD.
* `-static-libstdc++` makes the `.so` self-contained (no `libc++_shared.so`).
* `-Db_lto=true` is rejected by Mesa (`Building Mesa with LTO is not supported.`).

## Cross file

```ini
[binaries]
c = ['$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang',
     '--target=aarch64-linux-android35',
     '--sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot']
cpp = ['.../clang++', '--target=aarch64-linux-android35', '--sysroot=...']
ar = '.../llvm-ar'
strip = '.../llvm-strip'
ranlib = '.../llvm-ranlib'
ld = '.../ld.lld'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-DANDROID', '-D__ANDROID_API__=35', '-fPIC',
          '-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16',
          '-fomit-frame-pointer', '-fno-semantic-interposition']
cpp_args = [ same as c_args ]
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++']
c_std = 'c11'
cpp_std = 'c++17'
```

`-march=armv9-a` is invalid here: LLVM implies SVE, these Kryo cores implement
none, and the driver dies with `SIGILL` during device enumeration.

## Packaging

`scripts/package_driver.py` writes:

```
turnip-a22-<ver>.adpkg.zip   meta.json + stripped libvulkan_freedreno.so
turnip-a22-<ver>.debug.zip   libvulkan_freedreno.so.debug
SHA256SUMS.txt               hashes of both
```

`meta.json`: `schemaVersion: 1`, `name`, `description`, `author`,
`packageVersion`, `vendor: "Mesa"`, `driverVersion`, `minApi: 28`,
`libraryName: "libvulkan_freedreno.so"`.

Guarantees:

* deterministic archives (fixed timestamps, sorted entries, fixed permissions);
* the stripped `.so` is byte-identical to the hardware-validated library;
* debug symbols are a separate asset (AdrenoTools packages contain only
  `meta.json` + the driver).

Release process and hardware gate: [RELEASING.md](RELEASING.md).

## Windows host (Git Bash, no WSL)

`build_turnip_g2gen2.sh` detects Git Bash and switches to the NDK's
`windows-x86_64` prebuilts. One-time setup and per-shell environment:
[DEV_SETUP.md](DEV_SETUP.md) (`scripts/setup_win_env.sh`,
`source scripts/win_env.sh`).

Host-specific issues and how they are handled:

1. `win_flex --wincompat` emits `<io.h>`; patch `0002` makes Mesa look for
   POSIX `flex`/`bison` when the target is not Windows, and the build script
   stages WinFlexBison under those names (`tools/winflexbison/`).
2. meson needs `nm`; the cross file names the NDK's `llvm-nm`.
3. Mesa's freedreno XML-include lint compares `/`-separated paths, but Python's
   `glob` returns `\` on Windows, so every file was flagged; patch `0003`
   normalises the separators (no-op on Linux).
4. Patches are applied with git's repository discovery fenced off
   (`GIT_CEILING_DIRECTORIES`) and Windows-style paths; a patch that neither
   applies nor is already applied now aborts the build instead of being
   reported as "skipped".
5. Earlier builds (inside an agent sandbox) needed
   [`tools/win/mininja.py`](../tools/win/mininja.py) because ninja could not
   spawn children, and a `sitecustomize.py` shim for Python 3.14 temp-dir
   DACLs. Neither is needed in a normal Git Bash session with Python 3.12.

## CI

`.github/workflows/build.yml` builds on `ubuntu-24.04`, caches the NDK and
uploads `dist/*.zip` + `SHA256SUMS.txt`.
