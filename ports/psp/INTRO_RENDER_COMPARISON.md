# Intro lightning: macOS and PSP renderer comparison

Date: October 26, 2026. Branch: `new_psp`.

## Reference setup

- macOS: native OpenOMF `OpenGL3` renderer, Apple M1 Max, OpenGL 4.1 Metal; built with `MIN_LIB_BUILD=ON` and `USE_EXTENDED_PALETTE=OFF` to use the PSP's 256-color VGA palette.
- PSP: native indexed PSP GU renderer in PPSSPP strict classic JIT. Both the normal `PSP_GU_APPROX_EFFECTS=ON` build and the `OFF` exact-effects variant were exercised. The previously installed real-PSP EBOOT was not replaced.
- Both used the same packaged `INTRO.BK` and related freeware assets. The desktop build was enabled by excluding the PSP-only `psp_joystick.c` source from desktop CMake's recursive source glob.

## What was observed

The lightning animation was captured directly from the macOS window and the PPSSPP window, then compared more carefully using **14 matching intro scene ticks**, from 15 through 210 in increments of 15. These diagnostic hooks were temporary and have been removed from the shipping source.

| Scene tick | macOS OpenGL3 | PSP exact indexed snapshot |
| --- | --- | --- |
| 15 | Dark grey flash, dim blue-edged bolt; background RGB `(75,75,75)` | The same dark bolt and background RGB `(75,75,75)` |
| 30 | Bright white/cyan-edged bolt on near-white background, RGB `(247,247,247)` at the sampled background pixel | Same recognizable bolt and cyan edges; background RGB `(234,234,234)` at the sampled pixel; title text appears at a slightly different animation phase |
| 60 | `EPIC MEGAGAMES PRESENTS` on black | Same title and layout (at different output resolution) |
| 150 | Same static title on black | Same title and layout |

The native macOS screenshot is 640x400, with a 4:3 logical viewport; the PSP indexed screenshot is 320x200. Do not compare the PNGs byte-for-byte without applying equivalent viewport cropping and scaling. Short-lived flash colors and the title's visibility can also change between neighboring frames, so wall-clock screenshot pairs alone are not reliable.

**Special effect trace:** 17 PSP GU frames sampled between renderer frame numbers 136 and 408 all had **zero** `SPRITE_SHADOW`, `SPRITE_DARK_TINT`, remap-round, and `SPRITE_INDEX_ADD` draw commands. The intro lightning is drawn with normal indexed sprites and VGA palette changes; it does not exercise the PSP's approximate shadow/destination-remap algorithms in the sampled interval. Switching to the exact-effects build did not visibly alter the corresponding lightning window captures.

The slightly different peak-flash brightness and title transition need better GPU-presented, tick-synchronized capture before treating them as a renderer defect. The native and PSP screenshot callbacks use different capture paths; in particular, a screenshot request forces the PSP into its CPU replay path for that frame. Those captured PSP PNGs validate its indexed reference output, **not** its GPU approximation. Independently captured PPSSPP windows showed the on-screen GPU rendering.

## Artifacts on the development Mac (not committed)

- Side-by-side tick 15/tick 30 comparison (PSP image enlarged by 2x with nearest-neighbor filtering): `build/desktop-min/intro-captures/intro-macos-vs-psp.png`
- macOS tick-aligned PNGs: `build/desktop-min/test-home/state/screenshot/screenshot_intro_tick_XXXX.png`
- PSP indexed tick-aligned PNGs copied from PPSSPP: `build/desktop-min/intro-captures/exact-ticks/screenshot_intro_tick_XXXX.png`
- macOS window captures: `build/desktop-min/intro-captures/mac-*.png`
- PSP regular-GU window captures: `build/desktop-min/intro-captures/ppsspp/psp-*.png`
- PSP exact-effects window captures: `build/desktop-min/intro-captures/psp-exact/exact-*.png`
- Intro GU diagnostic build logs: `build/psp-intro-debug/` and PPSSPP virtual Memory Stick `PSP/GAME/OpenOMFIntroDebug/save/openomf.log`.

## Conclusion

The intro's thunderbolts substantially match between macOS and the PSP's indexed reference, with minor phase/brightness differences in the bright flash. No defect in the PSP's *shadow* approximation has been demonstrated by this scene, because those special shadow effects were not used during the sampled lightning animation. No rendering behavior was changed. Actual arena shadows and destination-index remap effects can be evaluated separately against macOS, ideally with deterministic per-frame GPU captures.

## Unresolved: later OMF logo and 2097 lightning reveal

The initial comparison above only covers the **early EPIC MEGAGAMES splash**.
The user separately reports substantially more noticeable PSP/macOS differences
**near the end of the intro**, when the OMF logo appears and lightning animates
the **2097** number. The rendering flags, first divergent tick, and cause of
that later discrepancy have **not been investigated or verified**. In
particular, the early conclusion about unused shadow effects must **not** be
extrapolated to this late sequence.

The late-sequence investigation, reproducible steps, and acceptance criteria
are recorded in [INTRO_2097_REVEAL_ISSUE.md](INTRO_2097_REVEAL_ISSUE.md).
Leave it open; do not treat the earlier match as a fix or resolution.
