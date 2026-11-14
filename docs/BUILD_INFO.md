# Build provenance — Turnip A22 26.3.0

## Target device

| Field | Value |
|---|---|
| Model | Retroid Pocket G2 (Moorechip) |
| SoC | Qualcomm Snapdragon G2 Gen 2 (`SG6275P`) |
| GPU | Adreno (TM) 22, `chip_id 0x43030b00`, `gen7-11-0` (Adreno 735 class) |
| Android | 15 (SDK 35), kernel 6.1.115-android14-11 |

## Sources

| Component | Version |
|---|---|
| Mesa | 26.3.0 (`archive.mesa3d.org`, `.sig` published upstream) |
| Android NDK | r28c — clang 19.0.1, target `aarch64-linux-android35` |
| glslang | 16.6.0 (Turnip needs >= 12.2 for BVH shaders) |
| Patches | `0001-freedreno-add-A22-device-id.patch`, `0002-meson-allow-posix-flex-when-cross-compiling-for-android.patch` |

## Meson configuration

```
--buildtype=release -Db_ndebug=true
-Dplatforms=android -Dplatform-sdk-version=35 -Dandroid-stub=true
-Dvulkan-drivers=freedreno -Dgallium-drivers= -Dfreedreno-kmds=kgsl
-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled
-Dopengl=false -Dllvm=disabled -Dshared-glapi=disabled
-Dandroid-libbacktrace=disabled -Dvideo-codecs= -Dbuild-tests=false -Dtools=
-Dvalgrind=disabled -Dvulkan-beta=true -Dwerror=false
cpp_link_args: -fuse-ld=lld -static-libstdc++
```

Compiler flags: `-march=armv8.6-a+crypto+crc+dotprod+fp16+i8mm+bf16`,
`-fomit-frame-pointer`, `-fno-semantic-interposition`. The library is stripped
with `llvm-strip --strip-unneeded` and depends only on system libraries (no
`libc++_shared.so`).

## Validation (on device, shell user)

| Check | Result |
|---|---|
| HAL load (`HMI`), `vkCreateInstance` | PASS |
| Device identity | `Turnip Adreno (TM) 22`, Vulkan 1.4.363 |
| Compute dispatch | 64/64 correct |
| Image clear + tiled resolve | 262144/262144 pixels correct |
| Compute submit round-trip | 0.230–0.254 ms |

Not built with LTO: Mesa 26.3 refuses `-Db_lto=true`
(`Building Mesa with LTO is not supported.`). See `docs/DRIVER_DESIGN.md` §3.4.

## Release artifacts

Checksums for the published packages live next to the release assets
(`SHA256SUMS.txt`), not in this file, so they cannot drift from the binaries.

Reproduce with `./scripts/build_turnip_g2gen2.sh` or the `build` workflow.
