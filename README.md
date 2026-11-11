<div align="center">

# Turnip A22

**A from-source Mesa Turnip driver package for the Snapdragon G2 Gen 2 handheld — Adreno (TM) 22 / `gen7-11-0`.**

[![Mesa](https://img.shields.io/badge/Mesa-26.3.0-2b6cb0)](https://mesa3d.org)
[![Android](https://img.shields.io/badge/Android-15%20%2F%20API%2035-3ddc84)](https://developer.android.com)
[![GPU](https://img.shields.io/badge/GPU-Adreno%20A22-e53e3e)](#supported-devices)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Status](https://img.shields.io/badge/status-validated%20on%20hardware-success)](#validation)

</div>

---

Turnip A22 builds [Mesa 3D](https://gitlab.freedesktop.org/mesa/mesa)'s freedreno
Vulkan driver for Qualcomm's handheld SoC and packages it as an
[AdrenoTools](https://github.com/bylaws/libadrenotools) driver — the same
mechanism Winlator, GameHub, Eden, Citron, ARMSX2 and Vita3K use to load custom
user-space drivers without root.

It exists because the Snapdragon G2 Gen 2 sits in an awkward spot: it is new
consumer hardware, but its GPU is the **7.11.0 generation already known to Mesa
as Adreno 735**, so most "custom driver" repos either ignore it or ship builds
tuned for 8-series flagship parts. This repo pins down what the silicon actually
is, builds it properly, and validates the result on a real device.

## Why another Turnip build?

| | |
|---|---|
| **Misidentified hardware** | The stock blob calls it `Adreno (TM) 22`; Mesa matches it through the A735 wildcard, so drivers report `Adreno (TM) 735`. Both are the same `gen7-11-0` die. This build adds an exact-match identity so logs, bug reports and per-device expectations are truthful. |
| **Wrong build flags** | `-march=armv9-a` looks right but LLVM implies SVE, which these Kryo cores do not implement — the driver dies with `SIGILL` at device enumeration. This repo documents that and ships an `armv8.6-a` build that actually runs. |
| **Needs the right HAL form** | AdrenoTools needs an Android Vulkan **HAL** (`HMI`), not a desktop ICD. The build here produces one, with `libc++` statically linked so there is no `libc++_shared.so` to bundle or collide with. |
| **Nothing validated** | Community packages are rarely tested beyond "a game booted". This one ships the test binaries used to verify it: HAL load, compute dispatch, and a tiled image clear/resolve with per-pixel checks. |

> Upstream Mesa already boots on this hardware. If you only want a stock Turnip
> build, any recent Mesa will work. What this project adds is a **correct,
> reproducible, hardware-validated package** — not a new GPU backend.

## Supported devices

| Device | SoC | GPU | Status |
|---|---|---|---|
| Retroid Pocket G2 (Moorechip) | Snapdragon G2 Gen 2 (`SG6275P`) | Adreno (TM) 22, `chip_id 0x43030b00`, `gen7-11-0` | ✅ built and validated on hardware |
| Other `gen7-11-0` parts (Adreno 735 class) | — | same `chip_id` family | ⚠️ expected to work, not tested here |

Requirements: arm64 Android 9+ (API 28+), a front-end that supports
AdrenoTools driver import. No root.

## Packages

| Variant | ISA | Size | Use it when |
|---|---|---|---|
| **A — Balanced** | generic `armv8-a` | ~2.6 MB zip | Default. Maximum compatibility, validated. |
| **B — Performance** | `armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16` | ~2.6 MB zip | You have this exact SoC family and want ISA-specific codegen. Same driver, same validation. |

Both are Mesa 26.3.0, `-O3` release, assertions and debug logging compiled out,
`libc++` statically linked, and fully stripped. Download the latest `.adpkg.zip`
from [Releases](../../releases).

## Install

1. Copy the zip to the device (`adb push turnip-a22-*.adpkg.zip /sdcard/Download/`).
2. Open your emulator's **GPU driver manager** (Winlator: *Adrenotools GPU
   Drivers → Install*; Eden: *Settings → GPU → Install driver*).
3. Select the zip, then activate **Turnip A22 Balanced**.
4. Launch a title.

Expected in the app's driver info or `adb logcat`: `Turnip Adreno (TM) 22`,
Vulkan `1.4.363`. To uninstall, delete the driver from the same manager — nothing
outside the app's private storage is touched.

## Validation

Tested over ADB on a Retroid Pocket G2 (Android 15 / SDK 35, kernel 6.1.115),
using the binaries in [`tools/probe/`](tools/probe):

```
HAL load (HMI)            PASS, vkCreateInstance PASS
Device                    Turnip Adreno (TM) 22, Vulkan 1.4.363
Compute dispatch          64/64 correct
Image clear + resolve     262144/262144 pixels correct
Compute submit round-trip 0.230–0.254 ms
```

Both variants, stripped exactly as shipped, pass. Raw output and method:
[docs/VALIDATION.md](docs/VALIDATION.md).

Not covered yet: real game titles, Vulkan CTS (`dEQP`), long thermal soaks.

## Building

```bash
git clone https://github.com/<you>/turnip-a22
cd turnip-a22
./scripts/build_turnip_g2gen2.sh
# -> dist/turnip-a22-26.3.0-variant{A,B}.adpkg.zip
```

The script downloads the pinned Mesa and NDK versions from
[`versions.env`](versions.env), applies [`patches/`](patches), builds both
variants, strips and packages them. CI does the same on every push
([`.github/workflows/build.yml`](.github/workflows/build.yml)); publishing is a
manual step that attaches validated packages to an existing tag
([`.github/workflows/release.yml`](.github/workflows/release.yml)).

Full toolchain details, cross files and Windows-host notes:
[docs/BUILD.md](docs/BUILD.md).

## Tuning

The driver auto-detects the 1.5 MiB GMEM, UBWC 4.0 mode and bank bits from KGSL.
Runtime knobs for per-title experiments:

| Variable | Values | Notes |
|---|---|---|
| `TU_AUTOTUNE_ALGO` | `bandwidth` (default), `profiled`, `profiled_imm`, `prefer_sysmem`, `prefer_gmem` | GMEM vs SYSMEM render-pass heuristic |
| `TU_AUTOTUNE_FLAGS` | `big_gmem`, `tune_small`, `preempt_optimize` | autotuner modifiers |
| `TU_GMEM` | `1572864` | override detected GMEM (diagnostics only) |
| `TU_DEBUG` | `startup`, `sysmem`, `gmem`, `nobin`, `noubwc`, `nolrz`, `flushall`, `perf`, … | force render modes / disable features |
| `FD_DEV_FEATURES` | `has_*=0/1` | override individual device features |

DXVK/VKD3D-Proton already get `tu_autotune_algorithm="prefer_sysmem"` from
upstream's baked-in driconf, which is the right default for a small-GMEM part.
For native Android emulators, `TU_AUTOTUNE_ALGO=prefer_gmem` is the first thing
to try if a title is resolve-bandwidth bound — measure before keeping it.

## Repository layout

```
.github/            CI: build on push, publish on tag, issue template
docs/               hardware telemetry, design notes, build & validation reports
patches/            the only two Mesa patches this build applies
scripts/            reproducible Linux/CI build + AdrenoTools packaging
tools/probe/        on-device telemetry and functional test sources
tools/win/          helper for Windows hosts where ninja cannot spawn children
versions.env        pinned Mesa / NDK / glslang versions
```

More detail, including what is deliberately *not* committed:
[docs/REPO_STRUCTURE.md](docs/REPO_STRUCTURE.md).

## FAQ

**Is this faster than stock Turnip?**
Variant B targets the exact CPU features of these Kryo cores, which helps
driver-side (CPU) work; Variant A is byte-for-byte standard `armv8-a` codegen.
Neither changes GPU behaviour, because the hardware descriptor already matches
the silicon. No inflated benchmark claims here — measure on your titles.

**Why not enable LTO?**
Mesa 26.3 refuses it outright: `Building Mesa with LTO is not supported.` This
is upstream policy, not a missing flag. See [docs/DRIVER_DESIGN.md](docs/DRIVER_DESIGN.md) §3.4.

**Why does `TU_DEBUG=startup` show Adreno 735 sometimes?**
If you run an unpatched upstream build, that is expected — it matches through
the A735 wildcard. This repo's package reports `Adreno (TM) 22`.

**Can I use this on an Adreno 740/750 device?**
Don't. The package is built and tuned for one chip family; use a build made for
yours.

**Does it need root?**
No. AdrenoTools loads the HAL inside the app process.

## Contributing

Bug reports are welcome — please use the issue template and include device,
driver variant, app/title, and `adb logcat -b crash -d` output. Driver bugs that
are not specific to this packaging belong upstream at
[Mesa's freedreno issue tracker](https://gitlab.freedesktop.org/mesa/mesa/-/issues);
this project keeps its two patches local and does not submit them upstream.

## License & trademarks

Project code and packaging: MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
Driver binaries are Mesa 3D builds (MIT). Qualcomm, Snapdragon, Adreno and
AdrenoTools are trademarks of their respective owners; this project is not
affiliated with or endorsed by Qualcomm or Mesa.
