# Upstream review: StevenMXZ Adreno-Tools-Drivers v39

Release: <https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/tag/v39>
(`Turnip Gen8 V39 / Turnip v27.0.0-R2`, published 2026-11-07)

## What v39 actually is

| Item | v39 | Ours (26.3.0-a22.2) |
|---|---|---|
| Mesa | 27.0.0-devel, `git-2648b2059f` (upstream `main`) | 26.3.0 release tarball |
| Vulkan | 1.4.367 | 1.4.363 |
| NDK / API | r29 / API 36 | r28c / API 35 |
| Variants | Gen8, Gen8-patched, v27-R2, v27-R2-OneUI | A (generic ISA), B (ARMv8.6-A) |
| Device naming | `Adreno (TM) 735` (A22 has no entry) | `Adreno (TM) 22` (our patch) |

The "patched" variants are aimed at A8xx (Gen8) hardware; the a7xx variant is
`Turnip_v27.0.0-R2.zip`, which is what matters for us.

## Patch-by-patch applicability

| Patch / script | What it does | Applies to A22? |
|---|---|---|
| `kgsl-syncobj-merge-ts-fd.patch` | fixes timestamp→fd conversion in `kgsl_syncobj_merge` | **No** — 26.3 already converts the aggregate `ret` to an fd before merging |
| `kgsl-zero-timeout-poll.patch` | polls retired timestamp instead of a blocking zero-timeout wait | **No** — 26.3 already has `kgsl_poll_timestamp()`; upstream MR merged |
| `apply_a7xx_gen1_quirks.py` | `has_early_preamble=False` etc. for A7xx **gen1** | **No** — our GPU is `a7xx_gen2` |
| `apply_a7xx_gen2_ubwc_hint.py` | `enable_tp_ubwc_flag_hint=True` for FD740/X1-85 | **No** — our A735/A22 block already sets it |
| `force_sysmem_no_autotuner.patch` | `use_sysmem_rendering()` returns true | **No** — measured ~3 pp more slow frames (see `FIELD_TEST_NMS.md`) |
| `0002-tu-ir3-…-half-a-wave.patch` | subgroup 32 lowering for A8xx 64-wide waves | **No** — a8xx-specific |
| `8g2_ui_glitch.patch` | OneUI compositor corruption workaround | **No** — not this firmware |
| `a8xx_gen8.patch`, `a840v2.py`, `a8xx_shared_mem.py` | Gen8 descriptor/VM work | **No** |
| `vk_sync_timeline.patch` | timeline-semaphore work | Not evaluated; 26.3 already has timeline sync types |
| D32S8 `EARLY_Z_LATE_Z` revert | a7xx performance workaround revert | **Not applied** — a hang workaround; risky without evidence |

Conclusion: **there is no A22-relevant patch in v39 that we do not already have**
(or that we measured to be a regression). The only substantive difference is the
Mesa version itself.

## Binary comparison (same GPU, both stripped)

| Section | v39 (Mesa 27) | ours (Mesa 26.3) |
|---|---|---|
| `.text` | 4.06 MB | 3.99 MB |
| `.rodata` | **3.01 MB** | **8.62 MB** |
| `.data.rel.ro` | 0.57 MB | 0.59 MB |
| File size | 7.98 MB | 13.75 MB |
| Dynamic symbols | 2,157 | 2,157 |

Code size is essentially identical; the 5.6 MB difference is read-only data.
Likely candidates: the ISA decode tables and generated register metadata that
26.3 links unconditionally, or build flags in their recipe. It affects load time
and package size, not steady-state performance, but it is worth understanding if
we bump Mesa.

v39 also carries strings for a shader-binary cache (`MesaVkShaderBin`) and an
on-disk-cache timestamp check that 26.3 does not have — a hint that Mesa 27 has
shader-cache work that could matter for stutter.

## Measured result (device, NMS, same scene)

| Run | Avg FPS | p50 | p95 | >33 ms | 60 fps frames |
|---|---|---|---|---|---|
| Turnip A22 26.3 (ours) | 31.8 | 33.33 | 33.33 | **1.5 %** | 20.6 % |
| Stock Qualcomm | 29.1 | 33.33 | 50 | 9.5 % | 18.6 % |
| v39 Mesa 27, cold cache | 24.3 | 33.33 | 66.67 | 40.7 % | 9.1 % |
| v39 Mesa 27, warm cache | **33.1** | 16.67 | 50 | 25.3 % | 53.7 % |

Mesa 27 is ~4 % faster on average and reaches 60 fps for half the frames, but
25 % of its frames fall below 30 fps versus 1.5 % on our build. For a handheld
targeting a stable 30, that is a regression in consistency. The first v39 run
(24 fps) was cold shader cache: switching drivers forces full recompilation.

### BOTW (Eden) on the same test spot

| Run | Avg FPS | p50 | p95 | Frames >33 ms | GPU busy |
|---|---|---|---|---|---|
| Turnip A22 26.3, run 1 | 29.1 | 33.33 | 50 | 7.2 % | ~62 % |
| Turnip A22 26.3, run 2 | 28.8 | 33.33 | 50 | 6.2 % | ~62 % |
| v39 Mesa 27, run 1 (cold cache) | 27.3 | 33.33 | 50 | 22.5 % | 62 % |
| v39 Mesa 27, run 2 (warm cache) | 27.1 | 33.33 | 50 | 24.7 % | 65 % |

Rendering was correct on v39 (no corruption). GPU busy is only 62-65 %, so BOTW
is CPU-bound in Eden: the gap is driver CPU overhead, not GPU work, and a warm
shader cache does not close it. v39 is ~7 % slower and stutters roughly three
times as often.

**Verdict: do not bump Mesa on this evidence.** Revisit when 27.0 is released
and the frame pacing can be re-measured; nothing else in v39 is A22-applicable.

## Test plan (historical)

1. Run `tools/probe/a22test` against `Turnip_v27.0.0-R2.zip`'s
   `libvulkan_freedreno.so` — does a Mesa 27 build even pass the gate on A22?
2. Import it in GameNative, load the same NMS scene, run the same 45 s capture,
   compare against our Turnip numbers (31.8 fps avg, p95 33.33 ms, 1.3 % slow).
3. Decide:
   * v27 clearly better (>5 % on p95 or slow-frame share) → pursue a Mesa bump,
     rebase the A22 identity patch, re-validate everything;
   * otherwise stay on 26.3.0 and revisit when 27.0 ships as a release.

No change to the shipped driver is justified by v39 alone.
