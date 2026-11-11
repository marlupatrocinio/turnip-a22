# Hardware telemetry — Snapdragon G2 Gen 2 (Adreno A22)

All values below were read live from serial `MC94528AQF092401436`
(Retroid Pocket G2, Moorechip) over ADB on 2026-11-10, with read-only sysfs,
`getprop`, `/proc` and KGSL ioctls. Nothing was written to the device except
files under `/data/local/tmp`.

Raw probe: [`tools/probe/a22probe.c`](../tools/probe/a22probe.c) (KGSL ioctls +
Vulkan HAL enumeration).

## 1. Platform identity

| Property | Value |
|---|---|
| `ro.product.model` / `ro.product.device` | Retroid Pocket G2 / `pineapple` |
| `ro.soc.model` | `SG6275P` |
| `ro.board.platform` | `pineapple` |
| `/sys/devices/soc0/chip_id` | `SGP_LAMMA` |
| `/sys/devices/soc0/machine` | `SG_CLIFFS7P` |
| `/sys/devices/soc0/sku` | `SG_CLIFFS7P-1-AB` |
| `/sys/devices/soc0/soc_id` | `700` |
| `/sys/devices/soc0/chip_family` | `0x94` |
| `/sys/devices/soc0/platform_version` | `65536` |
| `/sys/devices/soc0/revision` | `1.0` |
| `ro.build.fingerprint` | `qti/pineapple/pineapple:15/AQ3A.250226.002/eng.RPG2.20260123.180002:user/dev-keys` |
| Android | 15 (SDK 35), security patch 2025-03-05, vendor image 14 |
| Kernel | `6.1.115-android14-11-maybe-dirty` (built with clang 17.0.2) |
| Boot state | `ro.boot.verifiedbootstate=orange`, `flash.locked=0` (unlocked, dev-keys image) |

## 2. GPU identity (KGSL)

Device-tree node: `/sys/class/kgsl/kgsl-3d0/device/of_node/compatible`

```
qcom,adreno-gpu-gen7-11-0   qcom,kgsl-3d0
```

`/sys/class/kgsl/kgsl-3d0/gpu_model` = **`Adreno22`**

`KGSL_PROP_DEVICE_INFO` ioctl (`struct kgsl_devinfo`):

| Field | Value | Meaning |
|---|---|---|
| `chip_id` | **`0x43030b00`** | the ID Mesa's device table matches against |
| `device_id` | `0x00000001` | |
| `gpu_id` | `0` | |
| `mmu_enabled` | `1` | |
| `gmem_sizebytes` | **`1572864`** (1.5 MiB) | on-chip tile memory |
| `gmem_gpubaseaddr` | `0x0` | |

Other KGSL properties:

| Property | Value |
|---|---|
| `KGSL_PROP_UBWC_MODE` | `0x4` (UBWC 4.0) |
| `KGSL_PROP_HIGHEST_BANK_BIT` | `16` |
| `KGSL_PROP_UCHE_GMEM_VADDR` | `0x1000000` |
| `KGSL_PROP_GPU_VA64_SIZE` | `0x4000000000` (256 GiB) |

Misc sysfs: `bus_split=1`, `hwcg=1`, `ifpc=1`, `acd=1`, `lm=0`, `lpac=0`,
`popp=0`, `preemption=1`, `ft_policy=0xC2`, `reset_count=43`, `temp≈29.2 °C`,
`throttling=0`, `gpu_busy_percentage≈3 %`.

### GPU clocks

`gpu_available_frequencies` (Hz): `255 353 500 633 684 736 835 900 950` MHz,
9 power levels; `default_pwrlevel=8`, `max_gpuclk=950 MHz`; idle clock 736 MHz
at the time of capture; governor `msm-adreno-tz`.

### GPU firmware

`/vendor/firmware` contains:

```
gen71100_sqe.fw      <- 7.11.0 SQE microcode (also used by Adreno 735)
gen71100_gmu.bin
gen71100_zap.mbn
gen70900_sqe.fw / gen70900_aqe.fw / gmu_gen70900.bin   (sibling generation)
```

The `gen71100` firmware name and the DT compatible string both confirm that this
is the **7.11.0 GPU generation**, i.e. the Adreno 735 microarchitecture, not a
new/different GPU family.

## 3. CPU

| Cluster | Cores | Part | Max clock |
|---|---|---|---|
| Efficiency | 4 | `0xd80` = Cortex-A520 | 1.90 GHz |
| Performance | 3 | `0xd81` = Cortex-A720 | 2.61 GHz |
| Prime | 1 | `0xd82` = Cortex-X4 | 2.80 GHz |

`/proc/cpuinfo` features (arm64): `fp asimd aes pmull sha1 sha2 crc32 atomics
fphp asimdhp cpuid asimdrdm jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512
asimdfhm dit uscat ilrcpc flagm sb paca pacg dcpodp flagm2 frint i8mm bf16 dgh
bti ecv afp`

→ ARMv8.6-class ISA with dotprod/fp16/i8mm/bf16/crypto/LSE, **no SVE/SVE2**.
`dalvik.vm.isa.arm64.variant = kryo300`.

## 4. System

| Item | Value |
|---|---|
| RAM | 7,492,216 kB total (~7.5 GiB) |
| Display | 1080×1920, density 360 |
| GPU thermal zones | `gpuss-0..3`, ~29 °C idle |
| EGL | `Qualcomm, Adreno (TM) 22, OpenGL ES 3.2 V@0762.31` |

## 5. Stock Qualcomm Vulkan driver (`cmd gpu vkjson` / `dumpsys gpu`)

| Field | Value |
|---|---|
| `deviceName` | `Adreno (TM) 22` |
| `vendorID` / `deviceID` | `0x5143` / `0x43030B00` |
| `apiVersion` | 1.3.0 (4206592) |
| Driver build | `ae801e1e7c`, `I15873b352d`, 2025-01-03, compiler E031.45.02.23 |
| `subgroupSize` | 64 |
| `maxComputeWorkGroupInvocations` | 1024 |
| `maxStorageBufferRange` | 128 MiB |
| `timestampPeriod` | 52.083 ns |
| `maxImageDimension2D` | 16384 |

## 6. Turnip (this project) on the same device

| Field | Value |
|---|---|
| `deviceName` | `Turnip Adreno (TM) 22` |
| `apiVersion` | 1.4.363 |
| `driverVersion` | `0x06803000` |
| `vendorID` / `deviceID` | `0x5143` / `0x43030b00` |
| `subgroupSize` | 128 |
| `maxComputeWorkGroupInvocations` | 2048 |
| `maxStorageBufferRange` | 128 MiB |
| Memory heap | 1 heap, 5487 MiB (`DEVICE_LOCAL`) |
| Device extensions | 170 |

Differences worth knowing versus the stock blob: Turnip exposes Vulkan 1.4.363
vs 1.3.0, twice the compute invocations per workgroup, and 128-wide subgroups
instead of 64.
