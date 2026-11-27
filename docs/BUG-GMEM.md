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

## Conformance run with forced GMEM (2026-11-25)

`dEQP-VK.renderpasses.*` (81,687 cases, CTS 1.4.7.1) with `TU_DEBUG=gmem`
(verified active: `TU_DEBUG=0x1001` with `startup`), diffed against the
default-autotuner baseline ([VALIDATION.md §8](VALIDATION.md)):

| Run | Pass | NotSupported | Fail | Crash |
|---|---|---|---|---|
| default autotuner | 53,450 | 28,215 | 22 | 0 |
| `TU_DEBUG=gmem` | 53,450 | 28,215 | 22 | 0 |

**0 new failures, 0 fixed**; the 22 are the same fragment-density-map
`static_subsampled_4_4` cases in both. No GPU faults beyond the
`RB_CCU_CNTL` AHB error ([BUG-CCU-CNTL.md](BUG-CCU-CNTL.md)), which occurs in
both modes.

So tiled rendering is correct for everything the CTS render-pass suite
exercises - formats, load/store ops, input attachments, self-dependencies,
MSAA resolves, depth/stencil resolves, dynamic rendering. The BOTW
corruption needs something those tests do not combine; the CTS uses small
render areas that fit in very few bins, which is the most obvious gap on a
1.5 MiB part.

## Deferred-shading reproducer (2026-11-25)

[`tools/probe/a22gmem.c`](../tools/probe/a22gmem.c), driven by
[`scripts/gmem_repro.sh`](../scripts/gmem_repro.sh): one render pass, two
subpasses - a G-buffer pass (4 colour targets + depth/stencil, hundreds of
overlapping quads, depth LESS + stencil REPLACE) and a lighting pass reading
the targets as input attachments under a stencil mask. Every plane (4
colours, depth, stencil, final) is read back and compared byte-for-byte
between `TU_DEBUG=sysmem` and `TU_DEBUG=gmem`, in separate processes.

Controls: two sysmem runs are identical (deterministic); a different quad
layout (`GM_SEED=2`) differs on ~99 % of pixels (the compare detects
changes); the u_trace print (`MESA_GPU_TRACES=print`) shows
`tiledRender=true, forceRenderModeReason=TU_DEBUG(GMEM)` for the forced runs,
so tile memory really was used.

1920×1080, a22.3 driver:

| Colour formats | Depth | Draws | gmem vs sysmem |
|---|---|---|---|
| rgba16f, r11g11b10, rgba8, rgb10a2 | d32s8 | 300 | identical (7/7 planes) |
| 4× rgba8 | d32s8 | 300 | identical |
| 4× rgba16f | d32s8 | 300 | identical |
| 4× rgba32f (69 B/px, many bins) | d32s8 | 300 | identical |
| rgba16f, r11g11b10, rgba8, rgb10a2 | d24s8 | 300 | identical |
| rgba16f, r11g11b10, rgba8, rgb10a2 | d32s8 | 2000 | identical |

No KGSL faults beyond the `RB_CCU_CNTL` AHB message. The default autotuner
chose sysmem for this pass (`tiledRender=false`).

So multi-bin GMEM with deferred-style MRT + input attachments + D/S across
subpasses is correct.

### Multipass mode (`GM_MODE=multipass`)

Three render passes, the usual shape of an emulator frame:

1. **G** - the same G-buffer, `CLEAR` + `STORE` (GMEM: resolve to UBWC
   images in memory);
2. **L** - samples the 4 targets as textures (`texelFetch`, i.e. through the
   texture unit), `LOAD`s the stored depth/stencil for the stencil mask,
   writes the final image;
3. **P** - `LOAD_OP_LOAD` of colour 0, final and depth/stencil (GMEM:
   unresolve) over a partial render area, `GM_DRAWS/3` more quads
   depth-tested against the loaded depth.

Controls: u_trace shows all three passes `tiledRender=true` under
`TU_DEBUG=gmem`; disabling pass P (`GM_POST=0`) changes exactly colour 0,
depth, stencil and final, only inside its render area - so the
load/partial-area path is exercised.

Sweep: the six format/depth/draw cases above × {1920×1080, 1280×720} ×
pass-P area {unaligned default `(W/7, H/5+3, W/2+13, H/2+7)`, full frame,
tiny `1,1,37,23`} = **36 cases, all identical** (gmem vs sysmem and
autotune vs sysmem, 7/7 planes), all deterministic, no KGSL faults beyond
the `RB_CCU_CNTL` AHB message. Raw output: `results/gmem-repro-*.txt`
(local, not committed).

### Ruled out by the reproducer

* MRT G-buffers in RGBA8 / RGBA16F / R11G11B10 / RGB10A2 / RGBA32F, D32S8
  and D24S8, up to 69 B/px (many bins), 1080p and 720p, 300-2000 draws;
* input attachments across subpasses;
* GMEM resolve to memory followed by texture sampling in the next pass;
* `LOAD_OP_LOAD` (unresolve) of colour and depth/stencil - the path the
  rejected `RB_CCU_CNTL.concurrent_unresolve_mode` would affect;
* render areas that are partial, unaligned to bins, or tiny.

### Still untested (remaining suspects)

* **mutable-format images**: emulators create render targets with
  `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and render/sample through views of
  a different format (e.g. RGBA8 viewed as R32_UINT, or UNORM vs SRGB),
  which changes UBWC and GMEM resolve paths;
* MSAA attachments and resolves at full frame size;
* layered / array render targets, mip levels other than 0;
* secondary command buffers and dynamic rendering (Eden's own path);
* the actual BOTW frame: capturing its render-pass list with
  `MESA_GPU_TRACES` (`tiledRender`, draw counts, bandwidth per pass) under
  `prefer_gmem` would name the pass that corrupts.

## BOTW frame capture (Eden, default autotuner, 2026-11-25)

Captured with Mesa's u_trace through Android properties (no root):

```bash
P=dev.eden.eden_emulator.nightly
adb shell setprop debug.mesa.gpu.traces print
adb shell setprop debug.mesa.gpu.tracefile /sdcard/Android/data/$P/files/log/tu_auto.txt
# start Eden + BOTW at the test spot; the file grows ~10 MB/s (1.3 GB in 2 min)
adb shell "tail -n 400000 /sdcard/Android/data/$P/files/log/tu_auto.txt > /data/local/tmp/t.txt"
python scripts/tu_trace_summary.py t.txt --csv shapes.csv
```

Driver confirmed in-process from logcat (`TU_DEBUG=0x1`, autotune
`BANDWIDTH`). The 400k-line tail holds **27,306 render passes in 112
distinct shapes; only 164 (0.6 %) were tiled**, and every tiled one is a
single-bin pass (160×90 R16F, 192×192 RGB10A2, 64×64 R11G11B10+Z32).
**Every multi-bin pass runs in sysmem under the autotuner**, so forcing GMEM
moves exactly those passes onto a path BOTW never otherwise takes. No pass
uses MSAA.

Heaviest multi-bin shapes (all sysmem by default):

| Count | Size | Attachments | Load bytes/px | UBWC | Bins | Formats |
|---|---|---|---|---|---|---|
| 1030 | 1280×720 | 6 colour + depth | 26, **no clears** | colour **off**, depth on | 21 (192×256) | r11g11b10f, rg8, rgba8_srgb, rgba8, z32f |
| 246 | 1280×720 | 7 colour + depth | 30 | colour off, depth on | 21 (192×256) | + r32f |
| 477 | 1280×720 | 1 colour + depth | 8 | colour off | 6 (480×384) | r11g11b10f, z32f |
| 244 | 1280×720 | 1 colour | 4 | off | 3 (480×736) | rgb10a2 |
| 193 | 640×360 | 6 colour + depth | 23 | colour off | 6 (384×128) | r11g11b10f, r8, rgba8, z16 |

### Forced GMEM capture (same spot, `TU_DEBUG=gmem`)

All 112 shapes appear again, every pass tiled. GMEM stores out of the tile
buffer: almost all take the fast path; **1,499 take the slow
`unaligned=1, fast_path=0` path**, mostly `r11g11b10_float` (1,067) from the
tiny down-sample chain (8×8, 20×11, 40×22, …), plus RGBA16F (170) and
RGBA8 (120). Kernel log during both sessions: no GPU faults besides the
`RB_CCU_CNTL` AHB message; Eden's `GPU` thread hit
`page allocation failure: order:0` warnings inside zram/zsmalloc while
allocating GPU memory - system RAM pressure (7.5 GiB device), present with
the default autotuner too, so not GMEM-related.

### R11G11B10 clear-value rounding (found by the reproducer, not the BOTW bug)

At every size from 8×8 to 640×360, with all four targets R11G11B10,
colour target 2 differs between sysmem and gmem in ~22 % of pixels. Decoded:
only **bit 0 of the red channel**, only in pixels no quad covered, i.e. the
clear colour. Clear red 0.3 is stored as **0.29688 by the sysmem clear
(truncated, error 0.0031)** and 0.30078 by the GMEM clear (correctly rounded,
error 0.0008); the other targets' clear values (0.1, 0.2, 0.4) round the
same either way. A 1-ULP clear-conversion difference, within normal
conversion tolerance, and irrelevant to BOTW, whose passes never clear (all
`LOAD`). Tiny sizes and the unaligned store path otherwise match (RGBA16F,
RGBA8, mixed formats at 8×8 … 640×360).

### BOTW-shaped reproducer (`GM_MODE=botw`)

`a22gmem` rebuilds the main-pass shape from the trace: 1-8 colour targets
created `MUTABLE_FORMAT` (Turnip then disables UBWC on them), depth without
stencil, one CLEAR pass followed by `GM_REPEAT` passes that `LOAD` every
attachment and draw more depth-tested quads. Controls from u_trace under
`TU_DEBUG=gmem`:

* 6 colours + D32: `loadCPP=26, ubwc=nnnnnn|y, attachment_count=7` -
  BOTW's 1030-pass signature;
* 7 colours (+R32F) + D32: `loadCPP=30, ubwc=nnnnnnn|y`,
  **`numberOfBins=21, binWidth=192, binHeight=256`** - BOTW's exact bin
  layout;
* `GM_MUTABLE=0` turns UBWC back on (`yyyyyy|y`).

| Case | Size | gmem vs sysmem |
|---|---|---|
| r11g11b10, rg8, rgba8_srgb, 3× rgba8 + d32, 300 draws | 1280×720 | identical |
| + r32f (7 colours, BOTW bin layout) + d32 | 1280×720 | identical |
| 6 colours + d32, 1500 draws | 1280×720 | identical |
| 6 colours + d32s8 | 1280×720 | identical |
| 6 colours + d32, UBWC on (`GM_MUTABLE=0`) | 1280×720 | identical |
| r11g11b10, r8, 4× rgba8 + d16 | 640×360 | identical |
| r8 + d16 (shadow-map-like) | 720×720 | identical |
| 6 colours + d32 | 1920×1080 | identical |

All deterministic, autotune also identical, no KGSL faults. **BOTW's
render-pass configuration - attachment count and formats, UBWC state,
load/store ops, depth format, bin layout - renders correctly in GMEM.**
The corruption must come from what the draws inside those passes do, which
the reproducer's simple quads do not: geometry shaders, `discard`,
blending, colour write masks, depth bias, in-pass
`vkCmdClearAttachments` (BOTW issues ~1,500 `sysmem_clear_all` per trace
window), or reads of an image through a different-format view.

### Draw-level capture (default autotuner)

`debug.mesa.tu.gpu.tracepoint=+draw` adds per-draw records with every
stage's shader SHA-1 (`scripts/tu_trace_summary.py` reports them per
multi-bin shape). 700k-line tail, 20 s at the test spot:

* **186,304 draws, all `vs+fs`** - no geometry or tessellation shaders
  anywhere;
* the main 1280×720 7-attachment pass: 724 passes, 123,706 draws,
  **105 distinct pipelines**; the 8-attachment variant 6 pipelines;
* **in-pass `vkCmdClearAttachments` inside multi-bin passes**: 1280×720
  R11G11B10+Z32 (6 bins, colour clear), 1280×720 1-attachment (3 bins,
  colour), 720×720 depth-only (3 bins, depth), one rect each.

Reproducer `GM_INCLEAR=color|depth|both` (botw mode) clears one unaligned
rectangle halfway through every LOAD pass; controls: the clear changes
exactly its rectangle, u_trace shows BOTW's 6-bin 480×384 layout and
per-bin `gmem_clear`. 16 cases (colour/depth/both × r11g11b10+d32,
the 6-colour main pass, rgb10a2+d16, r11g11b10+d32s8 at 1280×720; depth and
both × r8+d16/d32 at 720×720): **all identical**.

### Ruled out so far

Pass configuration (attachments, formats, UBWC state, load/store, depth
format, bin layout), multi-bin rendering, input attachments, texture
sampling after a GMEM resolve, unresolve (`LOAD`), partial/unaligned render
areas and stores, in-pass clears, geometry/tessellation shaders, MSAA (not
used), LRZ and UBWC toggles (earlier `nolrz`/`noubwc` runs).

### Remaining suspects

What BOTW's 105 main-pass pipelines do per draw: blending, `discard`,
colour write masks, depth bias, and above all **attachment feedback loops**
- a shader sampling an image that is also bound as an attachment of the
same pass. In sysmem that reads memory the pass is writing (roughly
coherent); in GMEM the attachment lives in tile memory and the sampled copy
in memory is stale, which would turn lit surfaces into garbage exactly as
seen. Turnip handles declared loops (`VK_EXT_attachment_feedback_loop_layout`
sets `disable_gmem`, which even `TU_DEBUG=gmem` honours), but the forced-GMEM
trace shows every pass tiled with reason `TU_DEBUG(GMEM)`, so no pass was
declared as a loop.

### Bisect in-game by pass shape (diagnostic driver)

[`patches/diag/0100-tu-gmem-shape-bisect-filter.patch`](../patches/diag/0100-tu-gmem-shape-bisect-filter.patch)
adds `TU_GMEM_SHAPE` (Android: `debug.mesa.tu.gmem.shape`, read once per
process) to `use_sysmem_rendering()`: when set, passes whose framebuffer
matches go GMEM (after the hard constraints), all others go sysmem.
Syntax: comma-separated `WxHxA` (width, height, framebuffer attachment count
incl. depth; `*` wildcards) or `multibin` (any pass with more than one bin).
The u_trace reason shows `TU_GMEM_SHAPE match` / `no match`.

Never released. Built in its own tree and output folder:

```bash
cp -r src/mesa-26.3.0 src/mesa-26.3.0-diag
MESA_SRC=$PWD/src/mesa-26.3.0-diag WORK=$PWD/build/diag DIST=$PWD/dist-diag \
EXTRA_PATCHES=$PWD/patches/diag PKG_PREFIX=turnip-a22-gmembisect \
PKG_NAME="Turnip A22 GMEM bisect (diagnostic)" bash scripts/build_turnip_g2gen2.sh
```

Validated with `a22gmem` (1920×1080, 6 attachments): unset → autotuner;
`1920x1080x6`, `multibin`, `*x*x6`, `1280x720x7,1920x*x*` → GMEM;
`1280x720x7` → sysmem. Passes the device gate.

Bisect order: `multibin` first (must reproduce the corruption, or the
filter does not cover the culprit), then the multi-bin shapes from the
trace - `1280x720x7` (main pass), `1280x720x8`, `1280x720x2`, `1280x720x1`,
`640x360x7`, `640x360x2`, `640x360x3`, `720x720x1`.

Results (2026-11-25, Eden + BOTW at the test spot, everything else sysmem):

| Step | `debug.mesa.tu.gmem.shape` | Result |
|---|---|---|
| 1 | `multibin` | **corrupted** - culprit is a multi-bin pass |
| 2 | `1280x720x7,1280x720x8` | **corrupted** - one of the main G-buffer passes |
| 3 | `1280x720x7` | **corrupted** - the 6-colour + D32 main pass alone suffices |

### Feedback-loop detector (diagnostic v2)

The same patch adds `TU_FEEDBACK_CHECK` (`debug.mesa.tu.feedback.check=1`):
image descriptors written through `vkUpdateDescriptorSets`, update templates
and push descriptors are recorded per set (offset → image); at every draw
inside a render pass the bound graphics sets are compared with the pass's
attachments and each distinct (image, attachment) hit is logged once to
logcat (`TU_FEEDBACK hit #n: fb WxH att_count A attachment i format F ...`),
plus running totals. Controls: `a22gmem` subpass mode (input attachments)
→ exactly its 4 colour attachments flagged; botw mode (no descriptors) → no
hits.

BOTW: **262,144+ hits, 14 distinct pairs - undeclared feedback loops**:

| Pass | Attachments sampled while bound (VkFormat) |
|---|---|
| 1280×720, 8 attachments (main pass variant) | 0 (16 RG8), 1-2 (57 A8B8G8R8_SRGB), 3-4 (51 A8B8G8R8_UNORM), 5 (122 B10G11R11), 6 (100 R32F) |
| 1280×720, 2 attachments (R11G11B10 + Z32) | 1 (126 D32F - its depth) |
| 640×360, 7 attachments | 5 (51) |
| single-bin passes 512², 1024², 256², 64², 160×90 | their colour target |

**No hit in the 1280×720 7-attachment pass** that step 3 isolated. Open
questions: does `1280x720x8` alone (the pass that samples its own G-buffer)
also corrupt in GMEM, and does the x7 pass corrupt through its interaction
with the x8 pass, which renders into and samples the same images? Untested
gaps in the detector: descriptor copies (`vkCopyDescriptorSets`) and sets
with more than 64 image descriptors.

## What we still do not know

* Whether the corrupting pass is a specific attachment format, MSAA, or a
  render-pass size threshold — the autotuner's safe choice masks it. CTS
  render-pass tests (all formats, MSAA, resolves) pass under forced GMEM, so
  the size / bin-count / attachment-count combination is now the main
  suspect.
* Whether real A735 hardware is affected (same `chip_id`, same profile); if so,
  this is an upstream descriptor/CCU-layout bug rather than an A22 quirk.
