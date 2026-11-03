#!/bin/bash
# Real-world performance on a PS Vita: Silent Hill 2 (SLUS-20228) measured on
# the hardware, next to the synthetic instruction gap of tools/perf_gap.py.
#
#   tools/vita_realworld.sh RUN_DIR LABEL
#
# RUN_DIR holds the perf.csv of a scripted run that boots the game, waits on
# the title screen, picks New Game / Normal / Normal and lets the opening movie
# play (see runs/to_movie.sh in the PC-with-Vita-Lab workflow, docs/HANDOFF.md).
# Appends one row to docs/perf/vita_realworld.csv and rewrites
# docs/perf/vita_realworld.md (latest numbers and the history table).
#
# Columns (medians over each scene, per real second):
#   title_out_fps   frames shown on the title screen (the game draws at 30)
#   movie_vm_fps    emulated frames during the opening movie (full speed: 60)
#   movie_out_fps   frames shown during the movie
#   movie_pics      movie pictures decoded (BDEC macroblocks / 896; full speed: 30)
#   jit_ms_block    JIT compile time per translated block (whole run)
set -euo pipefail
RUN=$1; LABEL=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CSV="$ROOT/docs/perf/vita_realworld.csv"; MD="$ROOT/docs/perf/vita_realworld.md"
PERF="$RUN/perf.csv"
[ -f "$PERF" ] || { echo "no $PERF"; exit 1; }

col() { head -1 "$PERF" | tr ',' '\n' | grep -nx "$1" | cut -d: -f1; }
VM=$(col vm_fps); OUT=$(col out_fps); BDEC=$(col ipu_bdec)
JB=$(col jit_blocks); JO=$(col jit_opt_ms); JA=$(col jit_alloc_regs_ms); JG=$(col jit_codegen_ms)

median() { sort -n | awk '{v[NR]=$1} END {if(NR==0) print "-"; else printf "%.1f", (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2}'; }

# Title screen: the seconds before the first menu press (to_movie.sh presses at ~130s),
# once frames are shown.
TITLE=$(awk -F, -v o=$OUT 'NR>1 && $1>=110 && $1<=128 && $o>0 {print $o}' "$PERF" | median)
# Opening movie: seconds with MPEG decoding.
MOVIE_VM=$(awk -F, -v v=$VM -v b=$BDEC 'NR>1 && $b>0 {print $v}' "$PERF" | median)
MOVIE_OUT=$(awk -F, -v o=$OUT -v b=$BDEC 'NR>1 && $b>0 {print $o}' "$PERF" | median)
MOVIE_PICS=$(awk -F, -v b=$BDEC 'NR>1 && $b>0 {print $b/896}' "$PERF" | median)
JIT=$(awk -F, -v n=$JB -v o=$JO -v a=$JA -v g=$JG 'NR>1 {b+=$n; t+=$o+$a+$g} END {if(b) printf "%.2f", t/b; else print "-"}' "$PERF")

[ -f "$CSV" ] || echo "date,label,title_out_fps,movie_vm_fps,movie_out_fps,movie_pics,jit_ms_block" > "$CSV"
echo "$(date +%F),$LABEL,$TITLE,$MOVIE_VM,$MOVIE_OUT,$MOVIE_PICS,$JIT" >> "$CSV"

{
	echo "# Real-world performance on a PS Vita"
	echo
	echo "Silent Hill 2 (USA) on a PS Vita through Vita Lab, measured by"
	echo "\`tools/vita_realworld.sh\` from the emulator's perf.csv. Medians per real"
	echo "second. Full speed: 60 emulated fps; the game shows 30 fps on the title"
	echo "screen and plays its movies at 30 pictures per second. The synthetic"
	echo "instruction gap (\`gap.svg\`) says how much ARM code runs per PS2"
	echo "instruction; these numbers say what a player sees."
	echo
	echo "| Date | Build | Title screen fps | Movie: emulated fps | Movie: shown fps | Movie: pictures/s | JIT ms/block |"
	echo "|---|---|---|---|---|---|---|"
	tail -n +2 "$CSV" | tac | awk -F, '{printf "| %s | %s | %s | %s | %s | %s | %s |\n", $1, $2, $3, $4, $5, $6, $7}'
} > "$MD"
echo "$LABEL: title $TITLE fps, movie vm $MOVIE_VM / shown $MOVIE_OUT fps, $MOVIE_PICS pictures/s, JIT $JIT ms/block"
