# Known issue: forced GMEM corrupts rendering on Adreno A22

**Status:** confirmed on hardware, workaround available, root cause not fully
isolated. Default autotuner behaviour is unaffected in everything tested.

## Symptom

With tiled rendering forced, The Legend of Zelda: Breath of the Wild (Eden
nightly, Vulkan) renders corrupted: the player character becomes a flat black
silhouette, terrain lighting and shadows are wrong, distant geometry turns into
solid shapes. Frame rate *appears* fine.

Screenshot: [`artifacts/gmem-corruption.png`](artifacts/gmem-corruption.png).

Everything renders correctly with `TU_DEBUG=sysmem` and with the default
autotuner.

## Reproducer

```
adb shell setprop debug.mesa.tu.debug gmem,startup   # or TU_AUTOTUNE_ALGO=prefer_gmem
# launch BOTW, load any save
```

Both forcing paths reproduce it:

| Trigger | Result |
|---|---|
| `TU_DEBUG=gmem` | corruption |
| `TU_AUTOTUNE_ALGO=prefer_gmem` | corruption |
| `TU_DEBUG=sysmem` | correct |
| default autotuner | correct |

## Isolation performed

| Test | Result | Conclusion |
|---|---|---|
| generic `armv8-a` build + `gmem` | corrupt | not an ARMv8.6 codegen issue |
| ARMv8.6-A build + `gmem` | corrupt | — |
| `gmem,nolrz` | corrupt | LRZ not involved |
| `gmem,noubwc` | corrupt | UBWC not involved |
| `num_ccu=2` in the device profile + `gmem` | **device loss** (GPU hang) | CCU count hypothesis rejected |
| `num_ccu=3` (stock profile) + `gmem` | corrupt | — |

The `num_ccu=2` experiment is interesting: changing the presumed CCU count turned
visual corruption into a hard GPU hang, which means the tiling path is sensitive
to the GMEM carve-up but the stock value is not simply "too big" or "too small".

## Performance of the workaround

BOTW on Eden, 3-minute captures, GPU at 900 MHz, 30 fps cap:

| Build | Slow frames (>1.5x p50) | p50 |
|---|---|---|
| default autotuner, run 1 | 7.2 % | 33.33 ms |
| default autotuner, run 2 | 6.2 % | 33.33 ms |
| `TU_DEBUG=sysmem` | 9.4 % | 33.33 ms |
| forced-sysmem build | 9.8 % | 33.33 ms |

So **disabling tiling unconditionally is not free**: ~3 percentage points more
slow frames. The default autotuner is both correct and faster in this title,
meaning it selects GMEM only for passes where it works, and SYSMEM where it
does not.

## Workarounds

1. **Runtime, no rebuild** — for any title that shows corruption:

   ```
   adb shell setprop debug.mesa.tu.debug sysmem
   ```

   (or have the front-end set `TU_DEBUG=sysmem` for that container). Reset with
   an empty value afterwards.

2. **Build-time safe mode** — [`patches/optional/0003-force-sysmem-on-adreno-a22.patch`](../patches/optional/0003-force-sysmem-on-adreno-a22.patch)
   forces SYSMEM for `chip_id 0x43030b00` inside `use_sysmem_rendering()`. Not
   applied by default because of the cost above; useful for a "compatibility"
   build.

3. **Do not use** `TU_AUTOTUNE_ALGO=prefer_gmem` or `TU_DEBUG=gmem` on this
   device.

## Upstream report (to be filed by the device owner)

Mesa's contribution policy excludes AI-authored reports, so this section is a
draft for a human to file at
<https://gitlab.freedesktop.org/mesa/mesa/-/issues>:

> **freedreno/a7xx: forced GMEM rendering corrupts on Adreno A22 (chip_id
> 0x43030b00, gen7-11-0)**
>
> Device: Snapdragon G2 Gen 2 (SG6275P), Android 15, KGSL, GMEM 1.5 MiB,
> UBWC 4.0, highest_bank_bit 16, Mesa 26.3.0.
>
> With `TU_DEBUG=gmem` (or `TU_AUTOTUNE_ALGO=prefer_gmem`) Breath of the Wild
> renders with a black player model and broken terrain lighting; `TU_DEBUG=sysmem`
> and the default autotuner render correctly. Reproduces with a generic
> `armv8-a` build, and with `nolrz`/`noubwc`. Setting `num_ccu=2` in the device
> profile turns it into a GPU hang (device loss) instead of visual corruption.
> The device matches the FD735/A735 entry (chip_id 0x43030b00,
> `qcom,adreno-gpu-gen7-11-0`, firmware gen71100), so the A735 profile is being
> used unchanged.
>
> Attached: screenshot of the corruption, logcat of the device-loss case.

## What we still do not know

* Whether the corrupting pass is a specific attachment format, MSAA, or a
  render-pass size threshold — the autotuner's safe choice masks it.
* Whether real A735 hardware is affected (same `chip_id`, same profile); if so,
  this is an upstream descriptor/CCU-layout bug rather than an A22 quirk.
