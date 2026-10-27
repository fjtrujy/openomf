# PSP GU follow-up: persistent CLUT caching and shadow memory pressure

**Date:** October 27, 2026
**Base:** `new_psp` at `658257b0`

## Summary

Further CPU-side palette optimization is **safe and measurable**, but physical
PSP frame timing shows the main obstacle to 60 FPS at 222 MHz is **GPU
execution/synchronization**, not CPU palette construction.

### Changes

1. Preserve computed 256-entry indexed-color CLUT variants across frames as
   long as the VGA palette, remap tables, and framebuffer effects are unchanged.
   Each command still stores its resolved CLUT slot before drawing.
2. Use 128 validated direct-mapped hints for CLUT variant lookup to avoid
   linear scans of up to 64 entries on common hits. A hint is verified against
   all key fields; hash collisions simply fall back to the ordinary search.
3. When the CLUT cache is full, replace only entries **not referenced by a
   command in the current frame**. If all 64 variants are required this frame,
   retain the exact indexed CPU fallback rather than corrupting a palette.
4. Under indexed texture-memory pressure, reclaim LRU shadow masks not used in
   the current frame. If needed, reclaim unused textures next. Shadow masks are
   derived data that can be rebuilt; a current-frame mask/texture is protected.
   This avoids replaying the entire screen in software merely because an old
   mask occupied the texture budget.
5. Under `PSP_GU_PROFILE` only, report 600-frame CLUT reuse/invalidation,
   evictions and microsecond-resolution frame preparation, GU command setup,
   GU execution wait and VBlank wait. Normal builds have **no profiling timers**.

The ordinary display remains **480×272 RGBA8888**, with 4:3 pillarbox
clipping, T8 indexed sprites and palette effects unchanged.

## Physical PSP measurements

These were 120-second native PSPLink/HostFS Arena 4 AI-vs-AI tests on the
connected PSP. Resource folders and builds were isolated, the original clocks
were restored on normal exit, and audio output errors were zero.

| Build | Clock | Mean FPS after 5s | Arena 4 mean | CPU effect frames |
| --- | --- | ---: | ---: | ---: |
| Previous optimization `658257b0` | 222/111 MHz | 49.75 | 47.64 | 0 |
| Previous optimization `658257b0` | 333/166 MHz | 58.17 | 58.55 | 0 |
| Persistent CLUT only (early trial) | 222/111 MHz | 50.62 | 50.71 | 10 |
| Persistent CLUT only (early trial) | 333/166 MHz | 57.49 | 57.53 | 19 |
| **Persistent CLUT + lookup hints + shadow reclamation** | **222/111 MHz** | **51.87** | **51.52** | **0** |
| **Persistent CLUT + lookup hints + shadow reclamation** | **333/166 MHz** | **58.72** | **59.06** | **0** |

**Caveat:** Each fight used independently randomized pilots, robots and AI.
Arena transitions differed between runs; samples are not statistically
paired. The numerical FPS increases are promising but should **not** be
presented as precise causal speedups. The 333 MHz setting remains the
recommended default to approach 60 FPS.

The early persistent-cache trial uncovered 19 full CPU-effect frames at
333 MHz due to the 4 MiB cache running out of **shadow-mask space**.
Explicit fallback logging identified the shadow-mask budget, *not* CLUT
saturation, as the cause. The final reclaim-unused-masks implementation
completed its separate physical 333 MHz trial with zero CPU effect frames.

### CLUT construction and GPU timing

The first native-PSP instrumentation measured about 7.2 CLUT variant
constructions per frame in the old per-frame cache. With persistent CLUT
variants, the actual hardware runs observed:

- About **0.76 CLUT constructions per frame** on the final 333 MHz run
  (vs ~7.2 before). The number fluctuates with color fades and damage.
- About **2.9 CLUT constructions per frame** on the final 222 MHz run
  (its randomized fight triggered more palette invalidations).
- Hundreds of thousands of CLUT hits were served from the cache during the
  two-minute matches.

The **physical PSP at 222 MHz** measured these typical 600-frame means:

| Stage | Mean observed |
| --- | ---: |
| Indexed palette/shadow/frame preparation | **0.74 ms** |
| PSP GU command construction | **0.63 ms** |
| **Waiting for GU execution (`sceGuSync`)** | **10.62 ms** |
| Waiting for next VBlank / swap | **5.01 ms** |

This measurement explains why CPU palette work is no longer the principal
limit and why PPSSPP's CPU-timing numbers do not predict physical hardware
FPS. The GU execution wait represents completion of submitted graphics
commands; it is not time spent doing ordinary game physics.

### Shadow-memory pressure validation

A separate **120-second PPSSPP stress test** compiled with a deliberately
reduced **3 MiB indexed-texture budget** (shipping default remains 4 MiB).
The renderer successfully evicted **15 obsolete shadow masks** late in the
fight. It completed with **zero CPU-composited frames, zero audio errors,
and a clean shutdown**. This exercises the newly added mask-reclamation path
without reducing the shipping texture budget.

The normal **4 MiB / RGBA8888** configuration also passed the full
120-second PPSSPP Arena 4 stress run: **59.3 FPS average** over 23 reporting
windows, **zero CPU-composited effect frames**, zero audio output errors and
a clean shutdown. A separate normal-intro launch verified the palette fades,
electrical flash and completed 2097 logo in actual PPSSPP GU window captures.

### Rejected experiment: RGB565 framebuffer

A separate physical 222 MHz run tested 16-bit `GU_PSM_5650` display buffers
instead of 32-bit `GU_PSM_8888` while retaining indexed T8 sprites.
It averaged **50.18 FPS** in Arena 4, with **10.54 ms** GU synchronization.
The comparable 32-bit run averaged **51.52 FPS in Arena 4** and **10.62 ms**
GU synchronization. As these are randomized fights, the results do not
establish a regression, but there is **no demonstrated speedup** and RGB565
reduces color precision. The experiment was **reverted**; 32-bit remains the
only shipping framebuffer format.

## Remaining optimization opportunity

**Follow-up completed:** A later benchmark verified PSPSDK-style advanced
sprite blitting on physical PSP. Narrow 32-texel strips on large textures
reduced the 222 MHz GU synchronization interval to ~2.9 ms, from the ~10.6 ms
reported here. See [ADVANCED_BLIT_BENCHMARK.md](ADVANCED_BLIT_BENCHMARK.md).

For further gains toward 60 FPS at 222 MHz, profile the **PSP GU command
stream and fragment work**, not just CPU palette loops. Candidate steps:
count redundant CLUT loads and texture-state changes, measure contiguous
same-state sprite runs that could be batched safely, and reduce unnecessary
overdraw if visual parity permits. Do not reorder translucent/overlapping
sprite draw commands without validating ordering. Record timings on the
physical PSP and compare matched encounters.

## Raw local logs

- `build/psp-persistent-clut-222/save/openomf.log`
- `build/psp-persistent-clut-333/save/openomf.log`
- `build/psp-clut-mask-opt222/save/openomf.log`
- `build/psp-clut-mask-opt333/save/openomf.log`
- `build/psp-rgb565-test222/save/openomf.log`
- `build/psp-shadow-pressure/evidence/game.log`
- `build/psp-stress-arena4/ppsspp-strict-fast-jit.log`
- `build/psp-clut-visual-evidence/intro-*.png`

All are ignored build artifacts. The upstream-integration checkout was left
untouched.
