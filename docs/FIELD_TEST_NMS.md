# Field test: No Man's Sky on GameNative

First real Windows-game test of the Turnip A22 package, against the stock
Qualcomm driver, on the same scene.

## Setup

| Item | Value |
|---|---|
| Device | Retroid Pocket G2, Snapdragon G2 Gen 2 (SG6275P), Adreno A22 |
| Front-end | GameNative (`app.gamenative`) |
| Game | No Man's Sky (Steam, Vulkan) |
| Render resolution | 720p (container) |
| Graphics preset | all low / off, no FSR |
| Scene | planet "Kohola XVIII", red biome, fixed position, no movement |
| Capture | `dumpsys SurfaceFlinger --latency` on the GameNative SurfaceView, 45 s per driver |

Drivers compared: stock Qualcomm (system) vs `turnip-a22-26.3.0`
(Adreno A22 profile, default autotuner, no debug properties).

An earlier stock sample was taken on a different biome and is excluded — the
scene must match for the comparison to mean anything.

## Results

| Metric | Stock | Turnip A22 |
|---|---|---|
| Average FPS | 29.1 | **31.8** |
| p50 frame | 33.33 ms | 33.33 ms |
| p95 frame | 50 ms | **33.33 ms** |
| Slow frames (>1.5x p50) | 103 / 1412 (7.3 %) | **20 / 1532 (1.3 %)** |
| GPU busy | 97 % | 96 % |
| GPU clock | 950 MHz | 950 MHz |
| Power (overlay) | 7.7 W | 6.7 W |
| GPU temp | 84.8 °C | 84.4 °C |

Rendering was visually correct on both; no corruption, no device loss, nothing
in the crash buffer.

## Interpretation

* The GPU is saturated in both cases (96–97 % at 950 MHz), so this scene is
  GPU-bound, not CPU- or translation-bound.
* **Frame pacing is the real result.** On Turnip, p95 equals p50: the game holds
  a 30 fps cadence with only 1.3 % of frames taking longer. Stock shows 50 ms
  frames 7.3 % of the time — visible hitches.
* Average FPS is ~9 % higher on Turnip and power draw is ~1 W lower at the same
  temperature. Same scene, same settings, so this is not a biome artefact.
* This is consistent with the driver-side effort being in the right place: no
  forced modes, no risky tuning, just a correct build.

## Caveats

* One scene, 45 s per driver, not blind: the driver switch order was
  Turnip → stock, so heat accumulated slightly (84.4 → 84.8 °C).
* 84–87 °C is close to where this handheld starts throttling; a 10–15 minute
  session would show whether the pacing holds.
* NMS is a native-Vulkan title; DXVK/VKD3D behaviour (Winlator-style) is not
  covered by this test.
* The previous forced-GMEM bug ([BUG-GMEM.md](BUG-GMEM.md)) did not trigger here
  with the default autotuner, which is further evidence that the default path is
  safe in practice.

## Reproduce

```bash
# GameNative: select driver, launch NMS, stand still at the same spot
adb shell dumpsys SurfaceFlinger --list | grep -i 'SurfaceView\[app.gamenative'
adb shell dumpsys SurfaceFlinger --latency '<layer>'
```

Parse presentation timestamps into frame intervals; report p50/p95 and the share
of frames above 1.5x p50, always alongside `clock_mhz` and `gpu_busy_percentage`.

## 2026-11-27: release vs Mesa 27-devel vs prefer_sysmem (GameNative, Proton 11)

Same spot, 1280×720, low settings, 45 s captures with
[`scripts/frametimes.py`](../scripts/frametimes.py) on GameNative's
`gndrm-scanout` layer. Driver identity from the kernel log: the release
raises AHB errors on the GMEM path, the Mesa 27 build (with patch 0004) none.

| Run | Wrapper | Driver | avg FPS | p50 | p95 | p99 | GPU busy | temp |
|---|---|---|---|---|---|---|---|---|
| A | v1 | release a22.3 | 15.71 (first run after load) | 66.67 | 66.67 | 216.7 | 98.2 % | 86.9 °C |
| C | v1 | Mesa 27-devel 79d58f09 + 0001-0004 | 19.75 | 50.0 | 66.67 | 83.3 | 96.0 % | 78.0 °C |
| A2 | v1 | release a22.3 | 21.73 | 50.0 | 66.67 | 83.3 | 95.1 % | 85.3 °C |
| W2-1 | v2 | release a22.3 | 19.10 | 50.0 | 66.67 | 83.3 | 98.5 % | 83.9 °C |
| W2-2 | v2 | Mesa 27-devel | 19.13 | 50.0 | 83.33 | 83.3 | 96.2 % | 85.4 °C |
| B' | v1 | Mesa 27-devel + `prefer_sysmem` | **10.26** | 100.0 | 116.7 | 166.7 | 97.9 % | 80.3 °C |

All at 949-950 MHz. Findings:

* **Mesa 27-devel = 26.3 release in this GPU-bound scene** (19.75 vs 21.73,
  19.13 vs 19.10). Mesa 27's measured gains are CPU-side (`a22cpu` submit
  ~22 % cheaper) and Vulkan 1.4.368, not GPU throughput here.
* **Wrapper v2 vs v1: no measurable difference.**
* **`prefer_sysmem` roughly halves the frame rate** (single run, far outside
  the noise): the autotuner's GMEM passes are the biggest performance factor
  in GPU-bound titles. Policies that send passes to sysmem up front are a
  loss here.
* Method: the first capture after loading is unreliable (shader compile,
  streaming, day/night/weather); warm up ~1 min and interleave drivers.
  Identical release runs spanned 15.7-21.7 FPS.
