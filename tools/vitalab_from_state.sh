#!/bin/bash
# tools/vitalab_from_state.sh OUTDIR SECONDS [settings...]
# Boots Silent Hill 2, loads its save state (SELECT + R saves one in game) and
# collects perf.csv and screenshots like tools/vitalab_longrun.sh. Test a scene
# without playing through the intro first.
set -u
OUT=$1; SECS=$2; shift 2
mkdir -p "$OUT"
{ echo "game=ux0:data/VitaPS2/games/Silent Hill 2 (USA) (En,Ja).iso"; echo "seconds=$SECS"; echo "load_state=1"; for s in "$@"; do echo "$s"; done; } > "$OUT/autostart.txt"
vitalab quit all >/dev/null || true
vitalab rm ux0:data/VitaPS2/perf.csv >/dev/null 2>&1 || true
vitalab put "$OUT/autostart.txt" ux0:data/VitaPS2/autostart.txt >/dev/null
vitalab launch VTPS20000 >/dev/null
for ((t=15; t<SECS; t+=15)); do sleep 15; vitalab shot "$OUT/s_$t.png" --step 2 >/dev/null 2>&1 || true; done
sleep 12
vitalab get ux0:data/VitaPS2/perf.csv "$OUT/perf.csv" >/dev/null || echo "no perf.csv"
vitalab crashes | tail -2
