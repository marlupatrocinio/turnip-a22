# Turnip A22 26.3.1 (a22.4) - release notes (DRAFT)

Stability release for the Retroid Pocket G2 (Snapdragon G2 Gen 2, Adreno A22).
No new tuning; FPS is unchanged in the titles tested.

## What changed since a22.3

**New base: Mesa 26.3.1** (upstream bug-fix release, sha256 `4e4e438e…`,
pinned and verified by the build). Fixes that apply to this GPU include:

* ir3 (shader compiler): A7XX alias fixes - `don't emit alias.rt for
  relative const movs`, `check src flags of aliased collects`, `don't
  create aliases for array srcs`, `only fold fsat to (sat) on cat2/3 float
  instructions`; `count half registers when deciding to double the thread
  size`.
* tu/autotune: three render-pass history reference/reaping fixes (the
  autotuner runs on every render pass).
* tu: `Lower terminate to demote in fragment shaders`, `Disable LRZ for
  imported depth images`, advertise `VK_KHR_shader_maximal_reconvergence`.

**New patch 0004: no more GPU error interrupts.** On A7XX the binning thread
(BV) also received render-backend register writes it cannot perform; each
was rejected with `CP: AHB bus error`. The writes now execute on the
render thread (BR) only. The hardware always had the right values, so
rendering is unchanged.

| Measured on the G2 (26.3.0 + 0004 vs a22.3) | Before | After |
|---|---|---|
| AHB error interrupts, a22bench 4000-draw run | ~2,000 | 0 |
| AHB error interrupts, Sleeping Dogs DE benchmark | ~255/s | 0 |
| dEQP-VK.renderpasses.* (81,687 cases) | 53,450 Pass / 22 Fail | identical, 0 new failures |
| Sleeping Dogs DE benchmark (GameNative) | 52.05 fps | 52.80 fps |

## Validation of this exact package (26.3.1 + 0001-0004)

| Check | Status |
|---|---|
| Build: checksum, all four patches apply, warnings | done - clean |
| Static: identity `Mesa 26.3.1` / `Adreno (TM) 22`, only `HMI` exported, same 8 system deps | done |
| Device gate (`scripts/device_test.sh`) | **pending (device)** |
| AHB error count = 0 | **pending (device)** |
| `dEQP-VK.renderpasses.*` vs baseline | **pending (device)** |
| Short play check: BOTW (Eden), NMS / Sleeping Dogs (GameNative) | **pending (device)** |

Do not tag or publish until every row is done.

## Known issues (unchanged)

* Forcing GMEM (`TU_DEBUG=gmem`, `TU_AUTOTUNE_ALGO=prefer_gmem`) corrupts BOTW;
  the default autotuner renders correctly. See `docs/BUG-GMEM.md`.
* 22 `fragment_density_map ... static_subsampled_4_4` dEQP failures (rarely
  used feature).
