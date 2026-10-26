# PPSSPP ARM64 fast-memory JIT crash: OpenOMF PSP

Investigation status: **a matching real-PSP null-derived pointer bug was found
and corrected in OpenOMF**. The corrected game completed three 120-second
strict-fast-memory classic-JIT regressions with no new crash. The exact
generated native ARM64 instruction in the earlier host crash is not known,
so this is strong evidence for a shared root cause rather than a formally
isolated JIT-instruction diagnosis.
Original PPSSPP test date: 2026-10-25. Hardware investigation: 2026-10-26.
Branch: `new_psp`. Host: Apple Silicon macOS;
PPSSPP SDL **v1.20.4**, ARM64. Test binary: optimized PSP EBOOT built with
PSPSDK and native PSP GU rendering.

## Real PSP discovery and likely root cause (October 26, 2026)

Running the same 120-second Arena 4 stress scenario on a physical PSP
(firmware 6.61, CPU 222 MHz) through PSPLink reproduced a **real guest-side
bus error** at roughly 40 seconds. PSPLink captured:

```text
Exception: Bus error (data), thread user_main, module SDL App
EPC       0x08866F64 (module offset 0x00062F64)
a0        0x0000002C
ra        0x0883CECC
BadVAddr  0x363FE1F3
```

Mapping the exact unstripped PSP PRX ELF:

```asm
00062f64 <array_get>:
  lw     v0, 0(a0)  # a0 = 0x2c, invalid array pointer
```

The return address identifies `player_run_apply()`, specifically an
`af_get_move()` call for the `TAG_UA` damage-animation command.
`af_get_move()` tail-calls `array_get(&a->moves, id)`, where `moves`
begins at offset `0x2c` within `af`. A NULL `af *` therefore becomes
the exact invalid array pointer `0x2c`.

**Game-code error:** `TAG_UA` unconditionally interpreted the current
object's userdata as `har`, but both HARs and projectiles can carry UA.
`projectile_local` has a different layout: the incorrectly interpreted
`har->af_data` field could read a projectile's `wall_bounce` field
instead of an AF pointer. The fix selects `har->af_data` for HARs and
`projectile_get_af_data()` for projectiles, validates the source and
target moves, and avoids dereferencing missing animation data.

The physical PSP subsequently completed three full 120-second arena tests
without another exception: at 222 MHz, manually selected 333 MHz, and
automatic 333 MHz. After the source fix, **two strict fast-memory
classic-JIT runs in Arena 4 each passed at 59.2 FPS average**, and
**Arena 2 passed at 59.0 FPS average**. No new macOS crash reports were
generated. Longer repetitions are still valuable because the original
crash was intermittent.

The PSP's invalid `0x0000002c` matches the low address portion of the
earlier PPSSPP host fault `0x000000030000002c`. This is a **much stronger
explanation than a previously hypothesized ARM64 cached-pointer compiler
defect**. PPSSPP's cached-pointer setting changed whether the invalid
access caused an obvious host crash, but did not establish that PPSSPP
generated incorrect guest logic. The BadVAddr field above is not itself
equal to `0x2c`; the register and exact faulting instruction provide
the actionable evidence.

## Symptom and reproduction

```sh
export PSPDEV=/Users/fjtrujy/Projects/toolchains/psp/pspdev
export PATH="$PSPDEV/bin:$PATH"

# Run 120 seconds of normal AI-versus-AI gameplay in Arena 4:
ports/psp/run-stress.sh 4 strict-fast-jit
```

`strict-fast-jit` sets `FastMemoryAccess=True` and
`IgnoreBadMemAccess=False`, and selects PPSSPP's classic JIT (`-j`).
Before the game-code fix, the host emulator intermittently terminated during combat, sometimes within
5–25 seconds, before OpenOMF can log orderly shutdown. Several Arena 2 runs
crashed later (approximately 55–90 seconds).

macOS crash reports repeatedly show:

```text
Exception: EXC_BAD_ACCESS (SIGSEGV)
Subcode: KERN_INVALID_ADDRESS at 0x000000030000002c
Thread: EmuThread
Stack: (anonymous JIT code) -> MIPSState::RunLoopUntil -> Core_RunLoopUntil
```

This is a **host virtual address**, not necessarily a fully resolved guest
address. Its low bits are compatible with a guest null-plus-offset read through
PPSSPP's fast-memory mapping, but the report alone does not prove the
offending guest instruction.

## Controlled CPU-backend observations

| PPSSPP CPU backend | Fast memory | Observation |
| --- | --- | --- |
| Classic ARM64 JIT (`-j`) | ON | Intermittently crashes in Arena 2 or 4 at the same host fault address |
| Classic ARM64 JIT (`-j`) | OFF, strict invalid accesses | Completed multiple 120s combat tests, including Arena 2 and 4 |
| IR-based JIT (`-J`, PPSSPP 1.20.x) | ON, strict invalid accesses | Completed **two** 120s Arena 4 tests with normal transitions, including a runner-measured **59.4 FPS average** |
| Interpreter (`-i`) | N/A | Completed 120s Arena 2 |
| Classic ARM64 JIT, `JitDisableFlags=0x02000000` (flush GPR cache after each instruction) | ON | **Still crashed** after about 40s at the same host fault address |
| Classic ARM64 JIT, `JitDisableFlags=0x00001000` (disable optimized integer load/store) | ON | **Passed** 120s Arena 4 |
| Classic ARM64 JIT, `JitDisableFlags=0x00400000` (disable pointerification) | ON | **Passed** 120s Arena 4 |
| Classic ARM64 JIT, `JitDisableFlags=0x01000000` (disable cached native pointers) | ON | **Passed twice** in 120s Arena 4 and once in 120s Arena 2, including natural scene transitions and clean shutdown |

Disabling only the cached-native-pointer optimization, while leaving fast
memory enabled, removed the observed failures in the tested pre-fix runs.
At the time, the **classic ARM64 JIT's cached-pointer load/store translation**
was our leading suspect. The subsequent real-PSP exception demonstrates
a genuine OpenOMF bug which better explains these observations. Successful
workarounds are not proof of a JIT miscompile.

### Specific PPSSPP source path

`Core/MIPS/ARM64/Arm64CompLoadStore.cpp` in v1.20.4 contains the optimized
load/store path:

```cpp
if (jo.cachePointers && g_Config.bFastMemory) {
    // aligned loads/stores with a small positive immediate offset
    gpr.MapRegAsPointer(rs);
    // for LW:
    LDR(INDEX_UNSIGNED, targetReg, gpr.RPtr(rs), offset);
}
```

`Core/MIPS/ARM64/Arm64RegCache.cpp` in `MapRegAsPointer` converts guest
registers to host pointers. With `enablePointerify`, it replaces the
upper 32 bits of a 64-bit register with the upper 32 bits of
`Memory::base` using `MOVK(..., SHIFT_32)`. Disabling pointerification
also passed a 120-second test, as did disabling optimized load/store
compilation entirely.

Source references:

- https://github.com/hrydgard/ppsspp/blob/v1.20.4/Core/MIPS/ARM64/Arm64CompLoadStore.cpp
- https://github.com/hrydgard/ppsspp/blob/v1.20.4/Core/MIPS/ARM64/Arm64RegCache.cpp
- https://github.com/hrydgard/ppsspp/blob/v1.20.4/Core/MIPS/JitCommon/JitState.h

## Guest-side debugger capture

PPSSPP 1.20.4's built-in WebSocket debugger was enabled with
`RemoteDebuggerOnStartup=True`, on local port 61654. A memory breakpoint
watching the null page logged:

```text
CHK Read32(CPU) at 00000034, PC=08ad265c (z_un_08ad2654)
```

A linker map from the **byte-identical** optimized ELF confirms:

```text
0x08ad2654  sceGuSwapBuffers
            libpspgu.a(sceGuSwapBuffers.o)
```

The corresponding instruction is:

```asm
08ad2654  lui   v0,0x08d5
08ad2658  addiu v0,v0,0x198c
08ad265c  lw    v1,0x34(v0)
```

The captured paused GPR state included `v0=0x08d5198c`,
`pc=0x08ad265c`, and `ra=0x08874ba4`. That `v0` value would normally
make the load address `0x08d519c0` (a non-null address).
The return address points to the PSP GU renderer's frame-finish path.

**Important caveat:** the debugger's reported `0x34` is not consistent
with the paused register snapshot. The memory-breakpoint helper recomputes
effective addresses from emulated registers and adds JIT instrumentation;
its register state or instruction timing may differ from the actual native
fast-memory access. Do **not** equate this first memory-breakpoint hit with
the later host fault at `0x30000002c` without a direct correlation.

We also narrowed the debugger breakpoint to **exactly** `0x2C` (four bytes)
with `memory.breakpoint.add`. In the classic JIT, it triggered immediately:

```text
CHK Read32(CPU) at 0000002c, PC=08870ecc (z_un_08870e14)
```

The linker map identifies `0x08870EC0` as OpenOMF's
`video_move_target()`. The instruction and captured register were:

```asm
08870ec0  lui   v0,0x08b9
08870ec4  addiu v0,v0,0x5460
08870ec8  move  v1,a0
08870ecc  lw    t9,0x2c(v0)
```

```text
v0=0x08b95460  pc=0x08870ecc  ra=0x08826248
expected effective address=0x08b9548c (not 0x0000002c)
```

That is a **second independent mismatch** between PPSSPP's guest
memory-breakpoint report and its paused guest-register snapshot.
Inspecting `Arm64Jit::CheckMemoryBreakpoint` in the PPSSPP v1.20.4 source
shows that `JitMemCheck` computes effective addresses from
`currentMIPS->r[rs]` rather than inspecting native JIT registers.
The debugger may observe a stale or not-yet-synchronized guest register.
Neither this nor the earlier broad breakpoint can currently establish the
instruction responsible for the native `0x30000002c` fault.

Relevant PPSSPP implementation:
`Core/MIPS/ARM64/Arm64Jit.cpp` (`JitMemCheck`,
`Arm64Jit::CheckMemoryBreakpoint`).

## Other investigated issues

The game's old stdin polling issued `BADF=sceIoRead(0, ...)` repeatedly.
It has already been disabled for PSP builds. The errors disappeared, but
the same JIT crash occurred afterward; stdin polling was not its root cause.
Additional AI null-pointer guards also did not eliminate Arena 4 crashes.
Both exact-effect and accelerated renderer variants compile, but the
crash-prone 120s automation uses the native GU hardware renderer.

## Next diagnostic steps

1. Use a debugger-enabled PPSSPP or a native ARM64 crash handler to capture
   the exact generated ARM64 instruction, pointer register and guest PC at
   the crash. The built-in `memory.breakpoint.add` hit on guest `0x2c`
   cannot substitute for this because its effective-address reports are
   inconsistent with the captured guest-register snapshots.
2. Extend post-fix classic-JIT + strict-fast-memory Arena 2 and 4 runs
   beyond the three successful two-minute regressions. If the host crash
   remains reproducible, then audit
   `MapRegAsPointer` register-lifetime tracking and capture an actual
   native register state at the fault.
3. Continue PSPLink physical-hardware testing: the original real PSP
   exception is now captured and the first three post-fix 120-second
   tests passed, but the full human-controlled campaign and Memory Stick
   XMB launch have not been validated yet.
4. Minimize the reproduction to a PSP GU-only test if the failing PC
   is ultimately shown to lie in `sceGuSwapBuffers`.

Use `ports/psp/run-stress.sh 4 no-pointer-cache` to reproduce the narrow
workaround (classic JIT and fast memory remain enabled), or use
`ports/psp/run-stress.sh 4 jit-ir` for the independent backend.
`ports/psp/run-stress.sh 4 safe-jit` remains the established conservative
workaround. These are PPSSPP execution-mode changes, **not a fix** for the
underlying classic-JIT compiler defect.
