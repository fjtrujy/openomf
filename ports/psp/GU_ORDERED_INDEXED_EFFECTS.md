# PSP GU ordered indexed effects

**Date:** October 28, 2026
**Branch:** `new_psp`
**Scope:** Remove repeated CPU sprite reconstruction from bounded exact shadow, dark-tint, and destination-remap corrections.

## Architecture

The PSP GE is fixed-function: it can blend and mask RGBA render targets, but
cannot execute an arbitrary table lookup on the *previous destination palette
index* during an ordinary sprite draw. Earlier exact OpenOMF rendering therefore
reconstructed affected 16x16 tiles from sprite data on the CPU, using an
on-demand GPU-generated background prefix when available.

The new effect path uses the already-allocated 512x256 RGBA8888 index scratch
target as an **ordered three-channel indexed framebuffer**:

- **Red:** the base VGA palette index set by the most recent normal sprite.
- **Green:** the encoded destination remap requested by effect sprites.
- **Blue:** the destination dark-tint index.
- **Alpha:** a valid/opaque sample for the PSP GU's transparency blending.

Ordinary sprites write all three channels, resetting an earlier effect.
Destination remap draws overwrite only green. Dark tint writes green and blue.
Overlapping shadow commands use `GU_MAX` blending restricted to green, exactly
matching the original compositor's maximum-remap rule. The command stream
stays in its original submission order, so later ordinary sprites cover
previous effects as in the reference renderer.
The offscreen GE pass is scissored to the union of affected 16x16 tiles,
avoiding full-frame pixel fill when only a small region needs correction.

When a tile requires exact output, the renderer reads only the marked tile
pixels from GE EDRAM, resolves each encoded index/remap/dark triplet using the
original VGA palette/remap lookup rules, then uploads only the corrected
RGBA tiles for the final GU overlay. **There is no CPU sprite replay or
CPU reconstruction of the background/suffix on eligible frames.**
Final palette lookup, selective VRAM readback, and the per-frame effect-tile
coverage scan still use the CPU. This is intentionally not described as a
shader-only renderer.

### Exact effect source masks

PSP nearest-neighbor texture sampling can disagree with OpenOMF's integer-floor
sampling for heavily compressed shadow sprites. A direct GU shadow pass initially
produced hundreds of incorrect pixels in PPSSPP comparisons; changing texture
coordinate phases reduced but did not eliminate them.

The exact path now caches **destination-sized T8 masks**, populated once per
source GUID, target dimensions, flip, transparency, opacity and relevant Bayer
dither phase. Masks are generated using the same integer source-coordinate
mapping and original 4x4 opacity test as the software reference. The GPU then
draws each mask one-to-one in logical pixel coordinates. Masks are bounded by
a separate **1 MiB cache with 128 entries**, with LRU eviction only for entries
unused in the current frame; scene changes release the cache.

This is source preprocessing, not reconstruction of any destination pixels.
No mask is generated during the ordinary fast-GU path at 222 MHz.

### Performance and fallbacks

The normal 222 MHz combat path keeps its existing fast GU approximate shadows.
At 333 MHz, small eligible effect regions use the exact indexed pass instead
of CPU tile replay. The original OMF/2097 intro correction still accepts up
to 62 effect tiles, independent of CPU clock. The combat limit remains 16
tiles. Unbounded effects retain GU approximations.

The old exact CPU compositor remains a **correctness fallback** when the new
GPU path cannot support a command: additive-index effects mixed with remaps,
overly large destination masks, memory/cache exhaustion, or other unsupported
input combinations. `PSP_GU_APPROX_EFFECTS=OFF` and the explicitly selected
Software renderer continue to offer the original indexed CPU behavior.
The new route is not exposed as an additional user-facing build option.

## Verification

**PPSSPP 1.20.4, PSP GU Release builds:**

- Arena 4 shadow correction: sampled 16x16 corrected tile regions matched
  the full CPU reference, including the underlying index and remap channels.
- Forced 50%-opaque combat shadows with ordered Bayer masks also matched
  in sampled corrected frames.
- A synthetic small mixed dark-tint plus independent remap overlay matched
  the CPU reference in its sampled 1,536-pixel region.
- Original intro at 222 MHz: corrected frame 720 (10,112 pixels) and frame
  900 (13,056 pixels) matched the exact CPU reference with zero differences.
- The original GPU-indexed prototype *without* destination-aligned masks
  failed the same comparison; those incorrect experiments were not retained.

**120-second full-game combat regressions (PPSSPP strict fast JIT):**

| Build / arena | Reporting windows | Average FPS | Minimum FPS | Exact GPU effect work |
| --- | ---: | ---: | ---: | --- |
| 333 MHz configured, Arena 4 | 23 | 59.4 | 54.9 | Hundreds of bounded frames; no GPU-mask rejection in reported intervals |
| 333 MHz configured, Arena 2 | 23 | 59.4 | 55.6 | Hundreds of bounded frames; no GPU-mask rejection in reported intervals |
| `PSP_CLOCK_333=OFF`, Arena 4 | 23 | 59.29 | 53.10 | **Zero** combat GPU-indexed passes |

All three sustained runs exited cleanly. Each includes loading and
randomized AI combat, so the slight FPS difference is not evidence of a
causal regression. The 222-configured run verifies that the low-clock
combat performance policy introduces no additional indexed GPU passes,
not that the emulator reproduces physical 222 MHz timings.

PPSSPP's frame-rate pacing and emulated CPU clock are not a substitute for
timing on a physical PSP. **PSPLink USB currently refuses connections**, so
physical 222/333 MHz measurements could not be repeated for this milestone.
The conservative 222 MHz combat policy is retained for this reason.

## Remaining work

The bounded indexed pass removes CPU sprite replay where supported, but this
does **not** eliminate every CPU rendering operation or prove pixel-perfect
coverage for all effects:

- The CPU still performs effect-tile coverage scans, bounded index-to-color
  resolution, and occasional cached effect-mask generation.
- The original CPU compositor remains a safe fallback for unsupported or
  resource-constrained cases; removing it prematurely would lose correct
  graphics in those cases.
- The current exact region budgets deliberately leave large combat effect
  areas approximated. Broader GPU reconstruction requires real PSP timing
  results at 222 MHz before adjusting these limits.
- Full-game image parity should cover additional robots, fades, remap-table
  updates, wide and mixed effects, and normal scene transitions.
