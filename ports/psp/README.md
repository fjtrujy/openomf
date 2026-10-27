# OpenOMF PSP port

This port compiles OpenOMF's original C game engine using the PSPSDK, with
**native PSP GU sprite rendering**, **native PSP sceAudio mixing**,
**native sceCtrl controller input**, **native PSP kernel timing**, and
libxmp/PSM music. It is independent of the desktop OpenGL/epoxy CMake
build. The native PSP GU renderer uses `sceDisplay` directly and the
default `PSP_STANDALONE=ON` build **does not link SDL2, SDL2main or
SDL2_mixer**. Shared geometry, event, and scancode interfaces use OpenOMF
types; a small PSP-only compatibility layer implements remaining SDL-named
utility helpers without SDL headers or runtime libraries.

## Build and install

```sh
export PSPDEV=/Users/fjtrujy/Projects/toolchains/psp/pspdev
export PATH="$PATH:$PSPDEV/bin"

psp-cmake -S ports/psp -B build/psp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/psp -j8
sh ports/psp/package.sh build/psp
```

The packaged game is staged at `build/psp/PSP/GAME/OpenOMF/` (~16 MiB). Copy
this **whole directory** to `ms0:/PSP/GAME/OpenOMF/` on the memory stick:
the EBOOT is not standalone. All files in `resources/` must remain beside it.
`save/` holds logs, settings and saved progress.

### SDL2-free PSP executable

`PSP_STANDALONE=ON` (default) compiles a PSP module entry point and
registers a PSP kernel HOME/exit callback instead of using SDL2main.
The supported native path uses PSP GU, `sceAudio`, `sceCtrl`, and the
kernel clock. It uses normal PSP kernel semaphores for log synchronization,
the current EBOOT working directory for bundled resources, and standard
file I/O for network trace files. Inapplicable PSP keyboard and clipboard
operations remain inert. No SDL2 runtime or SDL2_mixer library is linked.

The shared renderer, scene, GUI-event and keyboard interfaces use
OpenOMF-owned `omf_rect`, `omf_event` and `omf_scancode` types, declared
in `src/utils/platform_types.h`. On desktop and legacy SDL-linked PSP
builds these are SDL aliases, maintaining the original ABI. With
`PSP_STANDALONE=ON`, the PSP compiles its own minimal versions without
any SDL SDK headers or SDL2 include directory. The compile dependency
files and linker flags can be inspected to verify this separation.

`ports/psp/psp_sdl_compat.c` still provides limited `SDL_*`-named
legacy utility operations (logging locks, resource paths, trace files,
inert keyboard and clipboard). Those names remain in some implementation
files, but are not an external SDL SDK or runtime dependency. Further
cleanup can rename these to entirely platform-neutral services.
Desktop behavior and the SDL-linked build are preserved.

The `Software` renderer is preserved in standalone mode by forcing the
existing indexed **CPU compositor** and presenting its RGBA result through
PSP GU. This does not require SDL software rendering, but it does require
GU for the final screen blit. The standard SDL software renderer remains
available when building the legacy PSP variant.

To build the previous SDL2-linked PSP implementation:

```sh
psp-cmake -S ports/psp -B build/psp-sdl-legacy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSP_STANDALONE=OFF
cmake --build build/psp-sdl-legacy -j8
```

If using `PSP_NATIVE_INPUT=OFF`, `PSP_NATIVE_TIMING=OFF`,
`PSP_NATIVE_DISPLAY=OFF`, `PSP_NATIVE_AUDIO=OFF`, or
`PSP_SDL_AUDIO_FALLBACK=ON`, also set `PSP_STANDALONE=OFF`.
The standalone target intentionally rejects those incompatible combinations.
The PSP HOME/exit callback has been observed registering on hardware;
manual HOME-button exit confirmation remains outstanding. A user has
confirmed that the native standalone game launches and runs smoothly from
the PSP Memory Stick XMB. The callback uses `sceKernelExitGame()`
as in PSPSDK samples; normal engine exit (rather than an OS-requested
HOME exit) restores the previous CPU/bus clock.

#### SDL2-free build validation (October 26, 2026)

The default PSP EBOOT is **1,764,476 bytes** without linking any of
`SDL2`, `SDL2main`, or `SDL2_mixer`. The equivalent legacy SDL-linked
build is **2,873,608 bytes** (standalone is about 39% smaller).
PSPLink/HostFS and PPSSPP both loaded resources, initialized
the native audio and controller devices and performed sustained
AI-versus-AI combat without an SDL runtime.

| PSP standalone run | 120s completed | Average FPS | Native audio errors |
| --- | --- | ---: | ---: |
| Real PSP at 333 MHz (native GU, registered HOME callback) | PASS | 58.23 | 0 |
| PPSSPP strict classic JIT (native GU, registered HOME callback) | PASS | 59.25 | 0 |
| PPSSPP strict classic JIT (CPU indexed `Software` renderer) | PASS | 29.69 | 0 |

All three tests shut down cleanly, with no new PSPLink exceptions or
PPSSPP host crashes. The PSP clock returned to 222 MHz on normal exit.
The CPU fallback reported `GPU=0` and thousands of CPU-composited frames
in the renderer counters; GU is still used to display the final image.
The legacy SDL-linked PSP configuration builds successfully as well.

These are randomized matches with loading included, not deterministic
CPU-only benchmarks. The full human-controlled campaign and PSP system
HOME button still require hands-on validation; direct XMB launch has passed.

#### Platform-neutral shared types validation (October 26, 2026)

| Run | Average FPS | Result |
| --- | ---: | --- |
| Real PSP native GU, 120 seconds | 56.56 | PASS |
| PPSSPP classic JIT native GU, 120 seconds | 59.46 | PASS |
| PPSSPP CPU `Software` fallback, 120 seconds | 29.71 | PASS |

The PSP build no longer adds `psp/include/SDL2` to its include paths
and the standalone dependency files reference no SDL2 SDK headers.
Native and legacy SDL-linked PSP builds compiled after the type migration,
as did the exact-effects and software-fallback variants. Hardware tests
reported zero PSPLink exceptions and audio output errors and restored
222 MHz on normal shutdown. Host C syntax checks of selected shared
modules also passed; a complete desktop build remains unavailable on
this Mac without SDL2_mixer.

A further standalone PPSSPP strict-fast-JIT regression run on October 26
completed Arena 4 for 120 seconds with **59.2 FPS** (native GU) and
**29.75 FPS** (forced indexed CPU `Software` renderer), averaged over
23 reporting windows each. Both runs reached normal engine shutdown,
reported zero native-audio output errors and no emulator memory faults.
The forced software run reported `GPU=0` and thousands of CPU-effect
frames, confirming that combat was not silently routed through GPU sprites.

Original One Must Fall 2097 data are not in git. `package.sh` downloads the
freeware data archive from `www.omf2097.com` if no archive is present. You can
supply your own file with `OMF_ASSET_ARCHIVE=/path/to/openomf-assets.zip`.
The special `ENGLISH.DAT2` OpenOMF extension catalog is generated from
`resources/ENGLISH2.TXT` using a small native-host C tool. Do not confuse it
with the original `ENGLISH.DAT` in the freeware assets.

**PPSSPP**: copy the complete directory into your emulator's
`PSP/GAME/` folder and launch `EBOOT.PBP` through the emulator.

**Physical PSP**: copy it to the Memory Stick, launch from the XMB, and check
`save/openomf.log`. The native GU renderer has also completed three
120-second AI-versus-AI combat tests on a real PSP running firmware 6.61
through PSPLink/HostFS (see results below). The packaged XMB launch is
user-confirmed; the full human-controlled campaign remains unverified.

On PSP hardware, `PSP_CLOCK_333=ON` (default) selects the PSP's supported
333 MHz CPU / 166 MHz bus clock for near-60-FPS combat, restoring the
previous CPU and bus clocks on normal exit. Higher clocks consume more
power; rebuild with `-DPSP_CLOCK_333=OFF` to respect the launcher's clocks.

The October 27 physical PSP clock comparison and GU cache/state optimizations
are documented in [GU_OPTIMIZATION_BENCHMARK.md](GU_OPTIMIZATION_BENCHMARK.md).
On the same device, 222 MHz averaged below 50 FPS after warm-up while 333 MHz
averaged near 58 FPS; PPSSPP alone did not predict this hardware difference.
Further cross-frame CLUT caching, shadow-mask memory reclamation and actual
physical GU execution timings are in
[GU_PERSISTENT_CLUT_PROFILE.md](GU_PERSISTENT_CLUT_PROFILE.md). The frame
completion wait was identified as the major hardware constraint at 222 MHz.
**Update:** [ADVANCED_BLIT_BENCHMARK.md](ADVANCED_BLIT_BENCHMARK.md)
documents a subsequent real-PSP fix: splitting large T8 sprites/backgrounds
into 32-texel strips dramatically improves texture-cache locality, reducing
the 222 MHz GU completion wait from ~10.5 ms to ~2.9 ms. This path is always
enabled in the PSP GU renderer **for textures at least 128×64 pixels**;
smaller textures use a single quad. There is no CMake option for this.
The 333 MHz clock remains the shipping default.

## Rendering architecture

The preferred **PSP GU** backend uploads 8-bit indexed sprite textures and
uses the PSP GPU's hardware CLUT for palette lookup, blending and compositing.
Textures are cached; the GPU synchronizes before buffers are reused. A bounded
cache of up to 4 MiB of indexed textures and precomputed shadow coverage avoids
per-frame CPU sprite rasterization.

The PSP has no programmable fragment shader, so OpenOMF's effects that alter
the *existing framebuffer's palette index* cannot be reproduced by normal
fixed-function blending. With `PSP_GU_APPROX_EFFECTS=ON` (default), most shadows
and dark tints use fast native GU alpha approximations. The OMF/2097 intro's
bright destination-index remap now reconstructs affected 16×16 tiles using
exact indexed CPU rules, with an indexed-background cache and a strict
workload budget; oversized effects still use GU approximation. See
[EXACT_INTRO_EFFECTS.md](EXACT_INTRO_EFFECTS.md) for the physical PSP
benchmarks and remaining fidelity limitations. To compare against the full
indexed software path:

```sh
psp-cmake -S ports/psp -B build/psp-exact -G Ninja \
  -DPSP_GU_APPROX_EFFECTS=OFF
cmake --build build/psp-exact -j8
```

That variant replays destination-index effects in a CPU framebuffer and is
**much slower in active matches**. The separate `Software` renderer remains
available as a fallback; the normal menu/game path defaults to `PSP GU`.
The 320x200 game is scaled into the 480x272 PSP screen.

The original game uses a **4:3 display aspect** despite its 320x200 VGA pixel
grid. The default video-menu `ASPECT: 4:3` mode uses a centered 362x272
viewport on the PSP, with 59-pixel black borders on each side. GU sprites and
effects are clipped to these boundaries so even fighters, projectiles and
camera shakes cannot draw into the pillarboxes. The menu's `ASPECT: NATIVE`
setting deliberately stretches the same game to the whole 480x272 display;
the full screen remains drawable in that mode. Scissoring is reset to the
whole display before clearing each frame, then applied to the unshifted
logical viewport, preventing stale borders when the aspect mode changes.

### Native PSP display lifecycle

`PSP_NATIVE_DISPLAY=ON` (default) calls `sceDisplaySetMode()` before
configuring the PSP GU draw and display buffers. With `PSP_STANDALONE=ON`,
startup does not call `SDL_Init()` or create an SDL window. Presentation
uses PSP VBlank synchronization and `sceGuSwapBuffers()`; normal shutdown
disables and terminates the GU. The optional SDL-linked PSP build retains
SDL initialization without the video subsystem for this display mode.

The desktop implementation is unchanged. Shared renderer interfaces use
`omf_rect` (an alias of `SDL_Rect` for SDL builds), while standalone PSP
defines its own rectangle type without importing SDL headers.

The previous PSP SDL video/window path remains selectable:

```sh
psp-cmake -S ports/psp -B build/psp-legacy-display -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSP_STANDALONE=OFF \
  -DPSP_NATIVE_DISPLAY=OFF
cmake --build build/psp-legacy-display -j8
```

The separate `Software` renderer is also retained. In standalone
mode it runs the indexed CPU compositor with GU presentation, while the
legacy SDL-linked build still initializes the SDL video subsystem lazily
before creating the software-renderer window. PSP builds use stub settings;
`-DPSP_TEST_SOFTWARE_RENDERER=ON` forces the fallback for testing only.

#### Display validation (October 26, 2026)

| Renderer and device | Arena 4 run | Average FPS | Result |
| --- | --- | ---: | --- |
| Native GU, physical PSP at 333 MHz | 120 seconds | 57.68 | PASS |
| Native GU, PPSSPP strict classic JIT | 120 seconds | 59.5 | PASS |
| SDL software fallback, PPSSPP strict classic JIT | 120 seconds | 50.79 | PASS |
| Legacy SDL-window GU, PPSSPP strict classic JIT | 120 seconds | 59.25 | PASS |

The native GU tests confirmed startup with SDL video uninitialized, no
SDL window, normal native presentation and clean GU shutdown. The PSP
reported no PSPLink exceptions, zero audio output errors and zero GU
CPU-effect fallback frames. The software test forced `Software` at
startup and verified that it successfully initialized SDL video on demand
and completed combat with clean shutdown. A legacy
`PSP_NATIVE_DISPLAY=OFF` build completed a 120-second combat run using
the original SDL-window GU renderer in PPSSPP. All measured FPS numbers
include scene transitions,
so different randomized fights are not deterministic performance tests.

The physical PSP was exercised through PSPLink/HostFS. The user also
confirmed a successful launch from the Memory Stick XMB and smooth gameplay,
but identified rough audio in the original single-buffer native build.

### Native PSP audio

`PSP_NATIVE_AUDIO=ON` (default) uses the native `sceAudio` hardware API
instead of SDL_mixer. A dedicated PSP audio thread combines three sound-effect
channels with streamed PSM music from **libxmp** into a stereo 16-bit
44.1 kHz stream. It supports sound priorities (handled by the existing
engine), per-channel volume, pitch, pan, fades, music playback and stopping.
The mixer synchronizes its audio callback and sample lifetimes with a PSP
semaphore, and releases owned PCM once playback finishes.

The native backend uses three rotating, 64-byte-aligned output blocks.
`sceAudioOutputBlocking()` queues a PCM buffer address, not an immediate copy
of its contents; mixing the next block into the same memory can corrupt audio
that has not yet been played. Triple buffering prevents that reuse without
introducing SDL or changing the engine's music/effect mixing levels.
`-DPSP_AUDIO_MIX_DIAGNOSTICS=ON` enables clipping and buffer-timing counters
for stress builds; the shipping configuration leaves these counters disabled.

In a 120-second Arena 4 run on physical PSP hardware (333 MHz), the original
single-buffer output reported **zero late audio blocks** and 126 clipped
samples out of 10.8 million; neither timing starvation nor clipping explains
persistent playback artifacts. After switching to three buffers, a second
120-second hardware run finished with **zero late blocks**, **zero audio
output errors** and no PSPLink exceptions. The diagnostic builds averaged
56.87 FPS (single buffer) and 57.94 FPS (three buffers), respectively;
randomized fights mean the difference is not a controlled speed comparison.
The three-buffer fix also completed PPSSPP's strict classic JIT Arena 4 test
for 120 seconds (59.4 FPS average, 23 measurement windows, zero audio output
errors, clean shutdown). A fixed standalone EBOOT was installed on the PSP
Memory Stick and verified byte-for-byte against the package. A human
listening comparison of the fixed EBOOT remains necessary to confirm the
audible improvement.

SOUNDS.DAT samples are unsigned 8-bit mono. Fixed-point resampling converts
each source and pitch to 44.1 kHz mono PCM, with a bounded 4 MiB converted
sample cache. Using mono rather than duplicated stereo buffers reduces memory
use. The PSP audio channel is inherently stereo, so this backend outputs
stereo even if the user's setting requests mono. PSP music and effects still
mix together, with no dependency on SDL_mixer.

Both alternative build configurations remain supported:

```sh
# Restore the original SDL_mixer implementation for A/B testing.
psp-cmake -S ports/psp -B build/psp-sdl-audio -G Ninja \
  -DPSP_STANDALONE=OFF -DPSP_NATIVE_AUDIO=OFF

# Include both: native by default, SDL_mixer selectable by audio-backend name.
psp-cmake -S ports/psp -B build/psp-dual-audio -G Ninja \
  -DPSP_STANDALONE=OFF -DPSP_NATIVE_AUDIO=ON \
  -DPSP_SDL_AUDIO_FALLBACK=ON
```

Both variants keep the desktop audio implementation unchanged.
The default standalone PSP build no longer links SDL2; shared SDL
type names and limited compatibility functions remain in PSP source.
Desktop keyboard/controller implementations are unchanged.

#### Native audio benchmark (real PSP, October 26, 2026)

Separate 120-second Arena 4 PSPLink/HostFS runs at 333 MHz CPU / 166 MHz bus.
The five-second FPS windows include scene setup, and the AI fights are
randomized, so these results are **not deterministic CPU-isolated benchmarks**:

| PSP audio configuration | Average FPS | PCM cache at exit | Audio errors |
| --- | ---: | ---: | ---: |
| SDL_mixer control (48 kHz stereo) | 57.97 | 3,396 KiB | No errors reported |
| Native sceAudio, initial mixer | 56.94 | 1,821 KiB | 0 |
| Native sceAudio, optimized gains | 58.02 | 1,355 KiB | 0 |
| Native sceAudio, SDL_mixer omitted from linking | 57.77 | 1,298 KiB | 0 |

The optimized run rendered 10,005 music blocks and 6,813 blocks containing
sound effects; 952 effects were started. A further 120-second native-only
hardware run completed without an exception after removing SDL_mixer
from the linker, recording 10,006 music blocks, 6,462 effect-active blocks
and 688 effect starts, with zero audio output errors. All runs exited cleanly, with no
PSPLink exceptions. The speed difference is within normal combat variability;
the clearest benefits are reduced middleware dependency and lower PCM memory.
Removing SDL_mixer from the default native build also reduced `EBOOT.PBP`
from 3,734,636 to 2,877,576 bytes (about 23%).

### Native PSP controller and event handling

`PSP_NATIVE_INPUT=ON` (default) compiles `src/controller/psp_joystick.c`
instead of the SDL GameController implementation. The PSP driver initializes
`sceCtrl` in analog mode and polls the built-in controller without an SDL
joystick thread. Both combat and menus deliver normal OpenOMF `ACT_*`
actions, preserving game logic and existing input-delay behavior.

| PSP control | OpenOMF action |
| --- | --- |
| D-pad / analog stick | Four movement directions, including diagonals |
| Cross | Punch / menu confirm |
| Circle | Kick / menu back |
| Start | Menu escape (not application exit) |
| Release directions/buttons | Stop |

Analog movement follows the prior SDL implementation's dead zone and
priority over the D-pad. HOME is left to the PSP system and is **not**
mapped to game/application exit. PSP-only input menus do not offer
inapplicable keyboard or second-local-joystick entries. Desktop joystick
and keyboard code is unchanged. PSP boot also enforces the native gamepad
for player one if an old desktop keyboard configuration was saved.

The standalone PSP engine bypasses SDL event polling and all SDL
subsystem initialization. With native GU enabled, SDL video startup
is bypassed too; SDL remains only a legacy source-level interface,
with native PSP implementations and optional SDL-linked builds.

Build the previous input backend for comparisons:

```sh
psp-cmake -S ports/psp -B build/psp-sdl-input -G Ninja \
  -DPSP_STANDALONE=OFF -DPSP_NATIVE_INPUT=OFF
```

For input tests, `-DPSP_NATIVE_INPUT_TRACE=ON` logs changes of
raw `sceCtrl` button and analog state together with decoded OpenOMF
actions. In PPSSPP 1.20.4, WebSocket debugger
`input.buttons.press` and `input.analog.send` were used to inject all
four directions, Cross, Circle, Start, and the four analog extremes.
Traces confirmed the expected action bits, a Cross-driven menu-to-game
transition, and return to neutral. The normal release has tracing OFF.

### Native PSP timing

`PSP_NATIVE_TIMING=ON` (default) uses the PSP kernel's monotonic
`sceKernelGetSystemTimeWide()` clock (microseconds, converted to
milliseconds) instead of `SDL_GetTicks64()`. Delays use
`sceKernelDelayThread()` rather than `SDL_Delay()`; long waits are
split into safe chunks to avoid microsecond argument overflow.
The shared `utils/platform_time.h` interface is used throughout the
game loop, combat/network timing diagnostics, software renderer and
native GU frame limiter, including stress-test reporting. The low 32
bits retain the same wraparound behavior as `SDL_GetTicks()`.

To compare with the previous implementation:

```sh
psp-cmake -S ports/psp -B build/psp-sdl-timing -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSP_STANDALONE=OFF \
  -DPSP_NATIVE_TIMING=OFF
cmake --build build/psp-sdl-timing -j8
```

The native clock changes neither game tick rates nor the GPU's existing
VBlank synchronization. Desktop builds still use SDL timing.
SDL2 is linked only in the optional legacy build. The default standalone
build uses native PSP timing and does not link an SDL runtime.

#### Native timing validation (October 26, 2026)

| 120-second Arena 4 run | Average FPS | Lowest 5-second FPS window | Result |
| --- | ---: | ---: | --- |
| Real PSP, kernel timing, CPU 333 MHz | 57.80 | 47.4 | PASS |
| PPSSPP classic JIT, kernel timing | 59.40 | 53.3 | PASS |
| PPSSPP classic JIT, SDL timing fallback | 59.29 | 55.7 | PASS |

All three tests completed the full 120 seconds using native GU,
native sceAudio and native sceCtrl. The physical PSP reported no
PSPLink exception, and its CPU clock returned to 222 MHz on normal
exit. PPSSPP used fast memory with strict invalid-memory checking;
neither run created a host crash report. Audio output error counts
remained at zero. The real-PSP GU trace contained no CPU-effect
fallback frames.

The PPSSPP differences are within run-to-run scene variability:
native timing is about independence from SDL, **not** a demonstrated
FPS improvement. Gameplay pacing and the five-second/120-second
stress reporting intervals were preserved.

## Repeatable gameplay stress tests

A non-shipping CMake option jumps directly to normal AI-vs-AI combat and tests
120 seconds of gameplay, rather than relying on the deceptively light title
screen. Use a separate build directory and any arena in 0..4:

```sh
psp-cmake -S ports/psp -B build/psp-stress-arena4 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DPSP_STRESS_DEMO=ON \
  -DPSP_STRESS_ARENA=4 -DPSP_GU_APPROX_EFFECTS=ON
cmake --build build/psp-stress-arena4 -j8
```

Install that build temporarily in a **separate** game directory, with the
same game assets, and collect `save/openomf.log`. It logs 5-second FPS
windows, GPU/CPU-effect frame counts, cache use, and outlier frame phases.
It will exit after 120 seconds. Any natural arena transition (e.g. a
victory screen) uses the original game logic; the harness must never replace
controllers or pilots during an active scene.

To build, launch and evaluate an arena in a single command, use the
repeatable test runner (requires an installed PPSSPP and a previously staged
`build/psp/PSP/GAME/OpenOMF/resources` directory):

```sh
export PSPDEV=/path/to/pspdev
ports/psp/run-stress.sh 2 safe-jit
# Diagnostic modes: no-pointer-cache, jit-ir, strict-fast-jit,
# interpreter, default-jit
```

The runner reports frame-window statistics, checks that the engine shuts
down successfully, and treats dropped GU draw commands or emulator crashes
as failures. It keeps emulator output under `build/psp-stress-arenaN/`.

### Physical PSP results (October 26, 2026)

The PSPLink USB console is running firmware 6.61, with native
480x272 GU rendering and a real SDL_mixer audio device. For hardware tests,
configure a dedicated `build/psp-hardware` with `-DBUILD_PRX=ON`,
`-DPSP_STRESS_DEMO=ON`, `-DPSP_STRESS_ARENA=4` and
`-DPSP_GU_PROFILE=ON`. Put the complete `resources/` directory beside
`openomf_psp.prx`. With HostFS mounted at the user's home directory, use
`pspsh` to change to the PRX's `host0:/` directory before `ldstart
openomf_psp.prx`. The game writes logs to `save/openomf.log`.

| PSP clock | 120s stress result | Average of 23 5s FPS windows | Minimum window |
| --- | --- | ---: | ---: |
| 222 MHz CPU / 111 MHz bus | PASS | 43.76 | 38.0 |
| 333 MHz CPU / 166 MHz bus (PSPLink setting) | PASS | 56.31 | 46.0 |
| 333 MHz CPU / 166 MHz bus (automatic `PSP_CLOCK_333`) | PASS | 56.57 | 48.5 |

All three tests completed with natural scene transitions and clean engine
shutdown. The first used the default 222 MHz clock; the second was clocked
to 333 MHz through PSPLink. The third verified automatic 222-to-333 MHz
switching and restoration to 222 MHz at
exit. FPS minimums include scene setup and transitions; single scene-loading
frames still take around 0.8-1.1 seconds via HostFS. The native GPU rendered
almost every frame: zero CPU-effect fallback frames in the first and third
runs; the manually clocked 333 MHz run logged one remap/shadow CPU fallback.
PSP GU texture storage remained
within its 4 MiB limit and the converted audio cache finished within its
4 MiB limit.

**Real-hardware crash found and corrected:** before the fix, combat raised a
PSPLink data bus error at `0x08866F64`, `array_get()`, with the array
pointer at `0x2c` after about 40 seconds. The caller is the `TAG_UA`
damage-animation path in `player_run_apply()`, which treated projectile
userdata as `har` userdata despite their different layouts. It now
resolves the animation file from the correct object type and validates both
source and target move data. Three subsequent 120-second hardware runs
reported no exceptions. This is a compelling explanation for PPSSPP's
older host fault at `0x30000002c`: after the fix, the previously crashing
classic JIT with strict fast-memory checks also completed **two 120-second
Arena 4 tests (59.2 FPS average each)** and **one 120-second Arena 2
test (59.0 FPS average)** without crashing. Longer playthroughs remain
necessary before declaring every gameplay path stable.

### PPSSPP results (October 25–26, 2026)

* The initial SDL software renderer started OpenOMF, but full combat was
  approximately 19–21 FPS while the menu was around 47 FPS.
* Native PSP GU rendering: menu 60 FPS. The first GPU implementation still
  used a CPU compositor for destination-index shadows; 552 of 600 sampled
  combat frames triggered that slow path.
* GPU approximate effects: combat mostly 45–54 FPS, but severe sub-5 FPS
  outliers appeared when animation events started PCM sound effects.
* Profiling identified the stalls as `player_run_apply` → `game_state_play_sound`
  → SDL PCM conversion (many 50–150 ms sound events). GPU sync, collision,
  drawing and VBlank were not responsible for these spikes.
* PSP-native PCM conversion plus bounded reusable sound cache: a complete
  **120-second Arena 4 AI-vs-AI run** completed cleanly. After warm-up,
  five-second combat intervals measured **59.7–60.0 FPS**, including sustained
  combat across natural scene transitions. There were **452 cache hits,
  32 misses**, 2.9 MiB of converted PCM at exit, and no measured 50–150 ms
  sound-conversion stalls. Entering/loading a new arena still took about
  370–430 ms in individual frames, so loading transitions are not yet
  entirely smooth.
* An additional Arena 2 test held 59.3–60 FPS during combat, but **multiple
  runs with PPSSPP v1.20.4's default JIT + fast memory access crashed the host
  emulator** at different times (including 55, 70 and 90 seconds). macOS
  reports show a near-null guest memory access at `0x30000002c`. The trigger
  is not yet identified, so these failures cannot simply be dismissed.
* **Two alternative PPSSPP CPU configurations completed Arena 2's full
  120-second test with natural scene transitions and no crash**: interpreter
  mode (`-i`) and JIT with `FastMemoryAccess=False` (diagnostic override).
  The latter reported 242 PCM cache hits, 33 misses and 3.9 MiB cached.
  Another complete 120-second run used `IgnoreBadMemAccess=False` as well as
  `FastMemoryAccess=False` and reported no invalid guest memory accesses.
* The automated `run-stress.sh 4 safe-jit` regression completed all 120
  seconds with natural scene transitions: **23 five-second FPS windows,
  59.3 FPS average, 54.9 FPS minimum (including arena loading)**,
  734 PCM-cache hits, 34 misses, and 3.3 MiB cached. Neither the GPU
  texture cache nor the draw-command queue reported an overflow.
* With additional AI guards for absent opponents and missing HAR userdata,
  **Arena 2 completed two 120-second default-JIT runs** and one
  `strict-fast-jit` run, which disables bad-memory ignoring while preserving
  fast memory access. The strict run averaged 58.2 FPS with a 50.1 FPS
  minimum five-second window, including transitions/loading.
* **This does not resolve the JIT crash:** two `strict-fast-jit` Arena 4 runs
  faulted on macOS at `0x30000002c`, after about 5–25 seconds. This is the
  same near-null emulated address as the earlier failures. There is no
  verified offending PSP instruction yet.
* The engine's debug console used to call `read(0,...)` every tick, causing
  repeated `BADF=sceIoRead(0,...)` errors in PPSSPP. Its stdin polling is now
  disabled on PSP only. The emulator's invalid-stdin errors disappeared,
  but the same JIT crash occurred afterward, so the stdin issue was **not**
  the root cause of the crash.
* With the completed hardening changes, `run-stress.sh 2 safe-jit` and
  `run-stress.sh 4 safe-jit` each completed 120 seconds with natural scene
  transitions, clean shutdown and zero invalid-stdin calls. Arena 2 averaged
  **59.2 FPS** over 23 windows (minimum 52.9); Arena 4 averaged **59.2 FPS**
  (minimum 53.7). Both minimums include scene-loading intervals. This is
  emulator testing only; physical PSP performance is unverified.
* Before the real-hardware `TAG_UA` fix, ARM64 PPSSPP 1.20.4 isolation tests narrowed the intermittent
  fast-memory crash to the classic JIT's optimized load/store handling.
  Disabling only cached guest-to-host pointers
  (`JitDisableFlags=0x01000000`) completed **two full Arena 4 stress tests**
  and **one full Arena 2 stress test**, without disabling fast memory.
  The independent IR JIT (`-J`) also passed
  two full Arena 4 tests with fast memory ON, whereas the classic JIT's
  register-flush mode still crashed at the original host fault address.
  The PPSSPP WebSocket debugger captured candidate guest memory breakpoints,
  but both reported addresses conflicted with the paused register snapshots.
  The PSPLink exception now establishes a genuine guest-side null-derived
  animation pointer bug; the earlier PPSSPP crash might share that cause.
  The exact faulty guest/native instruction in the emulator has not been
  independently captured.
* **After fixing `TAG_UA` source-type dispatch**, PPSSPP classic JIT
  (`-j`), `FastMemoryAccess=True`, `IgnoreBadMemAccess=False` completed
  three fresh 120-second tests across Arenas 4 and 2. No new macOS crash
  reports were generated. This supports a game-side root cause much more
  strongly than the old cached-pointer JIT-miscompile hypothesis.
  See [PPSSPP ARM64 JIT crash investigation](PPSSPP_ARM64_JIT_CRASH.md).

### PPSSPP JIT diagnostics and historical workaround (macOS)

Use the bundled configuration override with JIT while testing OpenOMF:

```sh
/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL -j \
  --appendconfig="$(pwd)/ports/psp/ppsspp-no-fastmem.ini" \
  --windowed "$HOME/.config/ppsspp/PSP/GAME/OpenOMF/EBOOT.PBP"
```

This disables PPSSPP's aggressive fast guest-memory path for the session
**without hiding invalid memory accesses**, and
**does not edit your global PPSSPP settings**. The fallback `-i` interpreter
also completed testing. Before the projectile/HAR userdata fix, this
was the conservative testing workaround. The corrected game now also passes
the tested strict fast-memory classic-JIT scenarios, so the workaround
is not necessary for those particular post-fix tests.

For a **narrower emulator workaround** that keeps classic JIT and fast
memory enabled while disabling the suspected cached-pointer optimization:

```sh
ports/psp/run-stress.sh 4 no-pointer-cache
```

It uses `ports/psp/ppsspp-no-pointer-cache.ini`. Alternatively,
`ports/psp/run-stress.sh 4 jit-ir` tests PPSSPP's independent IR-based JIT
with fast memory enabled. These are testing configurations, not changes to
the PSP EBOOT or proof of long-term physical-hardware stability.

For regression diagnosis, `run-stress.sh 2 strict-fast-jit` uses the bundled
`ppsspp-fastmem-strict.ini`, which enables fast memory but disables ignoring
bad reads. Earlier OpenOMF builds crashed in this mode; the corrected
build completed three Arena 2/4 test runs. Attempts to attach LLDB to PPSSPP were denied by
macOS debugserver permissions; recovering the PSP guest PC requires a
debugger-enabled environment or native PSPLink exception trace. The test
runner resets emulator logs before each run and rejects new invalid stdin
reads on PSP.

These are PPSSPP observations on a macOS host, **not a physical PSP FPS claim**.
The stress harness compiles instrumentation exclusively in `PSP_STRESS_DEMO`
builds and should never be shipped to users.

## Open work

* Verify complete five-arena playthroughs and sound quality/sync on a physical
  PSP, including PSP-1000 memory restrictions if that console is targeted.
* Continue long-run regression testing of the corrected projectile/HAR
  `TAG_UA` handling on PSP and classic ARM64 PPSSPP JIT. The old host
  fault has not reproduced after the fix in three strict fast-memory runs,
  but its exact generated ARM64 instruction remains unconfirmed. See
  the investigation report.
* Quantify long-term texture/PCM cache churn, dropped allocations and rare
  CPU-effect fallbacks under special moves and credits.
* Compare visual results against desktop OpenGL screenshots, particularly
  shadows, remap/tint effects and transparency. Retain exact software fallback.
* PS2: add a separate GS native hardware renderer and PS2SDK input/audio
  platform layer. The existing rendering callback interface and indexed-data
  source surfaces can be shared; PSP GU API calls cannot be carried over as-is.
