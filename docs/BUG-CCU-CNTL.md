# Fixed: BV thread writes to render-backend registers raise AHB errors

**Status:** root cause found and fixed by
[`patches/0004-tu-a7xx-RB-static-regs-on-BR-only.patch`](../patches/0004-tu-a7xx-RB-static-regs-on-BR-only.patch)
(2026-11-26). Not the cause of [BUG-GMEM.md](BUG-GMEM.md).

## Root cause and fix

On A7XX the CP runs two threads, BR (rendering) and BV (binning).
`tu_emit_bin_preamble()` emits `tu6_init_static_regs()` for **both**, and that
function writes `RB_CCU_CNTL` and the device's `raw_magic_regs` - including
render-backend registers (`RB_RBP_CNTL` 0x8e01, `RB_DBG_ECO_CNTL` 0x8e04,
`RB_CCU_DBG_ECO_CNTL` 0x8e06, `RB_CCU_CNTL` 0x8e07, `RB_UNKNOWN_8E79`). BV
cannot reach the RB block, so its copies of these writes are rejected (AHB
bus error); BR's copies succeed, i.e. **the hardware always had the right
values**. The sibling `emit_rb_ccu_cntl()` call was already skipped for BV.
The CP reports only the first rejected write, which is why only 0x8e07
appeared until it was fixed, and then 0x8e01.

Patch 0004 wraps the `RB_CCU_CNTL` write and every magic register in
0x8e00-0x8eff in `CP_COND_REG_EXEC(THREAD_MODE, BR)`, so BR executes them and
BV skips them, whichever caller emits them. Mesa `main` (2026-11) still has
the unguarded code; StevenMXZ's Mesa 27 build happens not to trigger it.

Result (`a22bench` 64×64 × 4000 draws, counting suppressed kernel
callbacks): **~2,000 AHB error interrupts per run → 0**. `dEQP-VK.renderpasses.*`
unchanged (53,450 Pass, 22 Fail, 0 new). No measurable CPU or frame-time
change (`a22cpu`, `a22bench`, interleaved dEQP timing 231/235 s vs 240/231 s).
A stability fix: the CP no longer faults on every submit, and the kernel no
longer services hundreds of GPU error interrupts per second in games.

## Original report (2026-11-25)

## Symptom

Every Turnip process that submits GPU work produces kernel messages like this
(rate-limited, ~10 per burst):

```
kgsl kgsl-3d0: CP: AHB bus error, CP_RL_ERROR_DETAILS_0:0x10008e07 CP_RL_ERROR_DETAILS_1:0x12144
```

`CP_RL_ERROR_DETAILS_0` low bits = `0x8e07` = **`RB_CCU_CNTL`** (A7XX). The CP
reports an AHB error when a register write in the command stream is
rejected, so the value Turnip programs never reaches the register.

## Reproducer (no root)

```bash
export MSYS_NO_PATHCONV=1
adb shell "dmesg | grep -c 'AHB bus error'"
adb shell "cd /data/local/tmp && LD_LIBRARY_PATH=. ./a22test ./libvulkan_rel.so"
adb shell "dmesg | grep -c 'AHB bus error'"     # +10
```

`dmesg` is readable by the shell user on this build (SELinux permissive).

## Stock vs Turnip (same a22test workload)

| Driver | `AHB bus error` lines | `reset_count` |
|---|---|---|
| Stock Qualcomm (`/vendor/lib64/hw/vulkan.adreno.so`) | +0 | +1 |
| Turnip a22.3 | **+10** | +1 |

`reset_count` rises by one per GPU-submitting process with *both* drivers, so
it is not a hang indicator on this kernel; the AHB error is Turnip-only.

## Where Turnip writes it

`src/freedreno/vulkan/tu_cmd_buffer.cc`, `tu6_init_static_regs<A7XX>()`
(Mesa 26.3.0), emitted into the command stream:

```c
tu_cs_emit_regs(cs, RB_CCU_CNTL(A7XX,
   .gmem_fast_clear_disable = !info->props.has_gmem_fast_clear,
   .concurrent_resolve_mode = CONCURRENT_RESOLVE_MODE_2,
   .concurrent_unresolve_mode = CONCURRENT_UNRESOLVE_MODE_FULL));
```

Per-render-pass CCU layout (offsets, cache sizes) goes to the separate
`RB_CCU_CACHE_CNTL` on A7XX, which raises no error.

## Why this matters

With the write rejected, the GPU keeps whatever `RB_CCU_CNTL` value the kernel
or firmware programmed, while Turnip emits resolves and clears assuming
*concurrent resolve mode 2*, *full concurrent unresolve* and its own
fast-clear setting. A mismatch there would only matter in GMEM (tiled)
passes, which matches [BUG-GMEM.md](BUG-GMEM.md): forced GMEM corrupts, sysmem
is correct, and changing `num_ccu` turned corruption into a hang.

## Baseline that this does not (yet) break

`dEQP-VK.renderpasses.*`, default autotuner: 53,450 Pass, 28,215
NotSupported, 22 Fail (all `fragment_density_map ... static_subsampled_4_4`),
0 Crash, no GPU faults besides this AHB error. See
[VALIDATION.md §8](VALIDATION.md).

Forced GMEM (`TU_DEBUG=gmem`) gives the identical CTS result (0 new
failures in `dEQP-VK.renderpasses.*`), so the rejected write does not break
the GMEM paths the CTS covers. It remains a candidate for the BOTW
corruption, which needs larger, multi-bin passes to reproduce.

## Next experiments

1. Disable each `RB_CCU_CNTL` field Turnip relies on
   (`TU_DEBUG=noconcurrentresolves,noconcurrentunresolves`) and rerun
   forced-GMEM dEQP and BOTW: does the corruption go away?
2. Find which value the kernel programs for gen7_11_0 (KGSL non-context
   register list) and whether KGSL protects 0x8e07 on this part.
3. If the register is kernel-owned, make Turnip match the kernel's value
   instead of writing it (A22-only change in `tu6_init_static_regs`), and
   confirm the AHB errors disappear.
