# Known issue: the hardware rejects Turnip's RB_CCU_CNTL write on Adreno A22

**Status:** confirmed on hardware 2026-11-25; root cause and impact under
investigation. Leading suspect for [BUG-GMEM.md](BUG-GMEM.md).

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
