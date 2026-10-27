# PSP on-demand GPU indexed-prefix cache

**Date:** October 27, 2026
**Branch:** `new_psp`
**Scope:** Exact indexed remap compositing during the OMF/2097 electrical reveal

## Motivation

The PSP GU can render T8 indexed sprites using CLUTs, but cannot perform an
arbitrary destination-palette-index remap while drawing into its RGBA display
buffers. Our prior intro correction replayed the original draw commands in
16×16 indexed tiles, with a cached CPU-rendered background before the first
destination-index effect. That background was identical for many consecutive
frames, but recomputing it after changes remained expensive on the PSP CPU.

The new implementation **builds that unchanged prefix on the PSP GPU**, then
retains the exact logical indices on the CPU until the prefix changes.

**This does not create a permanent second framebuffer or change combat
rendering.** Ordinary GU sprite drawing and large-texture advanced blitting
continue unchanged. The fast GU approximation remains for combat shadows,
dark tints and effect regions over the existing tile budget.

## Algorithm

1. While processing a frame with `FBUFOPT_BRIGHT_DESTINATION_REMAP`, detect
   at most 62 affected 16×16 logical tiles as before.
2. Identify the effect-free command prefix. Only fully opaque draws
   (`opacity == 255`) without a destination remap, shadow, dark tint or
   additive effect qualify for direct GU index rendering. Reject prefixes
   over 16 commands; retain the existing CPU prefix fallback.
3. Compare all prefix draw-command fields and immutable source texture GUIDs
   against the cached prefix. **On a hit, skip GPU work and VRAM reads.**
4. On a miss, encode each sprite's source palette index in the red channel
   of a dedicated RGBA CLUT. Render the prefix to a temporary 512×256 GE
   target at logical 320×200 coordinates; synchronize GU **once**, read the
   64,000 final indexed pixels from VRAM, and cache their logical indices
   as `indexed_pixel` values.
5. Restore the actual active PSP draw buffer obtained from
   `sceGuSwapBuffers()`; the following GU display render remains unchanged.
6. For each affected tile, copy its cached index values, replay only the
   destination-dependent suffix in the exact CPU compositor, and overlay
   the corrected tile through the ordinary GU display renderer.

The GPU target is only used for the *effect-free* prefix. Its contents
cannot be treated as authoritative once a destination-dependent effect is
applied. The existing exact tile budget and approximate fallback remain.

The cache is invalidated when the relevant remap tables change, when the
scene changes, or when any prefix command/texture identity changes.

## PSP VRAM footprint

Two PSP 8888 display buffers at 512×272 occupy 1,114,112 bytes. The
temporary encoded-index target is another 524,288 bytes (512×256×4), for
1,638,400 bytes total. This fits within the PSP's 2,097,152-byte GE EDRAM,
leaving 458,752 bytes. Indexed-prefix CLUT storage occupies at most 64 KiB
of main memory. The existing CPU indexed-prefix buffer is reused.

## Real PSP intro benchmark

Tests used a **35-second original intro** via PSPLink and the same
resources. Each run covered the 2097 reveal, was compiled in Release mode,
and exited cleanly.

| Intro statistic | CPU-prefix baseline, 222 MHz | GPU-cached prefix, 222 MHz | GPU-cached prefix, 333 MHz |
| --- | ---: | ---: | ---: |
| Exact corrected frames in reported interval | 159 | 172 | 230 |
| Prefix rebuilds | 5 | 5 | 5 |
| Prefix cache hits | 154 | 167 | 225 |
| Total prefix CPU reconstruction time | **425 ms** | **0.21 ms** (lookup only) | **0.24 ms** (lookup only) |
| GPU prefix rebuild cost | — | 12.76 ms per rebuild | 7.28 ms per rebuild |
| Of which GU wait | — | 2.99 ms | 1.59 ms |
| Of which VRAM readback and index caching | — | 9.54 ms | 5.54 ms |
| **Total exact-effect preparation per corrected frame** | **11.46 ms** | **9.02 ms** | **6.08 ms** |

GPU rebuild cost occurs only five times in the measured interval.
These results are **separate intro runs**, with differing numbers of
corrected frames and effects. They are measured workload comparisons,
not a strict deterministic speedup. The earlier optimized CPU-prefix
333 MHz result was approximately **7.25 ms per corrected frame**.

We also tested an earlier design that executed a GPU prefix pass on
**every** corrected frame. That required about 4.11 ms per corrected
frame at 222 MHz merely for GPU prefix rendering plus selective readback,
and was slower than the CPU-prefix cache. **It was not retained.**

## Fidelity checks

A separate PPSSPP Release build enabled a temporary diagnostic to compare
the corrected output of the GPU prefix + indexed tile replay to the full
original indexed CPU compositor. Across eight sampled intro effect frames
(game frames 810 through 1020), **every compared pixel matched**;
diagnostics recorded zero mismatches per frame. The first sampled
frame checked 10,112 pixels. This is a test of those effect regions, not
proof of pixel-exact rendering for all game effects.

The diagnostic reference checks and temporary timed auto-exit exist only
in ignored local test builds, not the shipping renderer.

## Combat regression

The optimized renderer completed a full 120-second **physical PSP at
222/111 MHz** AI combat regression in Arena 4, reaching **57.69 FPS**
after warm-up and exiting cleanly, with **zero audio output errors**.
The previous intro-exact version averaged **57.89 FPS** in a separate
randomized run. The GPU-prefix path was not used in combat
(**zero prefix rebuilds**), as intended. These matches are randomized,
so their 0.20 FPS difference should not be interpreted as a regression.

The normal packaged PSP implementation also passed a 120-second PPSSPP
strict-fast-JIT Arena 4 regression: **59.4 FPS** averaged across all 23
reporting windows (including loading), clean game shutdown and no audio
output errors.

## Remaining work

- Exact shadows, dark tint and general destination remaps in combat are
  **not enabled**. The GPU index target cannot perform arbitrary
  destination-dependent operations.
- The full 320×200 index snapshot still requires VRAM readback when its
  content changes; on a physical PSP this costs several milliseconds.
  Future improvements should test compact index snapshots, transfers
  with `sceGuCopyImage`, and narrower dirty regions with bounded sync.
- Additional fidelity tests should cover nonopaque prefix sprites,
  texture flips, scene transitions, overlapping remaps and palette changes.
- For combat, prefer *temporary region-scoped* GPU index passes only
  where their total render/synchronization/copy cost can beat existing
  CPU tile reconstruction. Continue to measure on physical PSP.

## Local evidence

- `build/psp-selective-prefix-probe/baseline222/save/openomf.log`
- `build/psp-selective-prefix-probe/cache222/save/openomf.log`
- `build/psp-selective-prefix-probe/cache333/save/openomf.log`
- `build/psp-selective-prefix-probe/stress222/save/openomf.log`
- `$HOME/.config/ppsspp/PSP/GAME/OpenOMFSelectiveGPUPrefixCacheVerify/save/openomf.log`

All diagnostic sources and binaries are in ignored `build/` folders.
The upstream PSP integration checkout was not modified.
