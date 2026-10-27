# PSP parallel indexed framebuffer investigation

**Date:** October 27, 2026
**Reference:** `new_psp` at `0f46f464`
**Scope:** Feasibility experiments only. No production renderer or PSP build configuration was changed.

## Question and conclusion

Could the PSP keep an exact indexed representation of the 320×200 game alongside
the fast fixed-function GU color output, so shadows and destination-index remaps
no longer need expensive repeated CPU sprite reconstruction?

**Conclusion:** A continuously maintained **CPU indexed mirror is too
expensive** on physical hardware, even with cached draw-list prefixes or
conservative dirty-tile invalidation. A **GPU-encoded index sidecar** is
technically feasible in spare VRAM and almost preserves the original 222 MHz
frame rate, but **only normal sprite composition is exact**. The PSP GE cannot
perform the arbitrary destination-index read/modify/write operations needed to
keep that GPU sidecar correct after a shadow, tint or palette remap. A promising
next step is a **temporary, effect-region GPU index pass**, followed by one
bounded CPU correction per ordered group of effects. It must be validated at
222 and 333 MHz before being enabled by default.

The existing native GU renderer, 32-texel advanced blit and 16×16 intro exact
patching remain the shipping implementation.

**Follow-up milestone:** [GPU_INDEX_PREFIX.md](GPU_INDEX_PREFIX.md)
implements an *on-demand* GPU prefix snapshot for the intro's exact tiles,
with physical 222/333 MHz performance and pixel-reference validation. It
does not enable the incomplete all-frame GPU index sidecar or general
exact combat effects.
The follow-up [EXACT_COMBAT_SHADOWS.md](EXACT_COMBAT_SHADOWS.md) documents
bounded shadow-only exact corrections at 333 MHz without a persistent
indexed framebuffer. Large and mixed combat effects remain approximate.

## Technical constraints

The current GU renderer consumes immutable T8 indexed sprite textures and
applies RGBA CLUTs to draw into 8888 PSP display buffers. OpenOMF's original
indexed effects also have `remap`, `dark` and `add` state that depends on the
*previous destination index*. The PSP fixed-function GU cannot store a T8
render target or sample and arbitrarily transform the destination index while
writing the same pixel.

The CPU reference (`indexed_pixel`) contains a 16-bit index, dark value and
additive value plus an 8-bit remap selector; this is **8 bytes per logical
pixel** with alignment (approximately **500 KiB** at 320×200). Keeping it
current independently of GU drawing means doing a substantial amount of
sprite composition twice.

An alternative can store the resulting 8-bit normal-sprite index in the red
byte of an offscreen RGBA8888 pixel. Using `sceGuDrawBufferList`, a third
512×256 target fits in the PSP's 2 MiB GE VRAM alongside the current two
display buffers:

| VRAM allocation | Bytes |
| --- | ---: |
| Display buffer A, 512×272×4 | 557,056 |
| Display buffer B, 512×272×4 | 557,056 |
| Encoded-index target, 512×256×4 | 524,288 |
| **Total** | **1,638,400** |
| **VRAM remaining out of 2,097,152 bytes** | **458,752** |

The test used a dedicated 256-entry RGBA index-encoding CLUT for each
indexed-color source variant. It tracked the current GU draw-buffer address
returned by `sceGuSwapBuffers()` and restored that address after the
offscreen rendering pass, without changing the visible framebuffer format.

This is a **useful index representation for ordinary sprites**, not a complete
framebuffer attachment: ordered dithering/partial alpha, `SPRITE_SHADOW`,
`SPRITE_DARK_TINT`, `SPRITE_INDEX_ADD` and destination remap logic are
not representable by that direct GE pass alone.

## Real physical PSP tests at 222/111 MHz

All tests used the same Arena 4 AI-vs-AI stress harness, resources and GU
advanced-blitting path. The matches were independently randomized, so small
FPS differences should not be treated as exact causal speedups. The
diagnostics ran **only in ignored, isolated build snapshots** with source
macros absent from the shipping repository.

| Approach | After-warm-up FPS | Additional CPU index work/frame | Average `sceGuSync` wait | Outcome |
| --- | ---: | ---: | ---: | --- |
| Existing single-pass GU | **57.91** | None | 2.86 ms | Reference |
| Full indexed CPU mirror alongside GU | **10.67** | **68.66 ms** | 2.84 ms | Reject |
| CPU mirror with cached unchanged draw prefix | **18.70** | **33.85 ms** | 2.88 ms | Reject |
| CPU mirror with inter-frame 16×16 dirty tiles | **25.69** | **20.92 ms** | 2.59 ms | Reject |
| Duplicate GU sprite pass, same target (proxy only) | **56.18** | None beyond GU command setup | 4.77 ms | Feasible cost floor |
| **Extra GPU RGBA encoded-index target** | **55.70** | No full CPU mirror; periodic diagnostic readback | **4.06 ms** | Promising, incomplete effects |

The full CPU-mirror run was intentionally stopped after multiple combat
windows and one 598-frame report, since the substantial performance
regression was clear. The duplicate same-target GPU test was similarly
ended after several full reporting intervals. The prefix, dirty-tile and
actual GPU offscreen-target tests completed **120-second runs** and exited
cleanly with zero audio output errors. The sampled GPU pass included a
full-index CPU comparison every 600 frames (measurement overhead).

The GU wait remaining similar during CPU-mirror tests is expected: the
CPU, not the GPU, is consuming the frame budget.

### Why persistent CPU dirty tiles were insufficient

The prototype conservatively compared every recorded sprite command with
its preceding-frame version. It invalidated the tiles touched by both the
old and new rectangles of changed commands, then reconstructed invalid
tiles in exact submission order.

Over five reporting intervals, **20.1% of all 16×16 tiles** were rebuilt;
the average CPU cost still exceeded **20 ms per frame**. Moving robots,
multi-layer animation and effects invalidate too much indexed state.
A more optimized dirty-region matcher might lower overhead, but it would
need a dramatic reduction to beat the 16.67 ms frame budget while leaving
room for normal game logic and GU presentation.

### GPU-encoded indices: correctness and readback

The real physical PSP could draw 320×200 logical indices into the additional
RGBA target while retaining the visible GU presentation. Reading **all
64,000 encoded pixels** from mapped GE VRAM cost **3.17 ms on average**
across sampled combat windows (full-frame CPU read, not a PSP DMA copy).
The corresponding GPU wait averaged **4.06 ms**, versus **2.86 ms** without
the second pass.

Actual comparisons against the original *full indexed CPU compositor*:

| Sample scene | Reference pixels | Index mismatches | Frame effect flags |
| --- | ---: | ---: | --- |
| Intro, GPU frame 600 | 64,000 | **0** | 0 |
| Intro, GPU frame 1,200 | 64,000 | **0** | 0 |
| Intro, GPU frame 1,800 | 64,000 | **11,949** | 1 (destination remap) |
| Arena 4, GPU frame 600 | 64,000 | **617** | 3 (remap + shadow) |
| Arena 4, later sampled frames | 64,000 each | 501–5,847 | Usually 3 |

In the independently randomized Arena 4 test, the encoded target diverged
from the true indexed framebuffer by **2.68% of pixels on average** across
the periodic 64,000-pixel checks. This is precisely why it cannot yet be
fed back as authoritative destination-index state after effect commands.

**Important caveats:** Normal-sprite zero-mismatch results are two sampled
intro frames, not exhaustive verification of every sprite transformation.
The periodic CPU reference comparison introduces benchmark stalls; the
55.70 FPS is not an isolation of only the extra offscreen GE draw. The
GPU encoded buffer was not used to alter the shipping game's pixels.

## Recommended architecture for a future renderer PR

1. **Keep the existing single-pass GU renderer by default.** A permanent
   CPU mirror is ruled out by the measurements.
2. Introduce an internal, memory-accounted, **ephemeral GPU index target**
   (or logically equivalent scratch area) *only when an exact destination
   effect requires it*. Avoid a second full-screen pass during ordinary
   combat frames unless hardware tests show ample headroom.
3. Render the **ordered, effect-free prefix** into index-coded RGBA, using
   the already cached T8 sprites and the same efficient advanced blitting.
   Do not render a previous destination-index effect into this buffer as if
   it were ordinary source-index compositing.
4. Sample or read back only the **actual affected 16×16 tiles**, not all
   64,000 pixels. The measured 3.17 ms full read is a reason to test
   smaller regions, **not** a justified estimate for their latency.
5. Apply the original destination-index remap, dark, additive and shadow
   rules to those pixels in correct draw order on the CPU; submit a GU
   correction patch and continue rendering later sprites. Group adjacent
   effects where semantics allow, minimizing mid-frame `sceGuSync` barriers.
6. Compare effect pixels against exact CPU reference, including alpha,
   multiple overlapping effects, palette changes, remap-table updates,
   flipped/scaled sprites, ordering and scene transitions. Test on
   physical PSP at **222 and 333 MHz**. Preserve the existing bounded GU
   approximation for regions that would miss their frame budget.
7. Keep offscreen VRAM resources explicitly separate from the two display
   buffers. Restore the actual active draw-buffer pointer after each
   scratch pass, and do not sample a texture while the GPU is writing to
   its source storage.

The first small upstream contribution should be a **GU index-target
abstraction plus diagnostic parity tests**, not a flag that silently
enables all exact combat effects. A follow-up PR can add bounded,
order-safe localized corrections. This preserves reviewability and gives
clear performance stop/go criteria.

## Local evidence (ignored files)

- `build/psp-indexed-mirror-investigation/full/save/openomf.log`
- `build/psp-indexed-mirror-investigation/prefix/save/openomf.log`
- `build/psp-indexed-mirror-investigation/dirty/save/openomf.log`
- `build/psp-indexed-mirror-investigation/gpu-pass/save/openomf.log`
- `build/psp-indexed-mirror-investigation/gpu-index/save/openomf.log`
- `build/psp-indexed-mirror-investigation/gpu-index-intro222/save/openomf.log`
- `build/psp-indexed-mirror-investigation/source/` — isolated benchmark
  source snapshots, **not** a tracked source or additional Git worktree
- `build/psp-indexed-mirror-investigation/gpu-index-source/` — isolated
  source snapshot for GE index-target testing

The repository's `new_psp` code, shipping EBOOT, Memory Stick saves,
resources, existing upstream PRs and upstream-integration checkout
were not changed as part of these experiments.
