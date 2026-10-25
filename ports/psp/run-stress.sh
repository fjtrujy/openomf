#!/usr/bin/env bash
# Build, run and evaluate one 120-second AI-vs-AI PSP arena in PPSSPP.
# Usage: ports/psp/run-stress.sh [arena:0-4] [safe-jit|interpreter|default-jit]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
arena="${1:-2}"
mode="${2:-safe-jit}"
if [[ ! "$arena" =~ ^[0-4]$ ]]; then
    echo "Arena must be 0..4" >&2
    exit 2
fi
if [[ -z "${PSPDEV:-}" ]]; then
    echo "Set PSPDEV to the installed PSP SDK before running this script" >&2
    exit 2
fi
export PATH="$PSPDEV/bin:$PATH"

ppsspp="${PPSSPP:-/Applications/PPSSPPSDL.app/Contents/MacOS/PPSSPPSDL}"
if [[ ! -x "$ppsspp" ]]; then
    ppsspp="$(command -v PPSSPPSDL || true)"
fi
if [[ -z "$ppsspp" || ! -x "$ppsspp" ]]; then
    echo "PPSSPP not found; set PPSSPP=/path/to/PPSSPPSDL" >&2
    exit 2
fi

args=()
case "$mode" in
    safe-jit)
        args=(-j --appendconfig="$root/ports/psp/ppsspp-no-fastmem.ini")
        ;;
    interpreter) args=(-i) ;;
    default-jit) args=(-j) ;;
    *)
        echo "Mode must be safe-jit, interpreter or default-jit" >&2
        exit 2
        ;;
esac

build="$root/build/psp-stress-arena$arena"
psp-cmake -S "$root/ports/psp" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPSP_STRESS_DEMO=ON \
    -DPSP_STRESS_ARENA="$arena" -DPSP_GU_APPROX_EFFECTS=ON >/dev/null
cmake --build "$build" -j8 >/dev/null

assets="${OMF_TEST_ASSETS:-$root/build/psp/PSP/GAME/OpenOMF/resources}"
if [[ ! -d "$assets" ]]; then
    echo "Missing packaged resources: $assets (run ports/psp/package.sh first)" >&2
    exit 2
fi
memstick="${PPSSPP_MEMSTICK:-$HOME/.config/ppsspp/PSP}"
game="$memstick/GAME/OpenOMFStress$arena"
mkdir -p "$game/save"
if [[ ! -d "$game/resources" ]]; then
    cp -R "$assets" "$game/resources"
fi
cp "$build/EBOOT.PBP" "$game/EBOOT.PBP"
log="$game/save/openomf.log"
report="$build/ppsspp-$mode.log"
rm -f "$log"

echo "Running arena $arena ($mode) with $ppsspp"
"$ppsspp" "${args[@]}" --windowed --log="$report" "$game/EBOOT.PBP" \
    >"$build/ppsspp-$mode.stdout" 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# PPSSPP remains on its menu after the guest exits. Watch the guest log,
# not the host process, to distinguish normal game completion from a crash.
started=$SECONDS
passed=0
while (( SECONDS - started < 155 )); do
    if [[ -f "$log" ]] && grep -q 'PSP STRESS: completed 120s run' "$log"; then
        passed=1
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        break
    fi
    sleep 1
done

if (( ! passed )); then
    echo "FAIL: no clean 120s completion; logs: $log and $report" >&2
    exit 1
fi
if ! grep -q 'Engine deinit successful' "$log"; then
    # Completion is printed before shutdown; allow its final log to flush.
    sleep 1
fi
if ! grep -q 'Engine deinit successful' "$log"; then
    echo "FAIL: completed game loop but shutdown was not confirmed" >&2
    exit 1
fi
if grep -Eq 'PSP GU texture cache exhausted|PSP GU draw command capacity exceeded' "$log"; then
    echo "FAIL: renderer dropped a draw command; see $log" >&2
    exit 1
fi

awk -F'FPS=' '/PSP STRESS: scene=/ {
    split($2, f, " "); fps=f[1]+0;
    if (!count || fps < minimum) minimum=fps;
    count++; sum+=fps;
} END {
    if (count) printf("PASS: %d windows, %.1f average FPS (%.1f minimum, includes loading)\n",
                      count, sum/count, minimum);
}' "$log"
grep 'PSP PCM cache:' "$log" | tail -1 || true
echo "Game log: $log"
echo "Emulator log: $report"
