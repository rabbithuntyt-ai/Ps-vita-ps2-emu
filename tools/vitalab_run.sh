#!/bin/bash
# Runs VitaPS2 on a Vita through Vita Lab (vitalab on the PATH) for a timed,
# scripted test and collects the results.
#
#   tools/vitalab_run.sh GAME_PATH SECONDS OUTDIR [setting=value ...]
#   tools/vitalab_run.sh ux0:data/VitaPS2/games/sh2.iso 90 runs/rate100 ee_cycle_rate=100 show_stats=1
#
# Optional: VPK=VitaPS2.vpk installs a build first; EBOOT=eboot.bin swaps only
# the program (quicker). Settings are per-game keys (ee_cycle_rate, safe_jit,
# threaded_spu, threaded_vu1, software_renderer, frame_skip, ...); they apply
# to this run only.
set -euo pipefail
TITLE=VTPS20000
GAME=$1; SECONDS_TO_RUN=$2; OUT=$3; shift 3
mkdir -p "$OUT"

if [ -n "${VPK:-}" ]; then vitalab install "$VPK"; fi
if [ -n "${EBOOT:-}" ]; then vitalab push-eboot "$TITLE" "$EBOOT"; fi

{
	echo "game=$GAME"
	echo "seconds=$SECONDS_TO_RUN"
	for setting in "$@"; do echo "$setting"; done
} > "$OUT/autostart.txt"

vitalab quit all || true
vitalab rm ux0:data/VitaPS2/perf.csv 2>/dev/null || true
vitalab put "$OUT/autostart.txt" ux0:data/VitaPS2/autostart.txt
vitalab launch "$TITLE"

# Vita-wide frame rate and per-core CPU load while it runs, plus screenshots
vitalab bench "$((SECONDS_TO_RUN - 5))" --csv "$OUT/vitalab_bench.csv" &
BENCH=$!
for t in 20 40 60; do
	if [ "$t" -lt "$SECONDS_TO_RUN" ]; then
		sleep 20
		vitalab shot "$OUT/shot_${t}s.png" --step 2 || true
	fi
done
wait "$BENCH" || true
sleep 10

vitalab get ux0:data/VitaPS2/perf.csv "$OUT/perf.csv" || echo "no perf.csv (crash? see crashes)"
vitalab get ux0:data/VitaPS2/log.txt "$OUT/log.txt" || true
vitalab crashes --get "$OUT/crashes" || true
echo "results in $OUT"
