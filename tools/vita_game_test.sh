#!/bin/bash
# tools/vita_game_test.sh GAME [SECONDS] [setting=value...]
#
# Everything we measure on a game, in one go, on a real PS Vita through Vita Lab:
#   1. keeps the Vita awake (nosleep), optionally installs a new build (EBOOT=path)
#   2. warm-up run(s) (WARM, default 1): the JIT cache fills, so the measured run
#      is not compiling (a build invalidates the cache)
#   3. measured run with scripted buttons, frame dumps, EE/IOP hotspots and RAM
#      dumps, then fetches every file and any new crash dump
#   4. tools/game_report.py: report.md (verdict against the goals, load phases,
#      per-phase costs, hotspots) and sheet.png (the frames)
#
# GAME is a file in ux0:data/VitaPS2/games/. Settings are autostart keys
# (README: ee_cycle_rate, mpeg_hle, ee_slice, ...); the game's profile and its
# settings file apply first, these override them.
#
# Environment:
#   OUT=dir           where to put the results (default runs/test-<game>-<date>)
#   EBOOT=path        install this eboot.bin first (then WARM should be >= 1)
#   WARM=n            warm-up runs (default 1; 0 when the cache is already warm)
#   PAD="t:button,.." scripted buttons for the game (seconds:button); default:
#                     START/CROSS alternately every 4 s from 15 s to 100 s, which
#                     gets most games past logos, title screens and "new game"
#   HOT_AT=s          start of the EE/IOP hotspot sampling (default: half way)
#   DUMP_EVERY=s      frame dump period (default 15)
#
# Example (RE4, its profile already has the right settings):
#   tools/vita_game_test.sh "Resident Evil 4 (USA).iso" 150
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME=${1:?game file name in ux0:data/VitaPS2/games}
SECS=${2:-150}
shift $(( $# >= 2 ? 2 : $# ))
SAFE=$(echo "$GAME" | tr -c 'A-Za-z0-9._-' '_' | cut -c1-40)
OUT=${OUT:-$ROOT/runs/test-$SAFE-$(date +%Y%m%d-%H%M)}
WARM=${WARM:-1}
HOT_AT=${HOT_AT:-$((SECS / 2))}
DUMP_EVERY=${DUMP_EVERY:-15}
if [ -z "${PAD:-}" ]; then
	PAD=""
	for ((t=15; t<=100; t+=4)); do
		if (( (t - 15) / 4 % 2 == 0 )); then b=start; else b=cross; fi
		PAD="$PAD${PAD:+,}$t:$b"
	done
fi
mkdir -p "$OUT"
DATA=ux0:data/VitaPS2

run() { # run NAME SECONDS settings...
	local dir=$1 secs=$2; shift 2
	mkdir -p "$dir"
	{ echo "game=$DATA/games/$GAME"; echo "seconds=$secs"; for s in "$@"; do echo "$s"; done; } > "$dir/autostart.txt"
	vitalab cmd "nosleep on" >/dev/null 2>&1
	vitalab quit all >/dev/null 2>&1
	sleep 2
	for f in perf.csv gsdiag.txt crash.txt eehot.txt iophot.txt syscalls.txt mpeghle.txt eeram.bin iopram.bin; do vitalab rm "$DATA/$f" >/dev/null 2>&1; done
	vitalab put "$dir/autostart.txt" "$DATA/autostart.txt" >/dev/null || { echo "could not send autostart.txt (Vita Lab busy or out of memory?)"; return 1; }
	vitalab launch VTPS20000 >/dev/null
	sleep $((secs + 12))
	for f in perf.csv crash.txt eehot.txt iophot.txt syscalls.txt mpeghle.txt eeram.bin iopram.bin; do vitalab get "$DATA/$f" "$dir/$f" >/dev/null 2>&1; done
	if vitalab ls "$DATA/frames" >/dev/null 2>&1; then
		for f in $(vitalab ls "$DATA/frames" 2>/dev/null | awk '{print $NF}' | grep ppm); do
			vitalab get "$DATA/frames/$f" "$dir/$f" >/dev/null 2>&1; vitalab rm "$DATA/frames/$f" >/dev/null 2>&1
		done
	fi
	[ -s "$dir/perf.csv" ] || echo "warning: no perf.csv from $dir"
}

echo "== $GAME -> $OUT"
vitalab cmd "nosleep on" >/dev/null 2>&1 || { echo "Vita Lab is not reachable"; exit 1; }
if [ -n "${EBOOT:-}" ]; then
	vitalab quit all >/dev/null 2>&1; sleep 2
	vitalab push-eboot VTPS20000 "$EBOOT" --no-launch || { echo "could not install $EBOOT"; exit 1; }
fi
BEFORE=$(vitalab crashes 2>/dev/null | wc -l)
for ((i=1; i<=WARM; i++)); do
	echo "warm-up $i/$WARM ($SECS s)"
	run "$OUT/warmup$i" "$SECS" show_stats=0 "pad_script=$PAD" "$@"
done
echo "measured run ($SECS s)"
run "$OUT" "$SECS" show_stats=0 "pad_script=$PAD" frame_dump_every=$DUMP_EVERY ee_hot_after=$HOT_AT dump_ee_ram=1 "$@"
AFTER=$(vitalab crashes 2>/dev/null | wc -l)
if [ "$AFTER" -gt "$BEFORE" ]; then
	vitalab crashes 2>/dev/null | tail -n $((AFTER - BEFORE)) > "$OUT/crashes_new.txt"
	echo "new crash dumps: $(cat "$OUT/crashes_new.txt" | tr '\n' ' ')"
fi
if command -v python3 >/dev/null 2>&1 && python3 -c 1 >/dev/null 2>&1; then
	python3 "$ROOT/tools/game_report.py" "$OUT"
else
	# Windows without Python: the report runs in WSL (/c/Users/... -> /mnt/c/Users/...)
	WSLOUT=$(echo "$OUT" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')
	WSLROOT=$(echo "$ROOT" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-24.04 -- python3 "$WSLROOT/tools/game_report.py" "$WSLOUT" </dev/null > "$OUT/report.log" 2>&1
	cat "$OUT/report.log"
fi
