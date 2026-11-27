# Turnip A22 26.3.1 (a22.4) - release notes

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

Package `turnip-a22-26.3.1.adpkg.zip`, sha256
`79c7aa7418fa8e7cfa9435522cfa6b246e3872f31e91f0de1db1110b504c668b`, on the
Retroid Pocket G2 (Android 15, kernel 6.1.115), 2026-11-27:

| Check | Result |
|---|---|
| Build: checksum, all four patches apply | clean |
| Static: `Mesa 26.3.1` / `Adreno (TM) 22`, only `HMI` exported, same 8 system deps | OK |
| Device gate (`scripts/device_test.sh`) | PASS - compute 64/64, clear+resolve 262144/262144 |
| AHB error interrupts, a22bench 4000-draw run | **0** (a22.3: 2,778) |
| a22bench 64×64 × 4000 draws | 1.188 ms/frame (a22.3: 1.185) |
| `dEQP-VK.renderpasses.*` (81,687 cases) | 53,450 Pass, 22 Fail, 28,215 NotSupported - **0 new failures** vs baseline |
| Play check: BOTW (Eden), GameNative title | renders and runs normally |

## Install

Import `turnip-a22-26.3.1.adpkg.zip` in the front-end's driver manager
(Eden: Settings → GPU → Install driver; GameNative/Winlator: driver
manager) and select it; it reports `Turnip Adreno (TM) 22`, driver version
26.3.1. Restart the emulator fully after switching drivers.

## Known issues (unchanged)

* Forcing GMEM (`TU_DEBUG=gmem`, `TU_AUTOTUNE_ALGO=prefer_gmem`) corrupts BOTW;
  the default autotuner renders correctly. See `docs/BUG-GMEM.md`.
* 22 `fragment_density_map ... static_subsampled_4_4` dEQP failures (rarely
  used feature).
