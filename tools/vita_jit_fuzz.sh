#!/bin/bash
# tools/vita_jit_fuzz.sh [COUNT] [setting=value...]
#
# The ARM JIT checked on the real Vita (qemu-user does not run under WSL1): random
# EE programs (tools/make_ee_fuzz_elf.py, plain and with loops) run on the Vita
# with the given settings (e.g. jit_regions=1 indirect_jit=1) and their final
# checksum (s7, written to eegprs.txt when the autostart run ends) must equal the
# PC build's. Needs WSL (Ubuntu-24.04) with /root/build-host built, and Vita Lab.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COUNT=${1:-10}
shift $(( $# >= 1 ? 1 : 0 ))
WROOT=$(echo "$ROOT" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')
OUT=$ROOT/runs/jit-fuzz-$(date +%Y%m%d-%H%M)
mkdir -p "$OUT"
DATA=ux0:data/VitaPS2
pass=0; fail=0
for mode in ${MODES:-"--likely" "--loops --likely"}; do
	for ((seed=1; seed<=COUNT; seed++)); do
		name=fuzz_${seed}${mode//[- ]/_}
		WOUT=$(echo "$OUT" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')
		MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-24.04 -u root -- bash -c "python3 $WROOT/tools/make_ee_fuzz_elf.py $WOUT/$name.elf $seed ${SIZE:-} $mode && cd /tmp && timeout 60 /root/build-host/vitaps2_host $WOUT/$name.elf --frames 60 --timeout 50 2>&1 | grep '^EE s6' > $WOUT/$name.expected" </dev/null >/dev/null 2>&1
		expected=$(cat "$OUT/$name.expected" 2>/dev/null)
		vitalab cmd "nosleep on" >/dev/null 2>&1
		vitalab quit all >/dev/null 2>&1; sleep 2
		vitalab put "$OUT/$name.elf" "$DATA/games/$name.elf" >/dev/null
		{ echo "game=$DATA/games/$name.elf"; echo "seconds=${SECS:-12}"; echo "show_stats=0"; for s in "$@"; do echo "$s"; done; } > "$OUT/autostart.txt"
		vitalab rm "$DATA/eegprs.txt" >/dev/null 2>&1
		vitalab put "$OUT/autostart.txt" "$DATA/autostart.txt" >/dev/null
		vitalab launch VTPS20000 >/dev/null
		sleep $(( ${SECS:-12} + 12 ))
		vitalab get "$DATA/eegprs.txt" "$OUT/$name.vita" >/dev/null 2>&1
		vitalab rm "$DATA/games/$name.elf" >/dev/null 2>&1
		got=$(head -1 "$OUT/$name.vita" 2>/dev/null)
		if [ -n "$expected" ] && [ "$got" = "$expected" ]; then
			pass=$((pass + 1))
		else
			fail=$((fail + 1)); echo "MISMATCH $name: PC [$expected] Vita [$got]"
		fi
	done
done
echo "$pass passed, $fail failed (settings: $*) -> $OUT"
