<div align="center">

# Turnip A22

**Mesa Turnip (Vulkan) for the Snapdragon G2 Gen 2 handheld — Adreno (TM) 22, `chip_id 0x43030b00`, `gen7-11-0`.**

[![Mesa](https://img.shields.io/badge/Mesa-26.3.0-2b6cb0)](https://mesa3d.org)
[![Android](https://img.shields.io/badge/Android-15%20%2F%20API%2035-3ddc84)](https://developer.android.com)
[![Vulkan](https://img.shields.io/badge/Vulkan-1.4.363-e53e3e)](#verification)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

---

Mesa 26.3.0 freedreno build packaged as an Android Vulkan HAL for
[AdrenoTools](https://github.com/bylaws/libadrenotools) front-ends. No root.

## Target hardware

| Field | Value |
|---|---|
| SoC | Qualcomm Snapdragon G2 Gen 2 (`SG6275P`) |
| GPU | Adreno (TM) 22, `chip_id 0x43030b00`, DT `qcom,adreno-gpu-gen7-11-0` |
| GMEM / UBWC | 1.5 MiB (`0x180000`), UBWC 4.0, `highest_bank_bit` 16 |
| OS | Android 9+ (API 28+), arm64 |

Device identity is exact-match (`Adreno (TM) 22`), not the Adreno 735 wildcard.

## Artifacts

| File | Contents |
|---|---|
| `turnip-a22-26.3.0.adpkg.zip` | `meta.json` + stripped `libvulkan_freedreno.so` |
| `turnip-a22-26.3.0.debug.zip` | `libvulkan_freedreno.so.debug` (build-id matched) |
| `SHA256SUMS.txt` | deterministic hashes for both |

Build flags: `-O3`, `-DNDEBUG`, `-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16`,
`-fomit-frame-pointer`, `-fno-semantic-interposition`, `-static-libstdc++`,
`-Wl,--gc-sections`, `llvm-strip --strip-unneeded`. Dependencies: system libraries
only (`libc`, `libm`, `libdl`, `libz`, `liblog`, `libhardware`,
`libnativewindow`, `libsync`).

## Install

```bash
adb push turnip-a22-26.3.0.adpkg.zip /sdcard/Download/
```

Import the zip in the front-end's driver manager (Winlator: *Adrenotools GPU
Drivers*; Eden: *Settings → GPU → Install driver*) and select **Turnip A22**.

Expected: `Turnip Adreno (TM) 22`, Vulkan `1.4.363`. Uninstalling is done from the
same manager; nothing outside the app's private storage is modified.

## Verification

Run on hardware (Retroid Pocket G2, Android 15 / SDK 35, kernel 6.1.115) with the
shipped stripped library:

```
HAL load (HMI), vkCreateInstance   PASS
deviceName / apiVersion            Turnip Adreno (TM) 22 / 1.4.363
compute dispatch                   64/64 invocations correct
image clear + tiled resolve        262144/262144 pixels correct
compute submit round-trip          0.24 ms
```

Field tests, same scene and settings, 45–180 s captures:

| Title / stack | Metric | Stock Qualcomm | This build |
|---|---|---|---|
| No Man's Sky, GameNative, 720p low | avg / p95 / frames >33 ms | 29.1 fps / 50 ms / 9.5 % | **31.8 fps / 33.33 ms / 1.5 %** |
| Zelda: BOTW, Eden | avg / frames >33 ms | — | 29.1 fps / 6.2–7.2 % |

Method and raw data: [`docs/VALIDATION.md`](docs/VALIDATION.md),
[`docs/FIELD_TEST_NMS.md`](docs/FIELD_TEST_NMS.md).

## Build

```bash
./scripts/build_turnip_g2gen2.sh
# -> dist/turnip-a22-26.3.0.adpkg.zip
#    dist/turnip-a22-26.3.0.debug.zip
#    dist/SHA256SUMS.txt
```

Pinned inputs in [`versions.env`](versions.env); two local Mesa patches in
[`patches/`](patches). Toolchain, cross file and host notes:
[`docs/BUILD.md`](docs/BUILD.md).

Release gate before publishing:

```bash
bash scripts/build_probes.sh
bash scripts/device_test.sh dist/turnip-a22-26.3.0.adpkg.zip
```

## Runtime knobs

| Variable | Values |
|---|---|
| `TU_AUTOTUNE_ALGO` | `bandwidth` (default), `profiled`, `profiled_imm`, `prefer_sysmem` |
| `TU_AUTOTUNE_FLAGS` | `big_gmem`, `tune_small`, `preempt_optimize` |
| `TU_DEBUG` | `startup`, `sysmem`, `nobin`, `noubwc`, `nolrz`, `flushall`, `perf` |
| `TU_GMEM` | override detected GMEM (diagnostics) |
| `FD_DEV_FEATURES` | `has_*=0/1` |

On this GPU, forcing tiled rendering (`TU_DEBUG=gmem`,
`TU_AUTOTUNE_ALGO=prefer_gmem`) corrupts output; use `TU_DEBUG=sysmem` if a title
misrenders. Details: [`docs/BUG-GMEM.md`](docs/BUG-GMEM.md).

## Layout

```
.github/     CI: build, hardware gate, manual release
docs/        telemetry, design, build, validation, benchmarks, bug reports
patches/     Mesa patches (2 applied, 1 optional sysmem workaround)
scripts/     build, packaging, probe build, device gate
tools/probe/ KGSL/Vulkan probe, functional test, benchmark sources
versions.env pinned Mesa / NDK / glslang versions
```

## License

Project code: MIT ([`LICENSE`](LICENSE), [`NOTICE`](NOTICE)). Driver binaries are
Mesa 3D builds (MIT). Qualcomm, Snapdragon and Adreno are trademarks of Qualcomm
Incorporated; this project is not affiliated with or endorsed by Qualcomm or
Mesa.
