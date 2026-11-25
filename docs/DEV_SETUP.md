# Developer environment

Two supported hosts, same scripts:

* **Windows 11, Git Bash** (no WSL, no admin) - primary development host.
* **Linux** (Ubuntu 24.04, CI, or WSL2) - `scripts/setup_dev_env.sh`.

The device (Retroid Pocket G2) is reached with `adb` over USB in both cases.

## Windows (Git Bash)

### Setup (once)

```bash
bash scripts/setup_win_env.sh
```

Installs with winget (user scope): Python 3.12, CMake, Ninja, WinFlexBison,
platform-tools. Then creates `tools/py` (venv: meson, ninja, mako, pyyaml,
packaging, lxml), downloads NDK r28c (Windows) into `src/` and glslang 16.6.0
into `tools/glslang/`. All of these folders are gitignored.

In every new shell:

```bash
source scripts/win_env.sh
```

It finds the winget tools even when winget's `Links` shims are invisible to
Git Bash, activates `tools/py`, sets `NDK`, and sets `MSYS_NO_PATHCONV=1` so
`adb shell` / `adb push ... /data/local/tmp/` arguments are not rewritten into
Windows paths. Local paths given to native Windows programs (meson, ninja,
cmake, python, adb) must then be Windows-style (`cygpath -m`); the scripts do
this themselves.

### Build

```bash
bash scripts/build_turnip_g2gen2.sh   # driver  -> dist/
bash scripts/build_probes.sh          # probes + libvkhal_shim.so -> build/probe-tools/
bash scripts/build_deqp.sh            # dEQP-VK -> build/deqp/
```

Windows specifics handled by the scripts:

* NDK `windows-x86_64` prebuilts, `.exe` tool names, `llvm-nm` in the cross file
  (meson needs `nm`; Windows has none).
* WinFlexBison staged as `tools/winflexbison/{flex,bison}.exe`. With patch
  `0002`, Mesa uses plain POSIX flex/bison for the Android target, i.e. without
  `--wincompat` (which would emit `<io.h>`).
* dEQP builds in `src/cts` / `build/cts`: Windows `LongPathsEnabled` is off on
  this host and CTS object paths are deep, so the roots are kept short.
* dEQP must use CTS's own `targets/android/ndk-modern.cmake`
  (`-DDEQP_TARGET_TOOLCHAIN=ndk-modern -DANDROID_NDK_PATH=...`). With the
  NDK's bare `android.toolchain.cmake` CTS detects the *host*
  (`DE_OS_UNIX`, `DE_CPU_X86_64`) and fails on `__assert_fail`.

Measured on this PC (Ryzen 5 5500X3D, 12 threads): driver clean build
~2 min, byte-identical to the a22.3 release (`f308afd4…`); dEQP-VK 1.4.7.1
~11 min, 68 MB stripped `deqp-vk` + 677 MB data (pushed once).

## Linux / WSL2

```bash
bash scripts/setup_dev_env.sh
source ~/turnip-dev/env.sh
./scripts/build_turnip_g2gen2.sh
bash scripts/build_probes.sh
bash scripts/build_deqp.sh            # -> ~/turnip-dev/deqp
```

Heavy trees live in `~/turnip-dev` on the Linux filesystem (`/mnt/c` is slow).
Under WSL an `adb` wrapper forwards to Windows `adb.exe`, since USB stays with
Windows. WSL2 needs the Windows hypervisor: on this PC (SOYO B550M) it fails
with Hyper-V event 44 until SVM/NX are enabled in the BIOS.

## Test on device

```bash
adb devices                                            # authorise the G2 once
bash scripts/device_test.sh dist/turnip-a22-26.3.0.adpkg.zip
python scripts/run_deqp.py --driver dist/turnip-a22-26.3.0.adpkg.zip \
    --filter 'dEQP-VK.api.smoke.*' --out results/smoke
```

`run_deqp.py` pushes `deqp-vk` once to `/data/local/tmp/deqp` and loads the
driver with `--deqp-vk-library-path` (no app, no root). Turnip's Android build
exports only `HMI`, so it goes through `libvkhal_shim.so`
([`tools/probe/vkhal_shim.c`](../tools/probe/vkhal_shim.c), built by
`build_probes.sh`), which opens the HAL and exports `vkGetInstanceProcAddr`.
Verified: `dEQP-VK.api.smoke.*` 8/8 Pass; `dEQP-VK.info.device_properties`
reports `apiVersion` 1.4.363 / `driverVersion 0x06803000` (Turnip, not the
stock 1.3.0 blob). It survives crashing cases, takes debug knobs via `--env` (e.g.
`--env TU_DEBUG=gmem`) and diffs against a previous run
(`--baseline results/<run>/results.csv`).

## Profiling (simpleperf, no root)

The NDK ships simpleperf for the device and for the host. As the shell user it
profiles anything started from `/data/local/tmp` (probes, deqp-vk). The driver
is built with `-fomit-frame-pointer`, so use DWARF unwinding:

```bash
source scripts/win_env.sh
SP=src/android-ndk-r28c/simpleperf
adb push $SP/bin/android/arm64/simpleperf /data/local/tmp/
adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=. ./simpleperf record \
    --call-graph dwarf -f 4000 -o perf.data ./a22bench ./libvulkan_rel.so"
adb pull /data/local/tmp/perf.data build/perf/

# symbolise with the matching debug file, placed at the device path
mkdir -p build/perf/symfs/data/local/tmp
unzip -o dist/turnip-a22-26.3.0.debug.zip -d build/perf
cp build/perf/libvulkan_freedreno.so.debug build/perf/symfs/data/local/tmp/libvulkan_rel.so
$SP/bin/windows/x86_64/simpleperf.exe report -i build/perf/perf.data \
    --symfs build/perf/symfs --children --sort symbol
```

Kernel symbols stay unresolved (`kptr_restrict`, needs root); kernel time is
still attributed to the driver call that entered it (`ioctl` -> `kgsl_*`).

First profile (a22.3, `a22bench` 4000 draws): 76 % of CPU cycles in the
kernel, mostly buffer allocation (`kgsl_bo_init`/`kgsl_bo_finish`, ~37 %) and
submission (`kgsl_queue_submit`, ~14 %). `a22bench` is allocation-dominated
and therefore not a good CPU-overhead benchmark; a recording-only benchmark
is needed for driver CPU work.

Profiling games (Eden, GameNative) needs the app to be profileable or root;
probes and dEQP do not.
