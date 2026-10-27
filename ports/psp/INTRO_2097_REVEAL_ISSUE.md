# Open issue: PSP intro OMF logo / 2097 lightning reveal mismatch

**Status:** Original black electrical silhouette fixed in PPSSPP; remaining
white-burst size/color parity discrepancy documented and open for refinement.
Optional physical PSP confirmation remains outstanding.
**Reported:** October 26, 2026, after manual testing of the native PSP build.
**Affected branch:** `new_psp` in `fjtrujy/openomf`.
**Reference:** native macOS OpenGL3 renderer. **Target:** PSP GU renderer on PPSSPP and physical PSP.

## User-observed problem

Near the **end of the opening cinematic**, after the earlier EPIC MEGAGAMES and
DIVERSIONS sequences, the **OMF logo** appears and a thunderbolt/lightning stroke
animates the **2097** number. The PSP output has noticeable visual differences
compared with the native macOS version.

The discrepancy was reproduced: the PSP GU approximation painted a large
**black, star-shaped silhouette** over the `2097` reveal where macOS shows a
bright electric burst. It was caused by the approximation of destination
palette-index remapping as a translucent **black** GU overlay; it is unrelated
to the `SPRITE_SHADOW` sprite option.

## Expected behavior

For the same game data and corresponding animation tick, PSP should preserve
the macOS rendering's logical sprite positions and stacking, intended palette
changes, light/dark transitions, electric stroke and final digits. Account for
screen aspect, interpolation and output resolution when comparing pixels.

## Reproduce

1. Start OpenOMF on macOS using OpenGL3 and the same freeware assets as PSP.
   Use the VGA 256-color palette (`USE_EXTENDED_PALETTE=OFF`) for closest parity.
2. Start `new_psp` on PPSSPP and/or a physical PSP.
3. Do not skip the intro. Progress beyond EPIC MEGAGAMES and DIVERSIONS until
   the OMF logo appears and lightning animates the 2097 digits.
4. Compare the full sequence from the first logo frame through the final
   fully revealed 2097 frame. Do not compare different animation phases.

## Diagnosis and comparisons (October 26, 2026)

- Captured macOS `OpenGL3` using the 256-color palette and PSP indexed
  screenshots at scene ticks 100-2500, with detailed three-way comparisons at
  ticks 800-1500. The logo appears around tick 900-1000, the lightning/digits
  develop around 1100-1300, and `2097` is visible around 1400.
- On PPSSPP, the PSP normal GU path drew destination-remap overlays in the
  OMF/2097 sequence (`INTRO_DRAW` counts of 1-3 remap commands per sampled
  frame); sampled `SPRITE_SHADOW`, `SPRITE_DARK_TINT` and `SPRITE_INDEX_ADD`
  counts remained zero in this interval.
- **Do not confuse screenshots with the presented GU output:** PSP's screenshot
  callback forces a CPU-composited frame. The indexed CPU screenshots looked
  close to macOS regardless of the GU-approximation setting. Independent
  captures of the actual PPSSPP window revealed the black silhouette.
- As a control, `PSP_GU_APPROX_EFFECTS=OFF` rendered a bright electric burst.
  A scene-specific *exact CPU* fallback also corrected the colors but caused
  **52 CPU effect frames** on real PSP hardware. The hardware profile interval
  containing these frames took approximately 15 seconds per 600 frames versus
  about 10 seconds when running entirely on the GU. This was rejected as an
  unacceptable shipping regression.
- The final candidate retains the fast GU path and uses a **bright RGB/alpha
  approximation** for destination-remap overlays during `SCENE_INTRO`. It
  replaces the erroneous dark silhouette with a white electric highlight in
  the actual PPSSPP display without CPU-effect fallback. This is visually
  closer to macOS but not guaranteed pixel-exact (the brightest star can be
  more uniformly white than the indexed reference).
- The subsequent isolated physical PSP test was blocked because PSPLink
  rejected the PRX with error `0x800200D9`. No new module was started by that
  failed attempt. The preceding exact-CPU performance experiment was stopped,
  and the PSP's original 222/111 MHz clocks were restored. A real PSP
  listening/viewing test of the final GU-only version is **still pending**.

## Final PPSSPP verification (October 26, 2026, evening)

Rebuilt and packaged the shipping `new_psp` EBOOT at commit `95b2818c`.
SHA-256: `792b73f27d0e281b2b362596ef352166f955a30094a006c0f4b3b4942d2fa1dd`.
Launched this exact standalone binary with the 57 packaged resources in
**PPSSPP v1.20.4**, strict-fast-memory classic JIT, with the native PSP GU
renderer (`PSP_GU_APPROX_EFFECTS=ON`). Captured 52 frames of the actual
PPSSPP window, rather than using OpenOMF's CPU-replay screenshot callback.

- The dark star/silhouette was **absent** in the lightning reveal.
- The burst was bright white/cyan, with the original OMF logo retained.
- The final `2097` digits appeared fully and correctly in blue.
- PPSSPP's frame counter stayed around **60/60 FPS** throughout the captured
  transition. A prior 120-second Arena 4 regression on this same codebase
  averaged **59.1 FPS**, with no indication that combat took the slow intro
  path.
- Captures and host logs: `build/psp/verify-2097-ppsspp/` (ignored build
  artifacts). Key evidence: `frame-06.png` (white electrical flash) and
  `frame-35.png` (finished logo and digits).

The user explicitly accepted PPSSPP verification as sufficient for deciding
whether this issue is fixed. **The original black-artifact issue is resolved
by the existing GU-only code; there was no need for another renderer patch.**
Physical PSP playback can still be checked separately if desired; it is not
a blocker for this PPSSPP-based resolution. The approximation is not claimed
to be perfectly pixel-identical to the macOS VGA reference.

## Further frame-synchronized comparison

The native macOS OpenGL3 and PPSSPP GU windows were subsequently captured at
ten matching actual intro ticks (901 through 1401), with neither backend using
the PSP's CPU-replay screenshot callback. This revealed a meaningful **visual
parity gap even after fixing the black silhouette**: during the reveal the
PPSSPP GU draws an oversized solid-white electrical star while macOS has a
smaller white core and red/orange textured fringes. At tick 1181, near-white
pixels in a 320×240 **4:3-preserving** image numbered 4,709 (macOS) versus
7,291 (PPSSPP); at tick 1401, static logo near-white pixel counts were
584 and 462 respectively. The first comparison's crop sizes were inconsistent
and its original metrics have been superseded; see the corrected report.

See [INTRO_2097_FRAME_PARITY.md](INTRO_2097_FRAME_PARITY.md) for all frame
comparisons, matching timestamps, pixel counts and evidence locations. The
initial **black star** bug remains fixed; **pixel-perfect fidelity has not
been achieved or claimed**. Future GU-only refinement is optional and must
preserve frame rate. All temporary frame-hold instrumentation was removed.

Diagnostic outputs were saved under ignored `build/desktop-min/intro-late-*`
and `build/psp-intro-*` directories. The debug screenshot and finite-profile
hooks were removed from the source after testing.

## Optional follow-up checks

**October 27 follow-up:** A bounded, intro-only exact indexed tile compositor
has been implemented and validated without slowing ordinary combat. It
restores red/orange electrical highlights and reproduces the full indexed
software result in a 10,112-pixel diagnostic check. See
[EXACT_INTRO_EFFECTS.md](EXACT_INTRO_EFFECTS.md) for implementation details,
222/333 MHz physical PSP timings, and the remaining approximation cases.

- Confirm that the bright-GU approximation looks correct on physical PSP;
  compare the timing of the electric flash without introducing stalls.
- Consider better approximations for highlight brightness and texture detail
  if visual parity warrants it. Do not enable whole-screen CPU fallback.
- Preserve ordinary combat sprite/shadow approximations and the software
  renderer fallback; repeat sustained combat regression tests.

## Existing comparison: limited to early intro

See [INTRO_RENDER_COMPARISON.md](INTRO_RENDER_COMPARISON.md): the EPIC MEGAGAMES
lightning flash was examined on macOS and PSP at intro ticks 15-210. In those
sampled early GU frames, no shadow, dark tint, remap or additive effect
commands were seen. **That result does not cover the end-of-intro OMF/2097
animation and cannot be used to close this issue.**

## Resolution criteria

- A reproducible, tick-aligned sequence documents the divergence, if any.
- Its root cause is isolated, or the apparent discrepancy is explained.
- The fix is minimal, compared with native macOS, and independently verified
  in PPSSPP using native GU presentation; hardware confirmation is optional.
- Native GU performance and the software-renderer fallback remain available.

**Original black artifact resolved in PPSSPP** by the bright-GU approximation
in `1db418c3`; the remaining electrical-highlight fidelity difference is
documented for follow-up. Keep this change independent of the upstream PSP
PR series until the renderer contribution is ready for review.
