# Driver design & engineering notes

## 1. Premise check: does Mesa already support the A22?

The brief assumed the Snapdragon G2 Gen 2 / Adreno A22 was an unknown chip that
community drivers ignored. The live hardware says otherwise, and this is the
single most important engineering finding of the project:

* KGSL reports `chip_id = 0x43030b00`.
* The device tree says `qcom,adreno-gpu-gen7-11-0`.
* Mesa's `freedreno_devices.py` contains
  `GPUId(chip_id=0xffff43030B00, name="FD735")` — a wildcard entry that matches
  `0x43030b00` (wildcard fuse-id + `0xffff` mask), using the Adreno 735 profile,
  which is itself a `CHIP.A7XX`, `a7xx_gen2` configuration.
* The GPU firmware in `/vendor/firmware` is `gen71100_sqe.fw` — the 7.11.0
  generation, i.e. the same micro-architecture as Adreno 735.

So upstream Turnip already boots and runs on this device; the FauxVkDB database
lists a dozen Turnip builds reporting `Adreno (TM) 735` for this exact
`0x5143:0x43030b00` device.

**What this means:** inventing a new hardware descriptor with different CCU
counts, tile limits or magic registers would be fabrication, not tuning, and
would risk corruption. The correct work was to *verify* the descriptor against
the silicon, give the device a truthful identity, and put the effort where it
actually pays off: build-system correctness, ISA-specific code generation,
packaging and validation.

## 2. Descriptor verification

| Profile field (Mesa FD735) | Expected | Measured on this device | Verdict |
|---|---|---|---|
| `num_ccu` | 3 | not directly readable from userspace; consistent with the 2016×2032 tile ceiling and with all Turnip builds running | accepted |
| GMEM | 1.5 MiB (`drm-shim` value 1024+512 KiB) | `gmem_sizebytes = 1,572,864` | **exact match** |
| `highest_bank_bit` | 16 (table) | KGSL `highest_bank_bit = 16` | **match** |
| UBWC | gen2 UBWC 4.0 | `ubwc_mode = 0x4` | **match** |
| `a7xx_gen2` feature set | gen2 | DT `gen7-11-0`, firmware `gen71100` | **match** |
| `enable_tp_ubwc_flag_hint` | true (A735 override) | n/a (runtime) | inherited |

Conclusion: the FD735 `A7xxGPUInfo` block is the correct descriptor for this
part, including its `raw_magic_regs` table. No register values were changed.

## 3. Changes made

### 3.1 Exact-match device identity (`patches/0001`)

```diff
 add_gpus([
+        GPUId(chip_id=0x43030B00, name="Adreno (TM) 22"),
         GPUId(chip_id=0xffff43030c00, name="Adreno X1-45"),
         GPUId(chip_id=0xffff43030B00, name="FD735"),
```

`fd_dev_info_raw()` returns the first matching record, and an exact `chip_id`
match wins before the wildcard is consulted, so this selects a truthful name
(`Turnip Adreno (TM) 22`) while keeping the byte-identical hardware profile.

### 3.2 Windows-host cross-compile fix (`patches/0002`)

Mesa's build system only adds `--wincompat` to `win_flex` when the **build**
machine is Windows, even when cross-compiling for Android. `--wincompat`
generates `#include <io.h>` and Windows CRT calls, which cannot compile for
Bionic. The patch additionally requires the **host** machine to be Windows:

```diff
-if build_machine.system() == 'windows'
+if build_machine.system() == 'windows' and host_machine.system() == 'windows'
```

This is a no-op for Linux/CI builds and makes Windows-host builds correct.

### 3.3 Compiler / toolchain configuration

Both variants:

* NDK r28c clang 19.0.1, `--target=aarch64-linux-android35`, API 35
  (matches the device's SDK level exactly).
* `--buildtype=release -Db_ndebug=true` → `-O3`, all `assert()` and debug
  logging compiled out.
* `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` (Mesa default in
  release), `-Wl,-Bsymbolic`, `-Wl,--build-id=sha1`.
* `-static-libstdc++`: the shipped `.so` needs **only system libraries**
  (`libc`, `libm`, `libdl`, `libz`, `liblog`, `libhardware`, `libnativewindow`,
  `libsync`). No `libc++_shared.so` to bundle or to collide with the host app.
* `-fno-emulated-tls` (Mesa's Android default) so TLS uses ELF TLS.

Variant B adds:

* `-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16`
  chosen from the device's actual `/proc/cpuinfo` feature list.
* `-fomit-frame-pointer`, `-fno-semantic-interposition`.
* Fully stripped with `llvm-strip --strip-unneeded`.

### 3.4 LTO: requested, investigated, rejected

The brief asked for `-Db_lto=true -Db_lto_mode=thin`. Mesa 26.3 hard-refuses it:

```
meson.build:52: ERROR: Building Mesa with LTO is not supported.
```

This is an upstream policy decision, not a missing flag; forcing LTO by patching
the check would produce a driver upstream explicitly considers unsupported (the
Vulkan HAL has to export a fixed symbol set, and Mesa's `--gc-sections` +
version-script link plus ThinLTO has historically broken the ICD/HMI surface).
Both packages therefore ship `-O3` without LTO. Do not expect a size or speed
win from re-enabling it; if you want a project-level PGO/LTO experiment, keep it
out of release packages.

### 3.5 What was deliberately *not* changed

* **No invented CCU/GMEM/tile numbers.** The measured values already match.
* **No `raw_magic_regs` edits.** Blind register tweaks are how drivers brick
  rendering; the A735 table is the upstream-validated set for this silicon.
* **No global autotuner override.** See §4.
* **No `tu_enable_ssbo_emulation` forced globally.** It costs performance and is
  only needed for >128 MiB storage-buffer views; upstream's driconf already
  enables it for the engines that need it (UE5 + vkd3d).
* **No KGSL syncobj / timestamp patch.** Mesa 26.3 already ships the merged
  implementation (`kgsl_syncobj`, `KGSL_SYNCOBJ_STATE_TS` and timeline-fence
  handling in `tu_knl_kgsl.cc`); the old community zero-timeout polling patch is
  obsolete and is not applied.
* **No IR3 scheduler/register-pressure heuristics.** Changing them without a
  workload corpus is guesswork; the shipped binaries use upstream IR3 with
  release optimisations.

## 4. GMEM / autotuner calibration for a 1.5 MiB part

Turnip's autotuner picks GMEM (tiled) vs SYSMEM (direct) per render pass using a
cost model over draw-call bandwidth.

* GMEM is only **1.5 MiB**, so a 1080p RGBA8 target (8.3 MiB) cannot be tiled in
  one go — the driver splits it into many tiles. That is still often a win
  because it avoids sysmem resolve traffic, but the crossover is workload
  dependent, which is exactly what the autotuner is for.
* The size is read at runtime from KGSL (`device->gmem_size = info.gmem_sizebytes`
  in `tu_knl_kgsl.cc`), so **no device-table GMEM constant exists to tune**.
  `TU_GMEM=1572864` can override it for experiments.
* Upstream driconf already pins `tu_autotune_algorithm="prefer_sysmem"` for
  DXVK/VKD3D-Proton — the correct default for desktop titles on a small-GMEM
  part, and it is baked into the binary through `driconf_static.h`.
* For native Android emulators the useful experiments are
  `TU_AUTOTUNE_ALGO=prefer_gmem` (favour tiling, less resolve traffic) and
  `TU_AUTOTUNE_FLAGS=big_gmem,tune_small`. These are runtime env vars; they are
  documented rather than hard-coded because without per-title measurements a
  baked-in choice is a guess.

## 5. ISA tuning: why `armv9-a` crashed and `armv8.6-a` works

The first Variant B attempt used `-march=armv9-a+…`. The driver died with
`SIGILL` in `blake3_hasher_update` while enumerating devices.

Reason: LLVM's `armv9-a` baseline implies SVE/SVE2, and these Kryo cores
(A520/A720/X4, `0x41` implementer parts `0xd80/0xd81/0xd82`) do **not**
implement SVE — `/proc/cpuinfo` lists no `sve`. Clang therefore emitted SVE
instructions that the CPU cannot decode.

Fix: target the exact architectural feature set the cores advertise —
`armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16`. Variant B then passed the same
functional tests as Variant A.

This is also why Variant A exists: a generic `armv8-a` build is the safe choice
for users who are not certain which bin/SoC variant they have.

## 6. Known limitations / future work

* `VK_KHR_android_surface` is not advertised by the ICD itself. That is normal:
  on Android the platform loader owns surface extensions and the driver
  implements `VK_ANDROID_native_buffer`. Our probe talks to the HAL directly, so
  it does not see loader-provided extensions.
* Subgroup size is 128 on Turnip vs 64 on the stock blob. Well-written apps query
  it; apps that hard-code 64 can use `TU_DEBUG`/driconf `tu_restrict_subgroup_size_64`.
  Not enabled by default because it costs performance in shaders that benefit
  from wider subgroups.
* PGO is the remaining big lever: profile a representative workload on-device,
  merge with `llvm-profdata`, rebuild. Not shipped here because it needs a
  reproducible workload corpus to avoid over-fitting one game.
* If future Mesa adds an A22-specific quirk, only the `GPUId` line in patch 0001
  needs to change.
