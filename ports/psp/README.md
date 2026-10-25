# OpenOMF PSP port

This port compiles OpenOMF's original C game engine using the PSPSDK, with
**native PSP GU sprite rendering**, SDL2 controller/input, SDL_mixer sound and
libxmp/PSM music. It is independent of the desktop OpenGL/epoxy CMake build.

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

Original One Must Fall 2097 data are not in git. `package.sh` downloads the
freeware data archive from `www.omf2097.com` if no archive is present. You can
supply your own file with `OMF_ASSET_ARCHIVE=/path/to/openomf-assets.zip`.
The special `ENGLISH.DAT2` OpenOMF extension catalog is generated from
`resources/ENGLISH2.TXT` using a small native-host C tool. Do not confuse it
with the original `ENGLISH.DAT` in the freeware assets.

**PPSSPP**: copy the complete directory into your emulator's
`PSP/GAME/` folder and launch `EBOOT.PBP` through the emulator.

**Physical PSP**: copy it to the Memory Stick, launch from the XMB, and check
`save/openomf.log`. The current port has been tested in PPSSPP, **not yet on
the connected physical PSP**; the PSPLink USB connection was unavailable for
the latest hardware test. Performance, sound and memory usage require separate
validation on hardware.

## Rendering architecture

The preferred **PSP GU** backend uploads 8-bit indexed sprite textures and
uses the PSP GPU's hardware CLUT for palette lookup, blending and compositing.
Textures are cached; the GPU synchronizes before buffers are reused. A bounded
cache of up to 4 MiB of indexed textures and precomputed shadow coverage avoids
per-frame CPU sprite rasterization.

The PSP has no programmable fragment shader, so OpenOMF's effects that alter
the *existing framebuffer's palette index* cannot be reproduced by normal
fixed-function blending. With `PSP_GU_APPROX_EFFECTS=ON` (default), shadows,
dark tint and remap overlays use native GU alpha approximations. Their colors
will differ from the desktop OpenGL reference. For fidelity comparisons:

```sh
psp-cmake -S ports/psp -B build/psp-exact -G Ninja \
  -DPSP_GU_APPROX_EFFECTS=OFF
cmake --build build/psp-exact -j8
```

That variant replays destination-index effects in a CPU framebuffer and is
**much slower in active matches**. The separate `Software` renderer remains
available as a fallback; the normal menu/game path defaults to `PSP GU`.
The 320x200 game is scaled into the 480x272 PSP screen.

### Audio improvements

The original sound samples are unsigned 8-bit PCM. The PSP backend now
resamples them directly to S16 mono/stereo with fixed-point linear interpolation,
avoiding a very expensive general-purpose SDL format conversion on each sound
event. Converted pitched samples are also shared through a bounded 4 MiB PCM
cache. Both optimizations are guarded with `__PSP__`; desktop audio retains
its existing path.

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

### PPSSPP results (October 25, 2026)

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
* An additional Arena 2 test held 59.3–60 FPS during combat, but **two runs
  with PPSSPP v1.20.4's default JIT + fast memory access crashed the host
  emulator** at different times (roughly 55 and 90 seconds). Both macOS
  reports show a near-null guest memory access at `0x30000002c`. The trigger
  is not yet identified, so these failures cannot simply be dismissed.
* **Two alternative PPSSPP CPU configurations completed Arena 2's full
  120-second test with natural scene transitions and no crash**: interpreter
  mode (`-i`) and JIT with `FastMemoryAccess=False` (diagnostic override).
  The latter reported 242 PCM cache hits, 33 misses and 3.9 MiB cached.

### PPSSPP JIT crash workaround (macOS)

Use the bundled configuration override with JIT while testing OpenOMF:

```sh
/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL -j \
  --appendconfig="$(pwd)/ports/psp/ppsspp-no-fastmem.ini" \
  --windowed "$HOME/.config/ppsspp/PSP/GAME/OpenOMF/EBOOT.PBP"
```

This disables PPSSPP's aggressive fast guest-memory path for the session and
**does not edit your global PPSSPP settings**. The fallback `-i` interpreter
also completed testing. A guest null-pointer bug remains a possibility, so
the underlying crash and real-PSP behavior still need investigation.

These are PPSSPP observations on a macOS host, **not a physical PSP FPS claim**.
The stress harness compiles instrumentation exclusively in `PSP_STRESS_DEMO`
builds and should never be shipped to users.

## Open work

* Verify complete five-arena playthroughs and sound quality/sync on a physical
  PSP, including PSP-1000 memory restrictions if that console is targeted.
* Investigate the two reproducible macOS PPSSPP fast-memory JIT crashes seen
  during Arena 2 gameplay; distinguish a JIT translation bug from a genuine
  guest near-null dereference before calling all arenas crash-free.
* Quantify long-term texture/PCM cache churn, dropped allocations and rare
  CPU-effect fallbacks under special moves and credits.
* Compare visual results against desktop OpenGL screenshots, particularly
  shadows, remap/tint effects and transparency. Retain exact software fallback.
* PS2: add a separate GS native hardware renderer and PS2SDK input/audio
  platform layer. The existing rendering callback interface and indexed-data
  source surfaces can be shared; PSP GU API calls cannot be carried over as-is.
