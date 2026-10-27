# PSP performance investigation — CPU workload versus clock speed

**Date:** October 27, 2026
**Repository:** `fjtrujy/openomf`, `new_psp` based on `40712334`
**Scope:** Native PSP GU, game simulation and native `sceAudio` mixing; Arena 4 randomized AI-versus-AI stress test.

## Goal

Determine which operations cost the most CPU time and why the PSP port uses
the supported 333 MHz clock when the original DOS game was designed for a
33 MHz-era 386. Processor clock frequencies are **not** directly comparable:
the CPU instruction sets, graphics paths, libraries, hardware devices and
rendering rates differ considerably.

## Method and limits

- Temporary compile-time diagnostics using
  `sceKernelGetSystemTimeWide()` measured stages of the game loop, PSP GU
  submission/synchronization and the audio worker. They were **removed**
  from the source after collecting this report.
- PPSSPP classic JIT with strict fast-memory configuration; two builds
  selected `PSP_CLOCK_333=ON` (333 MHz / 166 MHz) and
  `PSP_CLOCK_333=OFF` (nominal default 222 MHz / 111 MHz). Each ran
  Arena 4 for **40 seconds**; measurements below exclude the first five
  seconds of loading. A third build forced indexed CPU software
  compositing at 333 MHz.
- Counter overhead and randomized AI combat introduce some variation.
  Values are **emulator-observed elapsed times**, not PSP CPU cycle counts
  or reliable measurements of the real PSP hardware's remaining headroom.
  PPSSPP may have dramatically different timing characteristics from the
  actual GU, CPU memory cache and audio system.
- The application is VBlank-capped at 60 Hz. **Waiting for VBlank does not
  mean the CPU is doing expensive work.**

## Breakdown by stage

Times are **average microseconds per presented frame** except for audio
(microseconds **per 512-sample audio block**). `Render prep` includes GU
palette variant generation, shadow preparation and any indexed CPU fallback.
`GU commands` includes setting up and submitting draw commands, and in the
software fallback it also includes CPU conversion of the full indexed frame
to RGBA before the final blit. `Frame
draw` includes the scene's sprite generation and renderer draw callbacks
before GPU submission.

| Stage | 333 MHz native GU | 222 MHz native GU | 333 MHz CPU software |
| --- | ---: | ---: | ---: |
| Game simulation, AI and movement | 340 µs | 600 µs | 484 µs |
| Scene draw / sprite callbacks | 337 µs | 525 µs | 290 µs |
| GU frame preparation / indexed replay | 482 µs | 639 µs | **14,375 µs** |
| GU command setup / submission | 380 µs | 642 µs | **8,966 µs** |
| GU synchronization | 13 µs | 19 µs | 17 µs |
| VBlank / swap waiting | 15,136 µs | 14,258 µs | 9,343 µs |
| Native audio PCM mix, separate thread, per 512 samples | 774 µs | 1,166 µs | 769 µs |
| Average game FPS after warm-up | 59.83 | 59.83 | **29.90** |

The main-thread non-waiting work in a normal 333 MHz frame is about
**1.54 ms** (simulation + sprite callbacks + GU prep + commands).
The comparable 222 MHz value is about **2.41 ms**. These are approximate
breakdowns, not an exact accounting of all CPU activity: audio runs
concurrently on a separate thread, and GU hardware may run asynchronously.

At 333 MHz the audio mixer averaged **0.77 ms per 512-sample block**
(11.61 ms of audio); at 222 MHz the measured value was about **1.17 ms**.
The PCM thread calls libxmp to decode 44.1 kHz stereo music and mixes
three effect voices.

## Native renderer internals

Separate 333 MHz profiling of about 2,400 combat frames counted:

| Counter | Observed average |
| --- | ---: |
| Indexed sprite/texture lookups | **122 per frame** |
| Cache probes per lookup | ~42 |
| Texture cache misses/new textures | **0.14 per frame** |
| Texture cache evictions | **0.03 per frame** |
| New texture uploads | **~1.03 KiB per frame** |
| New 256-color CLUT variants constructed | **7.2 per frame** |
| CLUT variant cache hits within a frame | **237 per frame** |

Consequently, large continuous texture uploads or persistent cache thrashing
are **not the dominant steady-state problem** in this scene. The renderer
does repeatedly walk a 256-entry texture cache, construct several
256-color CLUT variants per frame and issue roughly 100–200 sprite commands
per frame. These are credible optimization targets, but microbenchmarking
individual routines on **physical PSP** is required before ranking them
precisely by CPU cycles.

## Main expensive mode: exact CPU compositor

When PSP software rendering is forced, `GPU=0`, `CPU-effects` advances
on every frame, and the GU stage spends about **14.4 ms** replaying indexed
drawing plus another **9.0 ms** converting the full frame and preparing the
final RGBA blit. This results
in **~30 FPS**, even when the clock setting is 333 MHz. The ordinary
GU configuration spends less than **1 ms** on the equivalent two stages.

Avoid full-screen CPU fallback for common palette remaps and shadows; retain
it only for correctness/diagnostic cases. The indexed CLUT GU path is the
most important optimization already present in this PSP port.

## Additional observations

- Scene initialization occasionally produces **hundreds of milliseconds**
  of asset/load/setup cost, most visibly in the first few seconds of an
  arena. This is a loading spike rather than sustained combat overhead.
- In these measured fights, game simulation (AI, collisions, movement) is
  a **small part** of the frame cost. This does not exclude specific arenas,
  mods or many-object effects having different behavior.
- On PPSSPP the 222 MHz build was also VBlank-capped at ~60 FPS, so this
  experiment **does not establish that real PSP needs 333 MHz**. Prior
  physical-PSP validations were made at 333 MHz (~56–58 FPS). A paired
  **physical PSP 222/333 MHz benchmark** (same ROM assets, same scene and
  power conditions) is needed to confirm the minimum real-hardware clock.
  This diagnostic did not interrupt the existing PSPLink game session.

## Practical optimization priorities

1. **Measure on real PSP** at both clocks, ideally with aggregate per-stage
   counters rather than per-frame logs. PPSSPP numbers alone cannot
   establish a physical hardware bottleneck.
2. **Native GU CPU overhead:** reuse unchanged CLUT variants or avoid
   unnecessary rebuilds; optimize linear 256-entry texture-cache lookup;
   reduce redundant texture/CLUT state changes where sprite ordering
   allows. Compare FPS and CPU times, especially in busy scenes.
3. **Audio:** investigate libxmp decoding/resampler effort using separate
   worker-thread counters. Avoid reducing audio fidelity by default now
   that the triple-buffered output sounds clean.
4. **Scene loading spikes:** isolate file I/O, asset parsing and texture
   warmup separately; these are not the steady-state 60-FPS limiter.
5. Do **not** route general combat through the exact CPU compositing
   path: its cost is overwhelmingly higher than native GU.

## Raw local evidence (ignored build outputs)

**Physical PSP follow-up:** The initial breakdown above used PPSSPP. Actual
PSPLink profiling subsequently measured approximately **10.6 ms/frame** in
`sceGuSync` at 222 MHz, plus less than **1.5 ms/frame** of GU CPU
preparation/command submission. See
[GU_PERSISTENT_CLUT_PROFILE.md](GU_PERSISTENT_CLUT_PROFILE.md) for real
hardware timings and the latest palette-cache results.
See also [ADVANCED_BLIT_BENCHMARK.md](ADVANCED_BLIT_BENCHMARK.md):
splitting large sprite quads into texture-cache-friendly strips subsequently
reduced the actual 222 MHz `sceGuSync` wait to around 2.9 ms.

- `build/psp-frame-profile-333/evidence/game.log`
- `build/psp-frame-profile-222/evidence/game.log`
- `build/psp-frame-profile-software/evidence/game.log`
- `build/psp-frame-profile-333/evidence/cache-game.log`

These builds had additional one-off diagnostics and a 40-second timeout.
**No shipping settings or input/audio/rendering behavior should be changed
based on the diagnostic instrumentation alone.**
