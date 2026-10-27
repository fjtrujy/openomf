# PSP bounded exact combat shadows

**Date:** October 27, 2026
**Branch:** `new_psp`
**Scope:** Improve combat shadow palette fidelity without sacrificing the PSP's 222 MHz performance.

## Background

The original VGA compositor stores an indexed destination pixel and applies
some sprite effects *to that destination index*, not to a finished RGB color.
The PSP GU can use T8 textures and 256-entry CLUTs but lacks shaders and
arbitrary destination-index read/modify/write blending. Our earlier exact
intro path uses the GPU to cache an effect-free indexed prefix, then replays
the remaining draw commands in affected 16×16 tiles on the CPU.

Previously, shadows in combat always used approximate black-alpha blending,
regardless of clock speed. Enabling full exact indexed replay on all combat
frames is prohibitively expensive.

## Implemented bounded policy

The PSP GU renderer now conditionally performs the same original indexed
compositing for **small, shadow-only combat regions**:

- The CPU clock must be **at least 300 MHz**; the standard 333 MHz PSP
  configuration qualifies. At 222 MHz the existing fast GU path remains.
  The check uses `scePowerGetCpuClockFrequencyInt()`, so runtime CPU clock
  changes are respected.
- The frame must contain shadows, **no separate destination-index remap**,
  no dark-tint effect, and no additive effect. A PSP shadow *itself* may
  have `remap_rounds > 0`; this is normal OpenOMF shadow metadata rather
  than an independent effect.
- Every visible affected pixel must fit within at most **16 logical 16×16
  tiles** (maximum 4,096 pixels). Larger/mixed frames retain approximate
  native GU blending.
- The cached GPU-generated indexed prefix and exact indexed CPU suffix
  replay preserve draw order and are overlaid only within those tiles.
- The original intro still allows its independently validated 62-tile
  budget, independent of combat CPU-clock policy.
- Unsupported screenshots, additive effects, palette/texture fallback
  cases, and the pixel-exact software backend retain their prior behavior.

This is **partial exactness by frame/region**, not general pixel-exact
shadows on every combat frame, and not exact tint/remap handling in
all combat scenes. It uses no user-facing toggle.

## Experiment progression (physical PSP)

The diagnostic trials were built in isolated, ignored source copies and
tested on the connected PSP using the Arena 4 120-second AI combat harness.
Their fights are **randomized**, so small FPS differences are not strictly
deterministic speedups or regressions.

| Test implementation | Clock | Average FPS after warm-up | Exact combat frames | Outcome |
| --- | ---: | ---: | ---: | --- |
| Existing GU approximation (prior baseline) | 222 MHz | **57.9** | 0 | Reference |
| Shadow-only GPU prefix + up to 32 exact tiles | 222 MHz | **39.3** | 2,965 | Reject |
| Shadow-only GPU prefix + up to 16 exact tiles | 222 MHz | **50.4** | 3,108 | Reject |
| Shadow-only GPU prefix + up to 16 exact tiles | 333 MHz | **57.6** | 2,253 | Adopt only at high clock |
| Shadows, tint, independent remaps, up to 8 tiles | 333 MHz | **58.4** | 147 | Very limited coverage; do not adopt |

The 333 MHz baseline for our advanced-blit renderer was approximately
59 FPS on a separate real-hardware run. The bounded exact shadow variant
has a modest performance cost but keeps combat close to the 60 Hz cap,
whereas 222 MHz loses too much performance.

The 333 MHz 16-tile candidate completed its full 120-second run with
zero native audio output errors.

### Final production-source regression

After integrating the automatic clock check into the normal PSP GU backend,
we rebuilt and reran the production-source configurations rather than
relying solely on the isolated prototypes:

| Platform / clock | 120-second average after warm-up | Exact combat-shadow frames | Native audio errors |
| --- | ---: | ---: | ---: |
| Physical PSP, 222/111 MHz | **57.94 FPS** | **0** | **0** |
| Physical PSP, 333/166 MHz | **58.00 FPS** | **2,171** | **0** |
| PPSSPP, standard shipping-clock configuration | **59.39 FPS** | **3,147** | **0** |

All three runs finished normally. Hardware scene transitions and heavier
effects still showed occasional FPS dips, so this is not a claim of a
perfectly locked 60 FPS. Each combat run used independently randomized AI.

### Correctness verification

A PPSSPP diagnostic build compared each exact-corrected combat tile against
the **complete original indexed CPU compositor**, not against GU-alpha
blended output.

Across **20 sampled frames and 62,080 corrected pixels**, the comparison
reported **zero mismatches**. Samples included varied robots, animation
frames, positions and shadow coverage. This verifies that subset, **not**
every shadow in the game; oversized and mixed-effect frames remain
approximated.

Diagnostic full-frame comparisons and profiling code were compiled
only in ignored experimental builds, not in the shipping EBOOT.

## Hardware behavior and remaining work

The automatic clock threshold means the next full single-player build
retains the previously measured ~58 FPS combat behavior at 222 MHz.
At 333 MHz small, shadow-only regions become pixel-accurate; larger
robot shadows remain alpha-approximated.

The most expensive activity remains repeatedly reconstructing overlapping
sprite layers through the CPU indexed compositor when combat backgrounds
change. A future improvement needs genuinely different GPU composition,
more efficient ordered-region tracking, or a dedicated PSP VFPU-assisted
pixel path before broadening exact combat effects without FPS regressions.

**Follow-up optimization:**
[EXACT_TILE_LUT_OPTIMIZATION.md](EXACT_TILE_LUT_OPTIMIZATION.md) describes
the subsequently implemented reuse of existing shadow masks and lazy
palette-index remap lookups. The exact small-combat-shadow budget remains
16 tiles at 333 MHz after a wider 24-tile physical PSP comparison.

### Isolated local evidence

- `build/psp-combat-effects-investigation/audit222/save/openomf.log`
- `build/psp-combat-effects-investigation/shadow32-correct222/save/openomf.log`
- `build/psp-combat-effects-investigation/shadow16-correct222/save/openomf.log`
- `build/psp-combat-effects-investigation/shadow16-333/save/openomf.log`
- `build/psp-combat-effects-investigation/broad8-333/save/openomf.log`
- `$HOME/.config/ppsspp/PSP/GAME/OpenOMFExactShadowsVerification/save/openomf.log`

All tracked renderer changes are confined to the PSP-specific GU backend.
The pending upstream PRs and separate upstream-integration checkout have
not been modified.
