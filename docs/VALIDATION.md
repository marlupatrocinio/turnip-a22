# On-device validation

Device: `MC94528AQF092401436` — Retroid Pocket G2, Android 15 (SDK 35),
kernel 6.1.115. Everything below was executed over ADB as the `shell` user
(uid 2000); `/dev/kgsl-3d0` is world-writable, so no root was needed.

Test binaries live in [`tools/probe/`](../tools/probe):

| Binary | Purpose |
|---|---|
| `a22probe` | KGSL ioctls + HAL load + instance/device enumeration + limits |
| `a22test`  | real compute dispatch and image clear/copy with result verification |

## 1. Driver load and enumeration

```
=== A22 driver validation ===
chip_id=0x43030b00 gpu_id=0 gmem=1536 KiB mmu=1
ubwc_mode=0x4
highest_bank_bit=16
dlopen ok: /data/local/tmp/libvulkan_freedreno.so
HMI found (Android Vulkan HAL)
HAL open -> 0 (Success)
vkCreateInstance -> 0
physical devices: 1
instance extensions = 8 (VK_KHR_android_surface: no)
deviceName   = Adreno (TM) 22
apiVersion   = 1.4.363
driverVersion= 0x06803000
vendor/device= 0x5143 / 0x43030b00
maxComputeWG = 1024x1024x1024 inv=2048
maxStorageBuf= 128 MiB
timestampPrd = 52.08 ns
subgroupSize = 128
heap[0] = 5487 MiB flags=0x1
device extensions = 170
  VK_KHR_android_surface: no
  VK_KHR_shader_float16_int8: yes
RESULT: driver loaded and enumerated OK
```

`VK_KHR_android_surface: no` is expected: the probe talks to the driver HAL
directly (`HMI`), bypassing Android's Vulkan loader, which owns the surface
extensions. Drivers implement `VK_ANDROID_native_buffer`, not the surface
extension itself.

## 2. Functional test — compute dispatch

A 64-invocation compute shader writes `i*3+7` to an SSBO; the host reads it
back. This exercises NIR→IR3 code generation, command-stream construction,
KGSL submission, fences and memory allocation.

```
GPU: Adreno (TM) 22 (api 1.4.363)
compute dispatch: PASS (64/64 correct)
compute round-trip: 0.230 ms/dispatch (200 submits)
image clear+resolve: PASS (262144/262144 pixels correct)
RESULT: functional validation finished
```

## 3. Functional test — image clear + tiled resolve

A 512×512 `R8G8B8A8_UNORM` optimal-tiling image is cleared to
`(1.0, 0.5, 0.25, 1.0)`, transitioned, copied to a linear buffer and every pixel
is compared against `(255,128,64,255)`. This exercises UBWC layout, the clear/
blit engine and the tiled→linear resolve path — historically the fragile part of
Turnip on new Adreno generations.

`PASS (262144/262144 pixels correct)`.

## 4. Results per artifact

| Artifact | Load | Compute | Clear/resolve | Round-trip |
|---|---|---|---|---|
| Variant A (unstripped, debug symbols) | PASS | 64/64 | 262144/262144 | 0.230 ms |
| Variant A (stripped, as shipped) | PASS | 64/64 | 262144/262144 | 0.254 ms |
| Variant B (ARMv8.6-A, as shipped) | PASS | 64/64 | 262144/262144 | 0.237 ms |

The compute round-trip difference between variants is ~3 % and within run-to-run
noise on this device; it measures submit+wait latency (0.23 ms), not GPU
throughput.

## 5. Negative result worth recording

The first Variant B build used `-march=armv9-a`. It crashed at device
enumeration:

```
Fatal signal 4 (SIGILL), code 1 (ILL_ILLOPC), fault addr 0x...
#00 pc ... libvulkan_freedreno_B.so (blake3_hasher_update+32)
#01 pc ... (tu_device_get_build_id(blake3_hasher*)+64)
#02 pc ... (tu_physical_device_init(...))
```

LLVM's `armv9-a` implies SVE/SVE2 and these Kryo cores have no SVE. Re-targeting
`armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16` fixed it; the shipped Variant B is
that build (see `docs/DRIVER_DESIGN.md` §5).

## 6. How to reproduce

```bash
adb push tools/probe/a22test /data/local/tmp/a22test
adb push dist/turnip-a22-26.3.0-variantA.adpkg.zip /data/local/tmp/
# unzip on host, push libvulkan_freedreno.so, then:
adb shell chmod 755 /data/local/tmp/a22test
adb shell LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22test \
    /data/local/tmp/libvulkan_freedreno.so
```

The shipped `.so` links only against system libraries, so no extra `.so` needs to
be pushed.

## 7. Not covered by this validation

* Real games / emulator titles (no benchmark corpus was installed on the device).
* Thermal throttling behaviour over long sessions.
* Android-surface presentation through an app (Winlator/Eden) — requires a
  front-end using libadrenotools; the HAL-level load and rendering path is
  validated, the loader integration is the front-end's responsibility.
* Vulkan CTS conformance (`dEQP`), famously sensitive on this GPU class for
  `compute_nbody` and `pipeline_barriers`; worth running before publishing wider.
