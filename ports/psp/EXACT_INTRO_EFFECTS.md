# PSP indexed intro effects: selective exact compositing

**Date:** October 27, 2026
**Branch:** `new_psp`
**Scope:** Native PSP GU renderer, original OMF/2097 electrical reveal

## Problem

OpenOMF's original indexed VGA renderer applies some sprite effects to
the **destination palette index**, not to the source's RGB color. The PSP GU
supports T8 indexed textures and CLUTs but cannot render to an indexed
framebuffer or perform an arbitrary destination-index lookup. Existing
`PSP_GU_APPROX_EFFECTS=ON` reproduced these remaps with white/black alpha
blending. This kept gameplay fast but produced a large solid-white
electrical star in the intro instead of the original red/orange fringes.

Simply switching the whole frame to the indexed CPU renderer is exact but
unacceptably slow. A first attempt to correct all shadows/remaps by CPU
replaying tiles reduced a 222 MHz combat stress test to ~20–50 FPS,
depending on the scene. **That experiment was not kept.**

## Implementation

The normal GU indexed renderer and the already validated 32-texel advanced
blitting remain untouched. Only frames with the intro's
`FBUFOPT_BRIGHT_DESTINATION_REMAP` can use localized correction:

1. Find which 16×16 logical tiles contain **visible** pixels from a
   destination-index remap sprite. Merely intersecting its bounding box is
   not enough: electrical sprites are mostly transparent.
2. If at most 62 tiles (approximately 16,000 logical pixels) require
   correction, recover their destination indices with the existing original
   indexed CPU-compositing rules, replaying all intersecting sprites in their
   original submission order. Frames exceeding that budget retain the
   established GU approximation instead of introducing severe CPU stalls.
3. Cache the unmodified indexed background **before the first effect
   command**. Compare every source texture GUID and draw command field, and
   invalidate when the remap tables change or the scene changes. Copy that
   base into the selected tiles and replay only later changing draw commands.
   This prevents reconstructing the static intro logo every frame.
4. Convert corrected tile pixels to a shared aligned RGBA scratch texture,
   write back the CPU cache once, then overlay all corrected tiles with a
   single GU draw after ordinary sprites. The rest of the frame stays fully
   GU-accelerated.
5. The indexed CPU fallback for other unsupported features, screenshots and
   full-frame exact-effects builds remains available.

All additional buffers are lazily allocated. If they cannot be allocated,
the normal GU approximation remains the fallback.

**Important:** The scope is **intro destination-index remapping**; combat
shadows, dark tint and other effects retain their fast fixed-function
approximation. This is an improvement in feature parity, **not yet a claim
of complete desktop pixel fidelity**.

## Fidelity validation

A temporary diagnostic build paused PPSSPP at game tick **1181**, using
the already-rendered GU output, and captured the 2097 logo. Compared with
the prior single-quad white-alpha result, the corrected image restores
the narrower white electrical core, red/orange fringes and surrounding
logo detail. The initial frame-by-frame macOS comparisons are documented
in `INTRO_2097_FRAME_PARITY.md`.

For correctness independent of the prefix cache, a second temporary
diagnostic recomputed the same marked tiles with the **full indexed CPU
renderer** and compared their packed RGBA colors. At the sampled intro
effect frame, **10,112 pixels were compared with zero mismatches**.

The diagnostic holds, full software verification, and temporary timed
exit instrumentation were removed from tracked source after testing.

## Physical PSP profiling

Native PSPLink/HostFS tests used the same connected PSP. Timed intro
runs were 35 seconds, and the combat regression ran for 120 seconds.
The 333 MHz clock was restored to its original 222/111 MHz setting on
normal exit.

| Intro measurement | 333 MHz first selective-rectangle trial | Optimized tile + indexed-prefix cache |
| --- | ---: | ---: |
| Exact frames in reporting interval | 184 | 219 |
| CPU preparation per corrected frame | **10.55 ms** | **7.25 ms** |
| Skipped large-effect frames | 2 | 4 |

The intro frames and tile counts differ between these **independent
non-deterministic runs**; these numbers are measured workload samples,
not a deterministic speedup percentage.

For the optimized build at **222 MHz**, the intro correction averaged
**11.36 ms per corrected frame**, in the measured 600-frame reporting
interval (160 corrected frames). At 333 MHz the mean was 7.25 ms per
corrected frame. This remains a meaningful CPU cost; 333 MHz should
remain the shipping default until all scenes and effects meet the
frame-time budget.

### Cost breakdown at 333 MHz

In one interval with 219 corrected frames:

| CPU stage | Total in interval | Per corrected frame |
| --- | ---: | ---: |
| Detect affected tiles | 409 ms | ~1.87 ms |
| Reuse/rebuild indexed background | 271 ms | ~1.24 ms |
| Replay sprites after effect boundary | 735 ms | ~3.36 ms |
| Resolve RGB and flush cache | 559 ms | ~2.55 ms |

The reusable indexed-prefix cache recorded **214 hits and 5 builds**
in that interval. As with the prior experiments, the exact replay
is still appreciably more expensive on real hardware than emulator
timings suggested.

### 222 MHz full combat regression

A full 120-second AI-versus-AI stress run retained **57.89 FPS
average after warm-up**, with **58.09 FPS** in Arena 4 only,
**zero CPU-composited frames**, zero native audio output errors, and
a clean exit. The previous advanced-blit-only configuration averaged
57.91 FPS in a separate randomized match. This verifies no evident
combat regression, though the matches are not frame-identical.

## Rejected alternatives and limitations

- A single enclosing rectangle required too many pixels to be replayed.
- Eight-pixel tiles only modestly reduced the CPU cost compared with
  16-pixel tiles, while increasing the number of GE primitives.
- Reconstructing **all combat shadows/dark tints** at 222 MHz caused a
  substantial frame-rate regression. It is deliberately **not enabled**.
- In unusually large intro flashes, the bounded implementation may still
  show the original approximate effect for some frames.
- No screenshots or resource data were committed; local PPSSPP and real-PSP
  artifacts remain under ignored `build/` folders.

## Evidence on the development Mac

- `build/psp-exact-intro-hardware333/save/openomf.log` — original
  localized-rectangle timing
- `build/psp-exact-stage2-hardware333/save/openomf.log` — optimized
  selective-tile 333 MHz timing and stage breakdown
- `build/psp-exact-final-intro222/save/openomf.log` — optimized
  selective-tile 222 MHz timing
- `build/psp-exact-final-stress222/save/openomf.log` — physical combat
  regression
- `build/psp-exact-verify-tick1181/evidence/held-frame.png` — actual
  GU display captured at game tick 1181
- `build/psp-exact-verify-tick1181/evidence/game.log` — exact software
  comparison (`mismatches=0`)

The upstream PSP integration checkout and the pending small upstream PRs
were left untouched.
