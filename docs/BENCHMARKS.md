# Benchmarks

A small harness for tracking driver performance across releases and for
comparing render modes on this specific GPU. Source:
[`tools/probe/a22bench.c`](../tools/probe/a22bench.c).

## What it measures

| Test | What it stresses |
|---|---|
| 1920×1080 render, 32 full-screen draws, 120 frames | fill rate, tiled vs direct rendering, resolve cost |
| 1280×720 render, same draw count | same, at a resolution that fits fewer tiles |
| 64 MiB device-local buffer copy | memory bandwidth (read + write) |

The render path uses one RGBA8 attachment with `LOAD_OP_CLEAR`/`STORE_OP_STORE`
and no depth, so it measures the colour path and tiling/resolve behaviour, not
LRZ or early-Z.

## Method (important)

The GPU's `msm-adreno-tz` governor ramps clocks quickly and the first run after
idle is always slower. An early version of this harness showed a 13 % swing
between identical runs. The harness now:

1. warms up with continuous submissions for **2 seconds** before measuring;
2. records `clock_mhz` from KGSL for every measurement;
3. runs both resolutions and the copy test in the same process.

Run each mode twice, interleaved, and only compare samples taken at the **same
clock** (835 MHz on this device unless it throttles to 736 MHz).

```bash
bash scripts/build_probes.sh
adb push build/probe-tools/a22bench /data/local/tmp/
adb push dist/stage-A/libvulkan_freedreno.so /data/local/tmp/libvulkan_bench.so
adb shell "LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22bench /data/local/tmp/libvulkan_bench.so"
adb shell "TU_DEBUG=sysmem LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22bench /data/local/tmp/libvulkan_bench.so"
adb shell "TU_DEBUG=gmem LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22bench /data/local/tmp/libvulkan_bench.so"
adb shell "TU_AUTOTUNE_ALGO=prefer_sysmem LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/a22bench /data/local/tmp/libvulkan_bench.so"
```

Env knobs: `BENCH_FRAMES`, `BENCH_DRAWS`, `BENCH_WIDTH`, `BENCH_HEIGHT`.

## Baseline — 2026-11-11 (Mesa 26.3.0)

Measured at 835 MHz (governor settled). Values are ms/frame; lower is better.

| Mode | 1080p, 32 draws | 720p, 32 draws | 64 MiB copy |
|---|---|---|---|
| autotune (default) | 3.88 – 3.93 | 1.87 – 1.92 | 41.1 – 43.1 GB/s |
| `TU_AUTOTUNE_ALGO=prefer_sysmem` | 3.93 | 1.91 | 41.1 GB/s |
| `TU_DEBUG=sysmem` | 3.86 – 3.89 | 1.91 – 1.92 | 40.4 – 41.4 GB/s |
| `TU_DEBUG=gmem` | 3.91 – 4.32* | 2.13 – 2.18* | 36.6 – 38.7 GB/s* |

\* samples that landed at 736 MHz (DVFS) — not comparable with the 835 MHz rows.

## Pipeline creation (shader compile) cost

`a22bench` times `vkCreateGraphicsPipelines` for its two render passes
(1080p and 720p, same trivial shaders). Samples:

| Scenario | 1st pipeline | 2nd pipeline |
|---|---|---|
| fresh process, no cache env | 1.55 ms | 0.15 ms |
| fresh process again (same env) | 1.47 ms | 0.11 ms |
| `MESA_SHADER_CACHE_DIR=/data/local/tmp/sc`, 1st process | 1.58 ms | 0.12 ms |
| same, 2nd process | 1.64 ms | 0.12 ms |

Interpretation:

* Mesa's **in-process** cache works: the second pipeline for the same shader
  costs ~0.1 ms instead of ~1.5 ms.
* **No cross-process cache** is written: the cache directory stays empty and the
  next process pays the full 1.5 ms again, with or without
  `MESA_SHADER_CACHE_DIR`. Compiling a trivial shader is ~1.5 ms; real DXVK
  shaders are far more expensive, which is why first-run stutter is dominated by
  the front-end's `VkPipelineCache` handling, not by the driver's own cache.
* Therefore: for emulation layers, make sure the front-end persists
  `VkPipelineCache` to disk (Winlator/Eden do this in their own settings). There
  is no driver-side switch to make this happen for them.

### What would reduce stutter further

* A shader corpus (hundreds of distinct SPIR-V modules) to profile and optimize
  IR3 compile time — this is the only way to make a PGO or micro-optimization
  case for shader compilation. The two-shader benchmark here is too small for
  that.
* Front-end-side: pre-warm pipeline caches per title, as some Winlator builds
  already do.

## What this says (and does not)

* Render-mode choice is **within noise** for this fill-bound workload on a
  1.5 MiB-GMEM part: sySMEM, GMEM and the autotuner land within ~1 % of each
  other at equal clock. The autotuner is not hurting anything here.
* The dominant variable is **GPU clock** (835 vs 736 MHz ≈ 10 % frame time),
  which is why the harness reports it and why earlier single-run comparisons
  were misleading.
* This is a synthetic workload. It says nothing about real titles, which have
  depth, LRZ, UBWC-compressed textures and many small draws; use it for
  release-over-release regression tracking, not for tuning drirc heuristics.
* A similar harness with depth attachments and many small draws would be needed
  to justify per-app `tu_autotune_algorithm` overrides. Until then, upstream's
  DXVK/VKD3D `prefer_sysmem` default stands.
