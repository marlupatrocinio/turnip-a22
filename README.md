<div align="center">

# Turnip A22

**Mesa Turnip (Vulkan) for devices with the Qualcomm Snapdragon G2 Gen 2 / Adreno (TM) 22 GPU**
**(KGSL `chip_id 0x43030b00`, `gen7-11-0`).**

[![Mesa](https://img.shields.io/badge/Mesa-26.3.1-2b6cb0)](https://mesa3d.org)
[![Android](https://img.shields.io/badge/built%20for-API%2035-3ddc84)](https://developer.android.com)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.4.363-e53e3e)](#validation)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---

A build of Mesa's open-source Turnip Vulkan driver, packaged as an Android
Vulkan HAL for [AdrenoTools](https://github.com/bylaws/libadrenotools)-based
front-ends (emulators and compatibility layers that load a custom GPU driver
per app). It does not replace the system driver and needs no root.

## Supported hardware

The driver targets a GPU, not a specific device. It applies to any Android
device whose GPU reports:

| Field | Value |
|---|---|
| SoC | Qualcomm Snapdragon G2 Gen 2 (`SG6275P`) |
| GPU | Adreno (TM) 22, KGSL `chip_id 0x43030b00`, DT `qcom,adreno-gpu-gen7-11-0` |
| GMEM / UBWC | 1.5 MiB, UBWC 4.0, `highest_bank_bit` 16 |
| CPU ISA | ARMv8.6-A class (dotprod, fp16, i8mm, bf16), no SVE |

Mesa matches this GPU to its Adreno 735 profile; patch 0001 adds an
exact-match entry so it identifies as `Adreno (TM) 22` with the same
hardware profile.

**Tested device:** Retroid Pocket G2 (Android 15, SDK 35, kernel 6.1.115). Other
devices with the same SoC should use the same code path, but they are untested:
differences in kernel (KGSL) version, GPU firmware or Android version can
change behaviour. To check a device: `adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_model`
should print `Adreno22`; `a22probe` (built by
[`scripts/build_probes.sh`](scripts/build_probes.sh)) prints the KGSL `chip_id`, and
[`scripts/device_test.sh`](scripts/device_test.sh) runs a functional check of a package.

**Android version:** the library is built against API 35 (Android 15) and has
only been tested on Android 15. Older Android versions are untested and may not
load it.

## Current release: 26.3.1 (a22.4)

Mesa 26.3.1 plus four local patches:

| Patch | Purpose |
|---|---|
| `0001` | Exact device entry for `chip_id 0x43030b00` (name only; Adreno 735 profile unchanged) |
| `0002` | Build fix: POSIX flex/bison when cross-compiling for Android from Windows |
| `0003` | Build fix: path separators in freedreno's XML-include check on Windows |
| `0004` | On A7XX, write render-backend static registers on the BR thread only. Removes the `CP: AHB bus error` interrupts (about 255/s in a measured game) caused by BV-thread writes the hardware rejects. |

No performance tuning is applied; defaults are upstream Mesa's. Release notes:
[`docs/RELEASE_NOTES_a22.4.md`](docs/RELEASE_NOTES_a22.4.md).

### Artifacts

| File | Contents |
|---|---|
| `turnip-a22-26.3.1.adpkg.zip` | `meta.json` + stripped `libvulkan_freedreno.so` |
| `turnip-a22-26.3.1.debug.zip` | `libvulkan_freedreno.so.debug` (build-id matched) |
| `SHA256SUMS.txt` | hashes of both (archives are deterministic) |

## Install

Copy the `.adpkg.zip` to the device and import it, unextracted, in the
front-end's GPU driver manager, then select it:

* **Eden** (tested): *Settings → GPU → Install driver*
* **GameNative** (tested): container driver settings

Other AdrenoTools front-ends (e.g. Winlator) use the same package format but
have not been tested. Restart the front-end fully after switching drivers. The
driver reports device name `Adreno (TM) 22` and driver version 26.3.1. Removing
it is done in the same driver manager; nothing outside the app's private
storage is modified.

## Validation

Results for the a22.4 package, measured on the Retroid Pocket G2:

| Check | Result |
|---|---|
| Functional gate ([`scripts/device_test.sh`](scripts/device_test.sh)) | pass - compute 64/64, image clear + resolve 262144/262144 pixels |
| `dEQP-VK.renderpasses.*` (Vulkan CTS 1.4.7.1, 81,687 cases) | 53,450 Pass, 28,215 NotSupported, 22 Fail |
| AHB error interrupts under load | 0 (a22.3: about 2,000 per benchmark run) |
| Play check | BOTW (Eden) and a GameNative title render and run normally |

Only the render-pass group of the Vulkan CTS has been run so far (the full
suite has 3.2 M cases). The 22 failures are `fragment_density_map ...
static_subsampled_4_4` cases and are the same in every build tested.

In-game measurements (45-150 s captures with
[`scripts/frametimes.py`](scripts/frametimes.py); details in
[`docs/FIELD_TEST_NMS.md`](docs/FIELD_TEST_NMS.md)):

| Title (front-end) | Observation |
|---|---|
| No Man's Sky (GameNative), 720p low | GPU-bound (95-98 % busy). 19-22 FPS after warm-up; repeated runs of the same driver varied by up to ±20 %, so single runs do not separate drivers. An earlier single-run comparison against the stock Qualcomm driver (31.8 vs 29.1 FPS, under different conditions) is in the same document. |
| Zelda: BOTW (Eden) | CPU-bound in Eden (~62-69 % GPU busy), 28-29 FPS at the game's 30 FPS cap, 7-8 % slow frames; driver changes tested so far stayed within run-to-run noise. |
| Sleeping Dogs: Definitive Edition benchmark (GameNative) | ~52 FPS, GPU clock limited by temperature (~910 MHz at 85-87 °C). |

## Build

Linux (CI, WSL) or Windows (Git Bash):

```bash
./scripts/build_turnip_g2gen2.sh
# -> dist/turnip-a22-26.3.1.adpkg.zip, dist/turnip-a22-26.3.1.debug.zip, dist/SHA256SUMS.txt
```

Inputs are pinned in [`versions.env`](versions.env) (the Mesa tarball's SHA-256
is verified); patches are in [`patches/`](patches). Host setup:
[`docs/DEV_SETUP.md`](docs/DEV_SETUP.md); toolchain and cross file:
[`docs/BUILD.md`](docs/BUILD.md).

Release gate before publishing (device connected over adb):

```bash
bash scripts/build_probes.sh
bash scripts/device_test.sh dist/turnip-a22-26.3.1.adpkg.zip
python scripts/run_deqp.py --driver dist/turnip-a22-26.3.1.adpkg.zip \
    --filter 'dEQP-VK.renderpasses.*' --out results/renderpasses
```

## Repository layout

```
.github/      CI: build, hardware gate, manual release
docs/         hardware data, design, build, validation, benchmarks, bug reports
patches/      Mesa patches (0001-0004 applied; optional/ and diag/ are not released)
scripts/      build, packaging, device gate, dEQP runner, frame capture
tools/probe/  KGSL/Vulkan probe, functional test, benchmarks, GMEM reproducer
versions.env  pinned Mesa / NDK / glslang versions
```

## License

Project code: MIT ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)). Driver binaries are
Mesa builds (MIT). Qualcomm, Snapdragon and Adreno are trademarks of Qualcomm
Incorporated; this project is not affiliated with or endorsed by Qualcomm, Mesa
or any device manufacturer.
