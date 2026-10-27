# PSP GU large-texture blit benchmarking

**Date:** October 27, 2026
**Repository:** `new_psp`; original baseline commit `a04b6770`

## Question

Does `sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE)` wait mostly because
large VGA-indexed textures are transferred or sampled inefficiently, especially
backgrounds? Can PSPSDK's `advancedSpriteBlit` technique help?

## Findings

**Yes. Large-texture sampling locality is the major cause of the GPU wait in
these combat scenes, and slicing the sprites provides a dramatic improvement.**

The renderer caches each texture as an immutable T8 (indexed) surface in main
RAM, with a matching RGBA CLUT in system RAM. `sceGuTexImage()` binds its
memory address: it does **not** imply a new explicit CPU-to-VRAM copy every
frame. The GE reads texels through its small texture cache when it draws.
On a texture-cache miss, linear 2D textures can incur many separate memory
fetches. PSPSDK's `advancedSpriteBlit` draws large rectangles as narrow
horizontal **strips spanning the sprite's full height**, improving texture
cache locality.

The baseline physical PSP drawing code used one `GU_SPRITES` pair for each
large textured rectangle. In an observed 600-frame Arena 4 window it submitted
about 57,000 sprite commands, of which **~624 were large textures** and
**~671 were background-sized**. Most other draws were small sprites.
The renderer reused indexed textures from its CPU-side cache; reported
`new-textures` were a fraction of total draws, and most textures were not
rebuilt every frame. This identifies **sampling**, not per-frame CPU uploading,
as the more credible bottleneck.

The optimized GPU path keeps normal small sprites as one pair of vertices
each; for indexed textures at least 128x64, it divides the texture into
**32-texel-wide vertical strips** and submits all pairs using one
`sceGuDrawArray(GU_SPRITES, ...)`. Pixel scaling, flips, ordering, CLUT,
alpha, shadows, 4:3 clipping, audio and game logic are unchanged.
Full-resolution texture cache snapshots remain immutable.

## 120-second physical PSP A/B results at 222 MHz

Each test ran through PSPLink and HostFS on the connected PSP using the same
Arena 4 stress setup and resources. These are independently randomized AI
matches, **not deterministic frame-for-frame replay**, and changes in opponents
and arenas affect average FPS. The GPU wait reduction is larger and much more
consistent than the FPS variation.

| Texture path | FPS after warm-up | Arena 4 FPS | Avg. GU sync wait |
| --- | ---: | ---: | ---: |
| **Original linear texture, single quad** | 49.03 | 48.80 | 10.52 ms |
| **32-texel sliced blit (selected)** | **57.91** | **58.63** | **2.86 ms** |
| Swizzled T8 texture, single quad | 55.86 | 56.67 | 3.88 ms |
| Swizzled T8 + sliced blit | 56.41 | 56.50 | 1.66 ms |

All runs finished normally, with zero audio output errors and zero
CPU-composited frames. The 32-strip approach reduced the measured GPU wait
by approximately **73%** while raising Arena 4 FPS from ~49 to ~59 at 222 MHz.

### Physical PSP at 333 MHz

The selected sliced-blit configuration completed a separate 120-second
physical-PSP test at **333/166 MHz**: **59.21 FPS** averaged after warm-up,
**59.58 FPS** in Arena 4, and an average `sceGuSync()` wait of
**1.76 ms**, with zero CPU-composited frames and zero audio output errors.
This keeps the current 333 MHz default close to the 60 Hz display cap.

The combination lowered the **GPU wait** even further, but took additional
CPU work to rearrange textures and didn't demonstrate improved *overall FPS*.
Swizzling is **not enabled** in the final version to avoid extra complexity and
keep the more performant measured slice-only path.

Note that the `sceGuSync()` interval is a CPU-side blocking wait until GE
completion, not a pure GPU hardware timer. Its large reduction in otherwise
similar work is strong evidence of GE texture-cache locality issues.

## Test observations

- The new default standalone EBOOT passed a **120-second PPSSPP strict-JIT
  Arena 4 stress test** at **59.5 FPS average** over 23 reporting intervals,
  with zero native audio output errors, zero CPU-effect frames and a clean
  engine shutdown.
- Sliced blitting was visually checked with the *actual PPSSPP GU display*
  during the intro and the OMF/2097 reveal. There were no obvious seams or
  missing source pixels in the sampled screenshots.
- The PSP uses a centered 4:3 viewport; the optimized drawing remains inside
  the clipping region, with the `ASPECT: NATIVE` option unchanged.
- RGB8888 framebuffers, indexed T8 textures and alpha blending are unchanged.
- The default PSP CPU clock remains 333 MHz until stronger full-game evidence
  supports reducing it across all scenes.

## PSP/PSPSDK references

- PSPSDK GU speed sample: `advancedSpriteBlit` in
  https://github.com/pspdev/pspsdk/blob/master/src/samples/gu/speed/speed.c
- GE texture cache: https://www.ppsspp.org/docs/psp-hardware/gpu/texture-cache/
- GE optimization notes: https://www.ppsspp.org/docs/development/psp-internals/ge-performance/

## Evidence on the development Mac

These ignored build folders contain the physical PSP PRXs, their resources and
their game logs:

- `build/psp-blit-baseline-222/save/openomf.log`
- `build/psp-blit-sliced-222/save/openomf.log`
- `build/psp-blit-swizzle-222/save/openomf.log`
- `build/psp-blit-combo-222/save/openomf.log`
- `build/psp-blit-sliced-333/save/openomf.log`

PPSSPP visual captures:

- `build/psp-blit-visual/evidence/`
- `build/psp-swizzle-visual/evidence/`
- `build/psp-combo-visual/evidence/`

The `openomf-upstream-integration` checkout was not modified.
