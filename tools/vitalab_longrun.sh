#!/bin/bash
# tools/vitalab_longrun.sh OUTDIR SECONDS [settings...]: Silent Hill 2 for SECONDS
# (like tools/vitalab_run.sh), with a screenshot every 15s.
set -u
TITLE=VTPS20000; OUT=$1; SECS=$2; shift 2
GAME="ux0:data/VitaPS2/games/Silent Hill 2 (USA) (En,Ja).iso"
mkdir -p "$OUT"
{ echo "game=$GAME"; echo "seconds=$SECS"; for s in "$@"; do echo "$s"; done; } > "$OUT/autostart.txt"
vitalab quit all >/dev/null || true
vitalab rm ux0:data/VitaPS2/perf.csv >/dev/null 2>&1 || true
vitalab put "$OUT/autostart.txt" ux0:data/VitaPS2/autostart.txt >/dev/null
vitalab launch $TITLE >/dev/null
for ((t=15; t<SECS; t+=15)); do sleep 15; vitalab shot "$OUT/s_$t.png" --step 2 >/dev/null 2>&1 || true; done
sleep 12
vitalab get ux0:data/VitaPS2/perf.csv "$OUT/perf.csv" >/dev/null || echo "no perf.csv"
vitalab get ux0:data/VitaPS2/log.txt "$OUT/log.txt" >/dev/null 2>&1 || true
vitalab crashes
