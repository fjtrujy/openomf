# PSP GU optimization and physical hardware clock comparison

**Date:** October 27, 2026
**Base commit:** `c3c0ca7b` (`new_psp`)

## Change summary

The native indexed PSP GU path now:

1. Maintains a **512-slot direct-mapped texture lookup hint** in addition to the existing authoritative texture array. Each hint is validated against texture data, immutable GUID, width and height before use. Evicted or reused slots cannot falsely match; hash collisions fall back to the original search.
2. Resolves each draw command's **CLUT variant once**, during GU frame preparation. The resolved index is stored in the command, avoiding an entire second linear lookup during the draw-list walk. This **does not** reuse CLUTs across frames or change palette semantics.
3. Sets the T8 texture mode and blending texture function **once per GPU frame** instead of repeating them for every sprite, and only binds a new indexed texture when the texture index or shadow-mask selection changes. The per-command color and CLUT changes are still applied correctly.

These are GU CPU-side bookkeeping optimizations, not a change to screen size, aspect ratio, sprite ordering, rendering effects, audio quality or game mechanics.

## Physical PSP benchmark

The connected PSP, through PSPLink USB / HostFS, ran 120-second Arena 4 AI-versus-AI stress tests. Resources were copied into separate build folders. CPU and bus clocks were restored after each normal exit. Each build used the native GPU renderer, not CPU fallback.

| Build | CPU/bus clock | Average after first five seconds | Arena 4 only | Observations |
| --- | --- | ---: | ---: | --- |
| **Before** (`c3c0ca7b`) | 222/111 MHz | **45.88 FPS** | 45.88 FPS (22 windows) | Stayed in Arena 4 throughout measurement |
| **Optimized** | 222/111 MHz | **49.75 FPS** | **47.64 FPS** (10 windows) | Later transitioned into another arena |
| **Optimized** | 333/166 MHz | **58.17 FPS** | **58.55 FPS** (19 windows) | Near 60 FPS sustained in Arena 4 |

**Important limitation:** These are individual randomized AI matches, not a deterministic paired benchmark. The baseline remained in Arena 4, whereas the optimized 222 MHz run visited another arena. The +3.87 FPS difference in overall averages **cannot** be attributed entirely to this optimization. Within the Arena 4-only windows, the difference is +1.76 FPS (~3.8%), which is encouraging but insufficient to claim a statistically robust improvement from one sample. The clock-speed comparison also involved different randomized fights, but shows a clear and much larger difference.

All three hardware runs reached clean engine shutdown, with **zero native audio output errors** and **zero CPU-composited effect frames**. The original 222/111 MHz clock was verified after the trials. Resource loading can cause occasional ~1-second initialization stalls, which are excluded from the reported steady-state comparison.

### Key takeaway on clock speed

**222 MHz is not sufficient for consistently smooth native PSP combat in these scenes**, even with the optimized renderer. At 333 MHz, most five-second samples in Arena 4 were around 57–60 FPS. Keep the current 333 MHz default for now; lowering the clock should be an explicit choice, not an automatic adjustment based on PPSSPP, which incorrectly predicted ~60 FPS at 222 MHz.

## PPSSPP validation

The optimized EBOOT completed a **120-second PPSSPP strict-JIT Arena 4
stress run**, reporting **59.2 FPS average** over 23 intervals (55.1 FPS
minimum, including an arena transition), **zero audio errors**, and a clean
engine shutdown. Its GU counter remained exclusively on the GPU path, with
no unexpected CPU effect replay. The 4:3 pillarbox clipping was preserved.

The normal, non-stress EBOOT was also launched independently in PPSSPP.
The OMF/2097 title and color palette rendered correctly in a window capture;
no missing sprites or corruption were observed. This is a visual smoke test,
not a claim of pixel-exact parity with the native macOS renderer.

## Evidence on development Mac (ignored build artifacts)

- `build/psp-real-baseline222/save/openomf.log` — unchanged pre-optimization source snapshot benchmark
- `build/psp-real-opt222/save/openomf.log` — optimized native GU, 222 MHz
- `build/psp-real-opt333/save/openomf.log` — optimized native GU, 333 MHz
- `build/psp-preopt-source-c3c0ca7b/` — archive of committed baseline source (not a Git worktree)
- `build/psp-gu-optimization-evidence/optimized-arena4.png` — actual emulator window screenshot
- `build/psp-gu-optimization-evidence/intro/` — optimized normal-intro captures
- `build/psp-stress-arena4/ppsspp-strict-fast-jit.log` — PPSSPP host log

## Future work

A more controlled benchmark should fix the arena, pilots, robots and AI random seed and repeat several runs per configuration. Target more substantial algorithmic improvements only after collecting native-PSP per-stage CPU timings; PPSSPP microsecond timings are not a reliable proxy for the physical hardware.
