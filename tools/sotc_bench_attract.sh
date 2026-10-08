#!/usr/bin/env bash
# sotc: benchmark on the title screen's attract demo (Agro and Wander across the map, the same
# route every time), 60 FPS mode, no input. Prints FPS per 5 s, the average and the minimum
# from 60 s on, and saves a shot every 30 s (out/dump/sheet.png). Extra arguments: --env X=Y.
cd "$(dirname "$0")/.."
G=${SOTC_GAME_DIR:-"I:/EMULADORES/Playstation 4/JOGOS/Shadow Of The Colossus/CUSA08809"}
PY=${PY:-/c/PyBuild312/python.exe}
SECONDS_TOTAL=${BENCH_SECONDS:-360}
$PY scripts/sotc_modes.py "$G" --out out --modes fps60,af16 --cvars "${BENCH_CVARS:-}" >/dev/null
cp out/patches.bin out/bench.patches; cp out/cmdline.bin out/bench.cmdline
shots=(); for t in $(seq 60 30 $((SECONDS_TOTAL - 5))); do shots+=("$t:shot"); done
timeout $((SECONDS_TOTAL + 120)) $PY tools/sotc_play.py "$SECONDS_TOTAL" --patches out/bench.patches \
    --cmdline out/bench.cmdline --env BB_VBLANK_HZ=120 --env SOTC_FPS=1 "$@" "${shots[@]}" >/dev/null 2>&1
grep "SOTC FPS" out/run.txt | awk '{t=$5+0; f=$3+0; printf "%s@%d ", f, t; if (t>=60) {s+=f; n++; if (!m || f<m) m=f}}
    END {printf "\navg %.1f FPS, min %.1f (from 60 s, %d samples)\n", s/n, m, n}'
