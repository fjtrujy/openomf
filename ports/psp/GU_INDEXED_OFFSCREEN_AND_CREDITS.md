# PSP GU indexed offscreen capture and credits compositing

**Date:** October 27, 2026  
**Branch:** `new_psp`  
**Scope:** Remove additional full CPU sprite-replay paths from the native PSP GU renderer.

## What changed

The original indexed CPU compositor is still authoritative for complex destination-index
effects, but two common cases do not require per-sprite CPU rasterization:

1. **HAR/newsroom captures.** These captures only consume the base palette index
   (`indexed_pixel.index`) for a rectangular subsection of the scene. Shadows,
   dark-tint commands, additive sprites and destination remaps update separate
   framebuffer fields and therefore do not modify this base index. The PSP GU
   now renders only commands that write the base index into its existing
   320x200 index-coded RGBA scratch target. It reads back the red-byte indices
   to the requested `surface`. Commands are processed in original draw order,
   so later normal sprites correctly replace earlier pixels. A full CPU replay
   remains for the explicit software renderer, translucent (Bayer-dithered)
   sprite commands, oversized source textures, or CLUT allocation failures.

2. **Credits `SPRITE_INDEX_ADD` effects.** The credits' framebuffer option
   resolves each pixel as `clamp(index + add)` and ignores remap/dark fields.
   The GU scratch target stores the base index in the red byte and the
   saturated additive amount (`source_index * 60`) in the green byte.
   `sceGuPixelMask` makes additive sprites overwrite **only green**, while
   ordinary sprites overwrite both channels (clearing any earlier additive
   value). This matches the original assignment semantics: multiple additive
   layers replace one another rather than summing. The final 320x200 index
   pairs are read once, resolved and converted to colors on the CPU; the
   sprite-compositing loop is no longer replayed on the CPU.

Both paths reuse the spare 512x256 RGBA GU target originally introduced for
the indexed intro-prefix cache. The render list restores the current display
draw-buffer pointer and its full pixel write mask before returning. No
additional VRAM allocation, background index mirror, user-facing switch or
SDL dependency is introduced.

## Validation

**PPSSPP 120-second Arena 2 stress, strict fast JIT:** 23 reporting windows,
59.4 FPS average (55.5 minimum including loading). Its offscreen-capture
diagnostic compared three captures with the original full indexed CPU replay:

| Capture dimensions | Pixels compared | Mismatches |
| --- | ---: | ---: |
| 140x100 | 14,000 | 0 |
| 140x100 | 14,000 | 0 |
| 202x144 | 29,088 | 0 |
| **Total** | **57,088** | **0** |

**PPSSPP credits scene diagnostic:** The standard game engine was temporarily
configured to enter the credits scene in a local, ignored test build. GPU
credits output was compared against the full original CPU compositor at
frames 1080 and 1440: **64,000 pixels each, zero differences**. The test
ran for approximately 28 seconds, with 5-second reporting windows near
59.6-59.9 FPS. In two 600-frame reporting intervals, the renderer logged
zero full-frame CPU-effect frames. The temporary scene override was removed
from tracked sources after building the diagnostic EBOOT.

The release PSP build also compiles. The previous exact intro and combat
effect paths are untouched. These PPSSPP checks do not replace real PSP
performance tests; particularly, index readback and final-color conversion
might cost more on the physical 222 MHz CPU.

## Remaining CPU-side rendering

- **General shadows, destination remaps and dark tints:** GU approximations
  remain for large effects. Small bounded regions still use the exact CPU
  tile suffix, with existing fidelity/performance gates.
- **Credits:** The palette conversion after GPU index composition still runs
  on the CPU. This is 64,000 final-pixel lookups, not sprite rasterization.
- **Screenshots:** Exact screenshots currently require CPU replay.
- **Unsupported draws:** Bayer-dithered opacity, >512 source dimensions,
  CLUT saturation and shadow-mask budget exhaustion retain safe fallback
  paths. The explicitly selected `Software` backend remains available.
- **Actual hardware:** Measure at 222 and 333 MHz before drawing conclusions
  about speedups versus the previous software credits compositor.

The next work should prioritize eliminating full-frame fallback for supported
oversized textures and screenshot capture, then the destination-index effects
that fundamentally need an ordered index representation.
