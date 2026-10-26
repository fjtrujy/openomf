# Open issue: PSP intro OMF logo / 2097 lightning reveal mismatch

**Status:** Open; investigation not started.
**Reported:** October 26, 2026, after manual testing of the native PSP build.
**Affected branch:** `new_psp` in `fjtrujy/openomf`.
**Reference:** native macOS OpenGL3 renderer. **Target:** PSP GU renderer on PPSSPP and physical PSP.

## User-observed problem

Near the **end of the opening cinematic**, after the earlier EPIC MEGAGAMES and
DIVERSIONS sequences, the **OMF logo** appears and a thunderbolt/lightning stroke
animates the **2097** number. The PSP output has noticeable visual differences
compared with the native macOS version.

The precise mismatch (missing portions, colors, shadow/brightness, reveal order,
or frame timing) has **not yet been isolated**. Do not assume the effect uses
`SPRITE_SHADOW` or that the PSP approximate renderer is responsible.

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

## Diagnosis checklist (future work, not done yet)

- Instrument **this late segment**, not just the first 210 intro ticks, with
  synchronized animation-tick markers and scene/object identity.
- Capture native macOS, normal PSP GU (`PSP_GU_APPROX_EFFECTS=ON`) and PSP
  exact-effects (`OFF`) at the same logical tick and assets.
- Log draw calls, render layer/order, alpha, opacity, palette/remap changes,
  `SPRITE_SHADOW`, `SPRITE_DARK_TINT`, `SPRITE_INDEX_ADD` and remap rounds.
- Identify the **first divergent logical frame**, distinguishing timing,
  indexed color transformations and a real GU effect approximation defect.
- Important: `video_schedule_screenshot` forces PSP CPU replay on its capture
  frame; therefore indexed screenshots are **not proof of what the normal GPU
  path presented**. Capture actual PPSSPP/PSP GU output independently.
- Reproduce and compare on real PSP where practical. Do not silently select
  the software renderer or trade away stable gameplay performance.

## Existing comparison: limited to early intro

See [INTRO_RENDER_COMPARISON.md](INTRO_RENDER_COMPARISON.md): the EPIC MEGAGAMES
lightning flash was examined on macOS and PSP at intro ticks 15-210. In those
sampled early GU frames, no shadow, dark tint, remap or additive effect
commands were seen. **That result does not cover the end-of-intro OMF/2097
animation and cannot be used to close this issue.**

## Resolution criteria

- A reproducible, tick-aligned sequence documents the divergence, if any.
- Its root cause is isolated, or the apparent discrepancy is explained.
- Any fix is minimal, verified on native macOS, PPSSPP and physical PSP if
  available, and retains the native GU, software fallback, audio, input and
  performance baseline.

**No fix has been made or claimed.** This issue is independent from the
upstream PSP port split into reviewable pull requests.
