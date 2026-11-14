# On-device validation

Device: Retroid Pocket G2 (Snapdragon G2 Gen 2, `SG6275P`), Android 15 / SDK 35,
kernel 6.1.115. All tests run over ADB as the `shell` user; `/dev/kgsl-3d0` is
world-writable, no root required.

Test binaries: [`tools/probe/`](../tools/probe) — `a22probe` (KGSL + HAL
enumeration) and `a22test` (compute dispatch and image clear/resolve with
verification).

## 1. Load and enumeration

```
=== A22 driver validation ===
chip_id=0x43030b00 gpu_id=0 gmem=1536 KiB mmu=1
ubwc_mode=0x4
highest_bank_bit=16
dlopen ok: libvulkan_freedreno.so
HMI found (Android Vulkan HAL)
HAL open -> 0 (Success)
vkCreateInstance -> 0
physical devices: 1
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
RESULT: driver loaded and enumerated OK
```

`VK_KHR_android_surface` is absent from the ICD because the platform loader owns
surface extensions; the driver implements `VK_ANDROID_native_buffer`.

## 2. Compute dispatch

64 invocations write `i*3+7` into an SSBO; the host verifies every value.
Exercises NIR→IR3 codegen, command stream, KGSL submission and fences.

```
compute dispatch: PASS (64/64 correct)
compute round-trip: 0.240 ms/dispatch (200 submits)
```

## 3. Image clear + tiled resolve

512×512 `R8G8B8A8_UNORM` optimal-tiling image, cleared to (1.0, 0.5, 0.25, 1.0),
copied to a linear buffer, every pixel verified. Exercises UBWC layout, clear
engine and tiled→linear resolve.

```
image clear+resolve: PASS (262144/262144 pixels correct)
```

## 4. Shipped package

The stripped library inside `turnip-a22-26.3.0.adpkg.zip` is byte-identical to
the validated one:

```
libvulkan_freedreno.so   f308afd49c710ab9…   (14.42 MB stripped)
package sha256           9ed76ba72842f4e2…
```

The same two tests pass with no `debug.mesa.*` properties set.

## 5. Negative result: `-march=armv9-a`

An early build used `-march=armv9-a`. LLVM's ARMv9-A baseline implies SVE, which
these Kryo cores (A520/A720/X4) do not implement; the driver died during
enumeration:

```
Fatal signal 4 (SIGILL), code 1 (ILL_ILLOPC)
#00 pc … libvulkan_freedreno.so (blake3_hasher_update+32)
#01 pc … (tu_device_get_build_id(blake3_hasher*)+64)
#02 pc … (tu_physical_device_init(...))
```

Re-targeting `armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16` fixed it.

## 6. Reproduce

```bash
bash scripts/build_probes.sh
adb push build/probe-tools/a22test /data/local/tmp/
unzip -p dist/turnip-a22-26.3.0.adpkg.zip libvulkan_freedreno.so > /tmp/libvulkan_freedreno.so
adb push /tmp/libvulkan_freedreno.so /data/local/tmp/
adb shell chmod 755 /data/local/tmp/a22test
adb shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22test /data/local/tmp/libvulkan_freedreno.so"
```

## 7. Release gate

The gate (`scripts/device_test.sh`) unpacks a package, checks `meta.json`,
pushes the library and `a22test`, and fails unless both tests report PASS.
Result on `turnip-a22-26.3.0.adpkg.zip`:

| Check | Result |
|---|---|
| compute dispatch | PASS (64/64) |
| image clear + resolve | PASS (262144/262144) |
| submit round-trip | 0.240 ms |

## 8. Not covered

* Vulkan CTS (`dEQP`); known weak cases for this GPU class are `compute_nbody`
  and `pipeline_barriers`.
* Long thermal soaks; field runs reached 84–87 °C GPU.
* DXVK/VKD3D workloads; the No Man's Sky field test uses native Vulkan.
