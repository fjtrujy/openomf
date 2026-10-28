# PSP GU oversized T8 pages and framebuffer screenshots

**Date:** October 28, 2026
**Branch:** `new_psp`
**Scope:** Avoid further full-frame CPU sprite reconstruction while preserving the existing indexed fallback.

## Large source textures

The PSP GU cannot bind a single T8 texture wider or taller than 512 pixels. Previously,
drawing any such source forced an entire 320x200 frame through the software compositor.
The native renderer now retains the original linear indexed image for CPU reference,
and, where the 4 MiB texture budget permits, caches the source as multiple aligned
T8 pages up to 512x512 each.

- Pages contain immutable, padded indexed texels, with their own source offsets
  and actual dimensions. They are allocated and populated once per texture GUID,
  not reconstructed for each frame.
- The existing GU CLUT and source-remap variants are shared across pages.
- Each page maps to the appropriate section of the original scaled and optionally
  flipped destination rectangle, and large pages retain 32-texel sprite slicing.
- Intermediate page boundaries are aligned to the *destination pixel coverage*
  (`ceil(source_edge * destination_width / source_width)`), avoiding one-pixel
  seams when page edges fall between display pixels.
- All page allocations are tracked by the existing texture-memory budget and
  released together during LRU eviction or a scene change.
- Unsupported large shadows and insufficient tile-memory budget still fall
  back to the original indexed compositor. The direct CPU copy remains available
  to keep this fallback exact.

### Synthetic GPU validation

A temporary PSP-only PPSSPP diagnostic created a 768x640 indexed texture,
split into four GU pages and downscaled to 320x200. It compared a four-quadrant
pixel-reference pattern against the CPU source sampler, including the two
512-pixel source page seams.

| Source flip | Compared pixels | Seam / color mismatches | Missing pixels |
| --- | ---: | ---: | ---: |
| None | 64,000 | 0 | 0 |
| Horizontal | 64,000 | 0 | 0 |
| Vertical | 64,000 | 0 | 0 |
| Both | 64,000 | 0 | 0 |

The initial fractional-edge implementation had one 200-pixel column
mismatch in the unflipped and vertical-flipped cases; aligning page boundaries
to pixel coverage eliminated this error. A separate fine-grained stripe pattern
showed **3,200 differences out of 64,000 pixels** between ordinary GU
nearest-neighbor sampling and the CPU's integer-floor sampling *before*
the seam correction. This illustrates the pre-existing difference in sampling
rules; the quadrant test specifically validates page continuity rather than
pixel-exact arbitrary resampling. The diagnostic code was not retained in
shipping builds.

## Hardware screenshots

The former `screenshot != NULL` condition always forced full-frame software
composition, even on scenes already rendered entirely by the GU.

The native PSP GU renderer now captures the **finished display draw buffer**,
after `sceGuSync` and before swap, converting only the 480x272 ABGR pixel
array to RGB for the screenshot callback. It includes the exact displayed
aspect ratio, pillarboxes, camera position, and any existing GPU effect
approximations. Unlike the old 320x200 software capture, its output resolution
is **480x272**; this deliberately records the actual PSP display.

Explicitly selected `Software` mode still captures the original 320x200
CPU reference and remains pixel-exact. Other genuine compositor fallbacks
are unaffected, but screenshots no longer *cause* such a fallback.

A temporary PPSSPP screenshot diagnostic verified callback invocation,
480x272 dimensions, orientation flag and nonblack image data. A second test
scheduled the normal game screenshot callback, generated a 480x272 RGB PNG
successfully and visually verified the original opening animation with the
expected black 4:3 pillarboxes. The temporary test hooks were removed after
validation.

## Remaining limitations

- The PSP GE cannot implement arbitrary destination-index-dependent shadows,
  tint or palette remaps without a CPU correction step or approximate blending.
  The bounded exact 16x16 tile path therefore remains.
- `SPRITE_SHADOW` on tiled textures still selects the CPU path until the
  four-tap shadow masks are packed into matching GPU pages.
- If large texture pages cannot fit the 4 MiB cache, the code keeps the CPU
  fallback rather than dropping the draw. CPU opacity dithering and some other
  indexed effects similarly require their prior software semantics.
- GPU screenshot readback and 480x272 RGB packing still use the CPU, but
  **no CPU sprite rasterization is performed for the screenshot itself**.
- Performance on a physical PSP at both 222 and 333 MHz remains to be measured.
  The PPSSPP pixel tests establish emulator correctness, not full hardware
  timing or arbitrary scaling parity.
