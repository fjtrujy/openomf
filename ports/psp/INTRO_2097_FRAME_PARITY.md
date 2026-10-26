# OMF/2097 reveal: tick-synchronized macOS vs PPSSPP GU comparison

**Date:** October 26, 2026
**Reference branch:** `new_psp` at `0ce93b15`
**Result:** Original black silhouette fixed, **pixel/appearance parity remains incomplete**.

## Capture method

Both game clients were instrumented *temporarily* to hold the **already presented**
GPU frame for 1.6 seconds after rendering and log a scene tick. This deliberately
avoids the PSP `video_schedule_screenshot()` API, which forces exact indexed
CPU replay instead of displaying the normal GU frame.

- macOS: native `OpenGL3` with the VGA 256-color palette
  (`USE_EXTENDED_PALETTE=OFF`).
- PSP: `PSP_GU_APPROX_EFFECTS=ON`, native GU presented through
  PPSSPP 1.20.4, classic JIT/strict fast-memory configuration.
- Both use the same freeware game resources. Their window captures were
  cropped to their actual **4:3 display viewports**, then resized with
  nearest-neighbor sampling to **320×240 displayed pixels**. The game's
  underlying pixel grid is 320×200, but the intended 4:3 display uses
  non-square pixels. PSP uses a centered ~362×272 region of the 480×272
  screen, leaving pillarboxes; macOS uses a corresponding centered 4:3 area.
  This comparison is visual, not proof of pixel-perfect backend parity.
- Ten synchronized checkpoints were captured. The initial Mac holds reached
  even-numbered ticks and PPSSPP reached the next tick; a second native Mac
  run was performed so **both actually held the same scene ticks: 901, 1051,
  1101, 1141, 1181, 1221, 1261, 1301, 1361 and 1401**.
- The temporary frame-hold code was completely removed from the tracked
  sources after the capture. No renderer behavior was changed by this
  investigation.

## Quantified comparison

The count below is the number of pixels with **R, G and B all above 230** in
the corrected 320×240, **4:3-preserving** images; it describes bright-white coverage. It is not
an absolute photometric accuracy metric.

| Actual scene tick | macOS near-white px | PPSSPP near-white px | Observation |
| --- | ---: | ---: | --- |
| 1051 | 584 | 462 | Static OMF logo; broad appearance agrees |
| 1141 | 5,051 | 7,267 | PSP star has ~1.44× the white coverage |
| 1181 | 4,709 | 7,291 | PSP white area is ~1.55× the macOS reference |
| 1221 | 2,863 | 5,363 | PSP white area is ~1.87× the macOS reference |
| 1301 | 3,081 | 5,195 | PSP highlight remains ~1.69× the macOS reference |
| 1401 | 584 | 462 | Final 2097 digits and OMF logo broadly agree |

**Correction to the first analysis:** The previous image comparison incorrectly
used a 1280×800 (16:10) macOS crop against a 1408×1056 (4:3) PSP crop and
forced both to 320×200, stretching one image differently from the other.
Its near-white counts and apparent shape differences were therefore biased.
The corrected measurements above supersede those numbers. Independently
measured red OMF-logo pixel bounds have virtually identical aspect ratios:
**1.409 on macOS and 1.410 in PPSSPP**. No PSP viewport-aspect bug was found.

The **macOS burst has a smaller bright white core surrounded by
red/orange, textured electric fringes**. The existing PSP GU approximation
turns destination-index remap effects into a comparatively **large, almost
uniformly white star**, losing much of that colored detail. The original
completely **black star** from the previous GU implementation is no longer
present. The main lightning geometry, OMF logo, digits, and final reveal
sequence otherwise appear in both renderers.

The frame at tick 1401 also shows a small pale triangular fragment at the
lower-left corner of the PPSSPP game viewport that is absent from the
corresponding macOS capture; it could be a transient end-of-animation sprite
or clipping difference and has **not yet been diagnosed**.

## Evidence on the development Mac

- **Corrected 4:3 side-by-side comparison:** `build/desktop-frame-compare/comparison/macos-vs-ppsspp-true-4by3.png`
- Every original native window: `build/desktop-frame-compare/comparison/captures/mac-tick-N.png`
  and `build/desktop-frame-compare/comparison/psp-captures/psp-tick-N.png`
- Corrected 4:3 crops and difference maps:
  `build/desktop-frame-compare/comparison/{mac,psp}-4by3-N.png`,
  `diff-4by3-N.png`, `tick-aligned-gpu-comparison-4by3.png`
- Corrected numeric data: `build/desktop-frame-compare/comparison/metrics-corrected-aspect.csv`
- Diagnostic output: `build/desktop-frame-compare/comparison/state/openomf.log`
  and PPSSPP virtual Memory Stick game `OpenOMFFrameHoldCompare/save/openomf.log`.

All of these are ignored build/test artifacts, not assets added to the repo.

## Next possible refinement

If stricter visual parity is wanted, investigate a PSP GU-only CLUT
approximation that preserves more of the remap's per-source-index brightness
and red/orange fringe detail, or another practical hardware technique.
Do **not** unconditionally enable full-frame CPU composition to solve this:
a prior exact CPU path caused 52 slow frames on physical PSP during this
short intro. Any further tweak needs both tick-aligned visual comparison and
PPSSPP/PSP performance checks.

**Conclusion:** The earlier issue's original black-silhouette defect is
resolved in PPSSPP; the scene is **not yet visually equivalent** to macOS.
