#!/usr/bin/env bash
# sotc: repeatable benchmark in 60 FPS mode: +nointro starts outside the Shrine of Worship,
# Wander walks forward for 25 s; prints the average FPS of 20..45 s. Extra arguments: --env X=Y.
cd "$(dirname "$0")/.."
G=${SOTC_GAME_DIR:-"I:/EMULADORES/Playstation 4/JOGOS/Shadow Of The Colossus/CUSA08809"}
PY=${PY:-/c/PyBuild312/python.exe}
$PY scripts/sotc_modes.py "$G" --out out --modes fps60,af16 --cvars "nointro" >/dev/null
cp out/patches.bin out/bench.patches; cp out/cmdline.bin out/bench.cmdline
timeout 150 $PY tools/sotc_play.py 50 --patches out/bench.patches --cmdline out/bench.cmdline \
    --env BB_VBLANK_HZ=120 --env SOTC_FPS=1 "$@" 18:ly=0:27000 >/dev/null 2>&1
grep "SOTC FPS" out/run.txt | awk '{t=$5+0; if (t>=25 && t<=46) {s+=$3; n++}} END {printf "avg FPS %.1f over %d samples\n", s/n, n}'
