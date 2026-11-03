#!/bin/bash
# tools/vitalab_compat.sh [OUTDIR] [SECONDS] [games...]
#
# Unattended compatibility run on a PS Vita through Vita Lab: boots every game
# in ux0:data/VitaPS2/games/ (or the ones named), plays SECONDS (default 300)
# while pressing START / CROSS now and then to get past title screens and
# menus, and collects screenshots, perf.csv and crash dumps per game. Each run
# appends one row per game to docs/perf/compat_runs.csv; tools/compat_report.py
# turns that file into docs/COMPATIBILITY.md.
#
# Automatic result (screenshots decide "in game" vs "menus" by eye):
#   crash    a new core dump appeared
#   no-boot  the GS never drew anything
#   boots    drew frames; fps and bottleneck columns tell how fast
set -u
TITLE=VTPS20000
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-runs/compat-$(date +%Y%m%d-%H%M)}; SECS=${2:-300}; shift $(( $# > 2 ? 2 : $# ))
CSV="$ROOT/docs/perf/compat_runs.csv"
GAMEDIR="ux0:data/VitaPS2/games"
mkdir -p "$OUT"

if [ $# -gt 0 ]; then
	GAMES=("$@")
else
	mapfile -t GAMES < <(vitalab ls "$GAMEDIR" | grep -iE '\.(iso|cso|chd|isz|cue|mds|bin|elf)$' | sed 's|.*/||')
fi
[ -f "$CSV" ] || echo "date,game,serial,result,first_draw_s,vm_fps,out_fps,ee_pct,vu1_pct,gs_wait_pct,jit_ms_block,crash" > "$CSV"

# Median of column NAME over rows after the first draw (perf.csv is per second).
col() { head -1 "$1" | tr ',' '\n' | grep -nx "$2" | cut -d: -f1; }
median_after() {
	local file=$1 name=$2 scale=${3:-1} start=$4 c
	c=$(col "$file" "$name"); [ -z "$c" ] && { echo "-"; return; }
	awk -F, -v c=$c -v s=$scale -v t0=$start 'NR>1 && $1>=t0 {print $c*s}' "$file" | sort -n |
		awk '{v[NR]=$1} END {if(NR==0) print "-"; else printf "%.1f", (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2}'
}

for GAME in "${GAMES[@]}"; do
	SAFE=$(echo "$GAME" | tr -c 'A-Za-z0-9._-' '_')
	DIR="$OUT/$SAFE"; mkdir -p "$DIR"
	echo "== $GAME"
	BEFORE=$(vitalab crashes | wc -l)
	{ echo "game=$GAMEDIR/$GAME"; echo "seconds=$SECS"; echo "show_stats=1"; echo "auto_tune=1"; } > "$DIR/autostart.txt"
	vitalab quit all >/dev/null 2>&1 || true
	vitalab rm ux0:data/VitaPS2/perf.csv >/dev/null 2>&1 || true
	vitalab put "$DIR/autostart.txt" ux0:data/VitaPS2/autostart.txt >/dev/null
	vitalab launch $TITLE >/dev/null
	for ((t=10; t<SECS; t+=10)); do
		sleep 10
		# Past logos, title screens and "press start" prompts; most menus
		# accept CROSS for their first option (new game / continue).
		case $(( t % 40 )) in
			10) vitalab press start 150 >/dev/null 2>&1 ;;
			30) vitalab press cross 150 >/dev/null 2>&1 ;;
		esac
		(( t % 30 == 0 )) && vitalab shot "$DIR/s_$t.png" --step 2 >/dev/null 2>&1
	done
	sleep 12
	vitalab get ux0:data/VitaPS2/perf.csv "$DIR/perf.csv" >/dev/null 2>&1
	vitalab get ux0:data/VitaPS2/log.txt "$DIR/log.txt" >/dev/null 2>&1
	AFTER=$(vitalab crashes | wc -l)

	SERIAL=$(grep -oE '\b(SLUS|SCUS|SLES|SCES|SLPM|SLPS|SCPS|SLKA)[-_][0-9]{3}\.?[0-9]{2}\b' "$DIR/log.txt" 2>/dev/null | head -1 | tr -d '.' | tr '_' '-')
	RESULT=boots; FIRST=-
	if [ "$AFTER" -gt "$BEFORE" ]; then RESULT=crash; fi
	if [ -s "$DIR/perf.csv" ]; then
		DRAWS=$(col "$DIR/perf.csv" gpu_draws)
		FIRST=$(awk -F, -v c=$DRAWS 'NR>1 && $c>0 {print $1; exit}' "$DIR/perf.csv")
		[ -z "$FIRST" ] && { FIRST=-; [ "$RESULT" = boots ] && RESULT=no-boot; }
	else
		[ "$RESULT" = boots ] && RESULT=no-boot
	fi
	START=${FIRST/-/0}
	VM=$(median_after "$DIR/perf.csv" vm_fps 1 "$START")
	OUTFPS=$(median_after "$DIR/perf.csv" out_fps 1 "$START")
	EE=$(median_after "$DIR/perf.csv" ps2_EE 100 "$START")
	VU1=$(median_after "$DIR/perf.csv" ps2_VU1 100 "$START")
	GSW=$(median_after "$DIR/perf.csv" ps2_GSwait 100 "$START")
	# JIT cost per compiled block over the whole run (scene loads).
	JIT=$(awk -F, -v b=$(col "$DIR/perf.csv" jit_blocks) -v f=$(col "$DIR/perf.csv" jit_finish_ms) -v t=$(col "$DIR/perf.csv" jit_translate_ms) \
		'NR>1 {n+=$b; ms+=$f+$t} END {if(n) printf "%.2f", ms/n; else print "-"}' "$DIR/perf.csv" 2>/dev/null)
	CRASH=$([ "$RESULT" = crash ] && vitalab crashes | tail -1 | awk '{print $1}' || echo -)
	echo "$(date +%Y-%m-%d),\"$GAME\",${SERIAL:--},$RESULT,$FIRST,$VM,$OUTFPS,$EE,$VU1,$GSW,${JIT:--},$CRASH" | tee -a "$CSV"
done
python3 "$ROOT/tools/compat_report.py" || true
