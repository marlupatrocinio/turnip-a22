# Building Turnip A22

## Quick start (Linux or CI)

```bash
# prerequisites: git, curl, xz, bison, flex, python3 >= 3.10, pip, zip
./scripts/build_turnip_g2gen2.sh
```

The script reads pinned versions from ersions.env. It:

1. installs `meson`, `ninja`, `mako`, `pyyaml`, `packaging` if missing;
2. downloads glslang 16.6.0 (needs `>= 12.2` because Turnip builds BVH SPIR-V);
3. downloads Android NDK r28c (override with `NDK=/path/to/ndk`);
4. downloads Mesa 26.3.0 and applies `patches/*.patch`;
5. builds Variant A (generic `armv8-a`) and Variant B (`armv8.6-a+…`);
6. strips both and writes `dist/turnip-a22-26.3.0-variant{A,B}.adpkg.zip`.

Useful environment variables: `NDK`, `NDK_VERSION`, `MESA_VERSION`, `MESA_SRC`,
`MESA_URL`, `API`, `JOBS`, `VARIANT=A|B|all`.

## Meson configuration

Both variants use the same option set; only the ISA flags differ.

```bash
meson setup build/mesa-varA "$MESA_SRC" \
  --cross-file build/cross-A.txt \
  --buildtype=release -Db_ndebug=true -Dstrip=false \
  -Dplatforms=android -Dplatform-sdk-version=35 -Dandroid-stub=true \
  -Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled \
  -Dandroid-libbacktrace=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools= \
  -Dvalgrind=disabled -Dvulkan-beta=true -Dwerror=false
ninja -C build/mesa-varA -j"$(nproc)"
```

Key points:

* `-Dandroid-stub=true` avoids needing `libcutils`/`libhardware` pkg-config
  files at build time; the resulting HAL resolves those symbols on the device.
* `-Dplatforms=android` produces an Android Vulkan **HAL** (`HMI` symbol), which
  is what libadrenotools expects — not a desktop ICD.
* `-static-libstdc++` in `cpp_link_args` makes the `.so` self-contained
  (no `libc++_shared.so` dependency).
* `-Db_lto=true` is **not** usable: Mesa hard-errors with
  `Building Mesa with LTO is not supported.` (see `docs/DRIVER_DESIGN.md` §3.4).

### Cross file (Variant A)

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
c_args = ['-DANDROID', '-D__ANDROID_API__=35', '-fPIC']
cpp_args = ['-DANDROID', '-D__ANDROID_API__=35', '-fPIC']
c_link_args = ['-fuse-ld=lld']
cpp_link_args = ['-fuse-ld=lld', '-static-libstdc++']
c_std = 'c11'
cpp_std = 'c++17'
```

Variant B appends to `c_args`/`cpp_args`:

```
-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16
-fomit-frame-pointer
-fno-semantic-interposition
```

Do **not** use `-march=armv9-a`: LLVM implies SVE, these cores have none, and the
driver dies with SIGILL (documented in `docs/DRIVER_DESIGN.md` §5).

## Packaging

`scripts/package_driver.py` strips the library, writes an ADPKG-schema
`meta.json` and produces a flat zip:

```
meta.json
libvulkan_freedreno.so
```

`meta.json` fields: `schemaVersion: 1`, `name`, `description`, `author`,
`packageVersion`, `vendor: "Mesa"`, `driverVersion`, `minApi: 28`,
`libraryName: "libvulkan_freedreno.so"`.

## Windows-host notes

This project was originally built on a Windows 11 workstation without WSL or
Docker, which surfaced three environment-specific issues. None of them affect a
normal Linux/CI build, but they are documented for reproducibility:

1. **`win_flex --wincompat`** — patch `0002` fixes the generated lexer trying to
   include `<io.h>`. Without it, `ir3_lexer.c` fails to compile for Bionic.
2. **`ninja` cannot spawn children under the local DSH sandbox** — ninja hangs at
   0 % CPU forever (no version worked: 1.11, 1.13, official or PyPI). The build
   was therefore driven by [`tools/mininja.py`](../tools/win/mininja.py), a small
   ninja-file interpreter/executor that expands Meson's `build.ninja`,
   generates rspfiles and runs the commands from Python (which can spawn
   processes). On Linux, plain `ninja` is used.
3. **Python 3.14 tempfile DACLs** — under the sandbox, directories created by
   `tempfile.mkdtemp()` with mode `0o700` deny access to the creating process.
   A small `sitecustomize.py` shim (in `tools/py/`, build-environment only)
   re-implements `mkdtemp` without the restrictive mode.

If you build on Linux, ignore all three: `scripts/build_turnip_g2gen2.sh` uses
normal `ninja` and the standard toolchain.

## CI

`.github/workflows/build.yml` builds both variants on `ubuntu-24.04`, caches the
NDK, and uploads the two `.adpkg.zip` files plus `SHA256SUMS-*.txt` as artifacts.
It is the recommended path if you do not have a Linux machine: fork the repo,
enable Actions, run the workflow, download the artifact.
