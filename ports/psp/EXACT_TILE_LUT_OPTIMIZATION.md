# PSP exact indexed tile compositor — shadow-mask and remap LUT optimization

**Date:** October 27, 2026
**Branch:** `new_psp`
**Scope:** Reduce the CPU cost of exact intro remaps and small combat-shadow regions without changing regular GU rendering

## Changes

The PSP native renderer previously recomputed multiple operations for every
pixel corrected through its exact 16×16 tile compositor. Those operations
were redundant even though the source sprites are immutable and palette
remap tables rarely change.

The optimized implementation makes three changes:

1. **Reuse the native GU shadow mask in the exact CPU path.** GU already
   computes the original four-tap vertical coverage for each immutable T8
   shadow texture. Exact indexed replay now reads that coverage instead of
   re-reading and comparing four source pixels for each destination pixel.
   The cached mask is tied to its **source transparent index**; a different
   transparency value invalidates and rebuilds it. Full software replay
   retains the original four-tap algorithm when no suitable mask exists.
2. **Cache source-index translations in each CLUT slot.** Each existing
   palette-variant CLUT now also stores a 256-byte map from source palette
   indices to their final indexed values. These maps are **built lazily only
   when the exact tile suffix needs that CLUT**, so ordinary 222 MHz GU-only
   combat does not repeat index translation work. The cache shares its CLUT
   entry's identity, eviction, and remap invalidation rules; no stale
   translation survives a changed variant. The software reference path
   still calls the original `source_index` function.
3. **Generate resolved destination remap chains lazily.** Exact tiles
   resolve a destination `(remap_code, palette_index)` pair through a
   256×256 byte table. Rows are generated only when encountered and
   invalidated on VGA remap-table changes or scene changes. Pixels
   containing dark-tint or additive state, plus special credits rendering,
   continue through the original `cpu_resolve` routine.

A small per-context flag restricts the source-index lookup to the exact tile
replay, leaving screenshot capture, the full indexed software backend,
and unsupported-effect fallback behavior unchanged.

The additional tables occupy approximately **80 KiB of PSP main memory**
(64 KiB destination remaps, 16 KiB per-CLUT source translations, a few
hundred bytes of validity metadata). They do not consume extra VRAM and
do not add a second GU pass.

## Real PSP measurements (333 MHz)

All tests used the connected physical PSP with the release-mode Arena 4
120-second AI-versus-AI stress harness and the same resource set. Randomized
combat differs across runs, so timings are **per exact corrected frame**,
aggregated across reporting intervals. FPS differences are indicative,
not deterministic A/B measurements.

| Metric | Previous exact-tile renderer | Optimized exact-tile renderer |
| --- | ---: | ---: |
| Average combat FPS (after warm-up) | **58.00** | **58.22** |
| Frames corrected exactly | 2,171 | 2,440 |
| Total exact-effect preparation per corrected frame | 4.88 ms | **4.27 ms** |
| CPU sprite replay per corrected frame | 3.02 ms | **2.60 ms** |
| Audio output errors | 0 | 0 |

The exact-effect preparation cost improved by **approximately 12.5%**,
and sprite replay improved by **approximately 13.9%** under these independent
randomized trials. Most gameplay frames still wait for the 60 Hz refresh.

In repeated 600-frame reporting windows, the optimized compositor reused
the precomputed shadow coverage mask for hundreds of thousands of pixels
and generated only a handful of destination-remap lookup rows; the LUT
lookup volume is far higher than the row-build count.

### Final production-source PSP and emulator regression

After moving the optimized code out of the experimental snapshot, the
shipping-source renderer was rebuilt in all PSP configurations. A **separate
120-second physical PSP test at 333 MHz** completed with **58.45 FPS** after
warm-up, **2,380 exact corrected frames**, and **zero audio output errors**.
Its exact-effect preparation averaged **4.28 ms per corrected frame**,
consistent with the 4.27 ms isolated trial. The PPSSPP strict-fast-JIT
120-second stress test also completed normally at **59.37 FPS** with no
audio output errors.

As an additional low-clock safeguard, the source-index tables were moved
from ordinary CLUT creation to the first actual exact-tile use. In the
next randomized 222 MHz physical PSP run, the GU-only path averaged
**57.32 FPS** after warm-up, versus **57.94 FPS** in a previous run,
with zero exact combat-shadow frames and zero audio output errors.
Those separately randomized fights include scene-loading intervals, so
this difference alone does not establish a repeatable performance loss.

The final *lazy-translation* implementation was also tested with fresh
120-second runs from its actual production source:

| Final lazy implementation | Physical PSP 222 MHz | Physical PSP 333 MHz | PPSSPP |
| --- | ---: | ---: | ---: |
| Average FPS after warm-up | **57.32** | **58.23** | **59.60** |
| Exact shadow frames | 0 | 1,862 | 3,075 |
| Exact-effect preparation per corrected frame | N/A | **4.76 ms** | 1.80 ms |
| Native audio errors | 0 | 0 | 0 |

Against the previous non-LUT 333 MHz run's 4.88 ms of exact-effect
preparation per corrected frame, this independently randomized final run
improved by about **2.5%**; the earlier 12.5% improvement was measured in
the separately randomized original optimized experiment. Different shadow
counts, layer overlap, and scene transitions change the mean, so do not
interpret either figure as a deterministic per-frame speedup.

## Pixel-reference validation

Special isolated PPSSPP diagnostic builds compared **each corrected tile**
against an independently calculated full original indexed CPU compositor:

- Randomized Arena 4 combat: **20 sampled frames, 60,800 pixels, zero mismatches**.
- OMF/2097 electrical reveal: **4 sampled frames, 37,888 pixels, zero mismatches**.

After the lazy source-index translation refinement, a second PPSSPP combat
check compared **18 sampled frames, 59,392 corrected pixels**, again with
**zero mismatches**.
The lazy build's separate intro check compared another **4 sampled frames,
37,888 corrected pixels**, also with **zero mismatches**.

Both tests used the fast native GU path with the optimized exact tile
compositor. The diagnostic full-frame replay and timed exits were removed
from shipping sources. These checks establish sampled regional parity,
not a guarantee for every possible sprite, palette or scene.

## Wider-shadow experiment: rejected for now

The optimization made larger exact effects less expensive, so an isolated
real-PSP benchmark raised the exact combat-shadow budget from **16 to 24
logical 16×16 tiles**:

| 333 MHz variant | Average FPS | Exact corrected combat frames |
| --- | ---: | ---: |
| Optimized, 16-tile limit | **58.22** | 2,440 |
| Optimized, 24-tile limit | **57.16** | 5,375 |

The larger budget more than doubled the number of corrected frames, but
lost around 1 FPS across these randomized fights and had more pronounced
heavy-scene dips. **The shipping limit remains 16 tiles** to prioritize
smooth gameplay.

## Remaining work

- Large combat shadows and general mixed destination-index effects still
  use the fast GU approximation. CPU indexed sprite replay remains costly
  enough that a 222 MHz always-on exact path is not recommended.
- Exact dark tint, additive index effects and independent remaps still
  need a lower-overhead ordered GPU/CPU architecture; the current optimization
  does not silently enable them.
- Future profiling could evaluate direct tile-specific GPU index snapshots,
  source/destination command overlap bins and precomputed scaled sprite
  sampling spans. All proposals require pixel-reference verification and
  real PSP performance measurements before enabling.

## Ignored local evidence

- `build/psp-final-exact-combat-333/save/openomf.log` — prior 333 MHz release
- `build/psp-shadow-lut-investigation/optimized333/save/openomf.log` — optimized 16-tile trial
- `build/psp-shadow-lut-investigation/optimized24-333/save/openomf.log` — wider 24-tile trial
- `build/psp-shadow-lut-investigation/evidence/ppsspp-verify.log` — combat reference
- `build/psp-shadow-lut-investigation/evidence/intro-verify.log` — intro reference
- `build/psp-shadow-lut-investigation/evidence/lazy-verify.log` — final lazy combat reference
- `build/psp-shadow-lut-investigation/evidence/lazy-intro-verify.log` — final lazy intro reference
- `build/psp-final-indexed-lut-222/save/openomf.log` — final lazy source, physical 222 MHz
- `build/psp-final-indexed-lut-333/save/openomf.log` — final lazy source, physical 333 MHz

The separate upstream PSP-integration checkout and user-owned game assets
were not modified.
