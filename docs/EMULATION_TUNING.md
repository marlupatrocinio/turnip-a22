# Per-title tuning for emulation layers

How to turn a real game session into evidence for (or against) a driver
override, without guessing. Works on this device **without root**: Mesa reads
its options from Android properties when the environment variable is absent,
and the shell user is allowed to set `debug.mesa.*`.

## Why this works

`os_get_option()` on Android falls back to properties:

| Environment variable | Property |
|---|---|
| `TU_DEBUG` | `debug.mesa.tu.debug` |
| `TU_AUTOTUNE_ALGO` | `debug.mesa.tu.autotune.algo` |
| `TU_AUTOTUNE_FLAGS` | `debug.mesa.tu.autotune.flags` |
| `TU_GMEM` | `debug.mesa.tu.gmem` |
| `FD_DEV_FEATURES` | `debug.mesa.fd.dev.features` |

A front-end that sets the environment variable wins over the property. If a
value does not seem to apply, check `adb shell getprop debug.mesa.tu.autotune.algo`
and whether the emulator sets `TU_*` itself.

Verified on `MC94528AQF092401436` (production build, `adb root` refused):

```
$ adb shell setprop debug.mesa.tu.debug startup
$ adb shell setprop debug.mesa.tu.autotune.algo prefer_gmem
$ adb shell "LD_LIBRARY_PATH=/data/local/tmp a22bench libvulkan.so"
$ adb logcat -d | grep TU
I TU: TU_AUTOTUNE_ALGO=4 (prefer_gmem)
I TU: TU_AUTOTUNE: Algorithm: PREFER_GMEM, Mod Flags: 0x0
```

Reset after experiments:

```bash
adb shell "setprop debug.mesa.tu.debug ''; setprop debug.mesa.tu.autotune.algo ''"
```

## Procedure

### 1. Find the game's SurfaceFlinger layer

Start the title, then:

```bash
adb shell dumpsys SurfaceFlinger --list | grep -i <game or emulator>
```

Typical emulator layers contain the package name, e.g. `SurfaceView[...]`.

### 2. Capture a repeatable scene

The scene must be identical between runs: same save state / menu / in-game
benchmark. Drive it the same way each time (same inputs, same duration).

```bash
adb logcat -c
adb shell dumpsys SurfaceFlinger --latency "<layer>" > /tmp/frames.txt
```

`--latency` returns up to 128 presentation timestamps in nanoseconds; call it
repeatedly for longer captures. Skipped frames are marked with `INT64_MAX`.
Convert deltas to frame times and report p50 / p95 / jank, not averages.

### 3. A/B the autotuner

For each setting, force-stop the app, set the property, relaunch, replay the
scene, capture:

```bash
adb shell am force-stop <pkg>
adb shell setprop debug.mesa.tu.autotune.algo prefer_gmem   # or prefer_sysmem, or ''
adb shell am start -n <pkg>/<activity>
# ... replay scene, capture ...
```

Settings to compare, in order:

1. default (`bandwidth` autotuner, no property)
2. `prefer_sysmem`
3. `prefer_gmem`
4. optionally `TU_AUTOTUNE_FLAGS=preempt_optimize`

Only keep a change if it wins by more than run-to-run noise on **p95 frame
time**, not just average FPS. DVFS and thermals will otherwise fool you — see
[`BENCHMARKS.md`](BENCHMARKS.md) for a 13 % swing between identical runs.

### 4. Pass-level telemetry (optional, needs a throwaway build)

Turnip's autotuner logging is compiled out (`TU_AUTOTUNE_DEBUG_LOG_*` macros in
`tu_autotune.cc`). For a deeper look, build a **telemetry** driver with those
set to 1, install it in the front-end, replay the scene, and capture:

```bash
adb logcat -c
# replay scene
adb logcat -d | grep -E "autotune|TU " > autotune.log
```

The log records per-pass decisions (mode, draws, predicted costs). Aggregate by
engine/app to see whether the autotuner consistently picks the wrong mode for a
title's pass mix. This build is diagnostic only and is never released.

### 5. Bake and verify

If an override wins:

1. add it to `src/freedreno/vulkan/00-turnip-defaults.conf` under the matching
   `<engine engine_name_match=...>` (or `<application>`),
2. rebuild and repackage,
3. re-run the A/B with the property **unset** to confirm the baked rule
   reproduces the win.

## What not to do

* Do not tune against a menu or a loading screen; both are unrepresentative.
* Do not compare runs at different GPU clocks — record `clock_mhz` or at least
  keep sessions short and interleaved.
* Do not bake a rule for a single title into a global engine match without
  testing a second title on the same engine.
* Do not ship the telemetry build.
