# PSP upstream integration: staged pull request roadmap

**Prepared:** October 26, 2026. This is a handover plan, not implementation.
**Upstream base:** `omf2097/openomf:master`, *not* the old
`fjtrujy/openomf:master` (which is far behind upstream).
**Working reference:** `fjtrujy/openomf:new_psp` at `a5dc8346` when
this plan was prepared; re-check its HEAD when work actually starts.

## Why separate PRs?

As of October 26, the PSP branch contains 27 local commits not in upstream
and touches 73 files relative to the shared upstream history (~4,186 added
lines / 124 removed). Most of the original port arrived in one large commit
(`73d1f654`); cherry-picking it wholesale would create an unreviewable PR.

Upstream currently has two commits absent from `new_psp`. Fetch and inspect
the **live** upstream again before choosing integration bases.

The recommended strategy is to **reconstruct feature slices** from the known
working port in new branches, rather than rewrite or wholesale merge
`new_psp`. Keep every intermediate PR independently buildable. Downstream
PRs can be explicitly stacked where unavoidable; retarget them to upstream
`master` as predecessors land.

## Proposed PR series

| PR | Scope | Important boundaries |
| --- | --- | --- |
| 1. PSP foundation: SDL2 | PSP CMake target, SDL2-based window/renderer/input/audio/timing, initial PSP CI YAML building an EBOOT and optional packaging. Bring up gameplay on PPSSPP before replacing drivers. No native GU renderer yet. If upstream only provides OpenGL3, introduce an SDL2-presented indexed software renderer as the minimum viable PSP renderer, keeping it available later as fallback. | This PR must build as-is with PSPDEV and must not alter native desktop behavior. |
| 2. Small platform abstractions | Only the platform hooks shared by the drivers: event/timing types and registration seams as needed, with desktop aliases and unchanged desktop semantics. Make small, independently reviewable patches; postpone broad SDL type replacement until PR 8 where practical. | Avoid unrelated mass rewrites. |
| 3. Native PSP input | Add `sceCtrl` adapter with analog, D-pad, face buttons and menu controls; leave SDL2 video/audio/timing operational and SDL input selectable where feasible. | Stable gameplay and menu navigation. |
| 4. Native PSP clock/timing | Use PSP monotonic kernel clock/delays and proper frame pacing with fallback. Separate clock scaling / HOME lifecycle if review scope grows. | VBlank pacing and stable combat, no accelerated animations. |
| 5. Native PSP audio | `sceAudio`, libxmp music and effect PCM cache; preserve SDL_mixer fallback during transition. Include the **three rotating PCM output buffers** (`78665373`) that fixed audible corruption on real hardware. | Stable playback; no errors, no premature reused samples. |
| 6. PSP GU renderer | Introduce `src/video/renderers/psp/gu_renderer.c` with indexed T8/CLUT sprites, scene integration, and existing indexed software fallback. Compare special-effect behavior against desktop OpenGL before declaring visual parity. `PSP_GU_APPROX_EFFECTS` is not pixel-exact and must be documented/configurable. | Measured performance; no lost sprites/commands, no forced CPU compositing for ordinary frames. |
| 7. Native PSP display lifecycle | Remove SDL video/window dependency for the selected PSP GU path, keeping renderer selection and CPU fallback valid. | No SDL video initialization for native GU path; reliable start/exit/HOME. |
| 8. Fully SDL-free PSP build | Use `PSP_STANDALONE` and native input/audio/time/display, remove SDL2 and SDL_mixer from PSP includes/link graph. Port the narrow PSP compatibility shim and platform-neutral types deliberately; keep desktop SDL aliases and legacy PSP build as useful test controls. | `ninja -t commands` and link checks show no PSP SDL headers/libraries; full PSP playback/performance preserved. |
| 9. Isolated polish/fixes | Follow-up small PRs: PSP packaging and README, PPSSPP stress harness and JIT workaround notes, hardware lifecycle/333 MHz clock, minor shared crash/data fixes if independent. Prioritize independent correctness fixes separately from platform enablement. | Each issue isolated, validated and properly documented. |

## Validation gates for every PR

1. Start from a fresh version of `omf2097/master`; report the exact base
   SHA and the PR dependency order, including any cherry-picked precursor.
2. Build **that PR's own branch**, not only the final `new_psp` branch, and
   require its PSP GitHub Actions workflow to pass.
3. Confirm native macOS and existing upstream Linux/Windows builds and tests
   remain unaffected. For any changes in shared `src/` modules, run the
   strongest available desktop checks before opening a PR.
4. Run PPSSPP menu + combat smoke testing. For renderer/audio/input changes
   perform a sustained stress run. Verify on a connected real PSP when safe.
5. Do not alter `resources/`, overwrite saved games, remove software
   rendering, or introduce console exit through ordinary controller buttons.
6. Keep code, docs, commits and PR descriptions in **English**; include
   motivation, migration stage, validation matrix and concrete review notes.
7. Create the PR with **base = `omf2097:master`** (or label a deliberate
   stacked PR base explicitly), **head = `fjtrujy:<feature-branch>`**. Do
   not open one PR containing the whole 27-commit branch. Do not merge any PR
   without explicit authorization; keep user work and existing branches safe.

## Branch/check-out isolation

The user may continue investigating PSP rendering in another chat while
upstream work progresses. **Never run two agents switching branches or
editing the same checkout simultaneously.** Use an **independent clone**
for truly simultaneous work, not an additional Git worktree; otherwise
coordinate exclusive use of the existing checkout. Preserve `new_psp`
and all local files. Do not reset, clean, rewrite or force-push it.

## Pending unrelated visual issue

[INTRO_2097_REVEAL_ISSUE.md](INTRO_2097_REVEAL_ISSUE.md) documents a
**still-open** late-intro mismatch between native macOS and PSP during the OMF
logo / lightning reveal of 2097. The bright-GU remap approximation was verified
in PPSSPP on October 26 and removes the original dark silhouette; physical PSP
verification is optional and not yet repeated. The earlier EPIC MEGAGAMES
splash comparison does not validate this late sequence. Keep this fix separate
from initial platform integration unless it is ready and required for a focused
renderer PR; do not falsely claim pixel-exact visual parity.

## Handover expectation

The integration chat should first create a PR dependency map and validate
upstream freshness, then implement and open **PR 1 only** before proceeding
to the next PR. Report every upstream PR URL, commit SHA, branch, passing
tests, explicit limitations, and next dependency. The user controls the
merge sequence.
