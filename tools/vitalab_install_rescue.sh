#!/bin/bash
# tools/vitalab_install_rescue.sh SKPRX: installs Vita Lab's kernel plugin with the rescue
# listener (UDP 1342, Vita-Lingo lab/vita/kernel/rescue.c) while Vita Lab still answers:
# saves the current plugin, uploads the new one, reboots, then checks Vita Lab and the
# rescue listener. To go back: vitalab put <the saved copy> ur0:tai/vitacompanion_kernel.skprx
# and reboot (if the Vita does not boot: hold L while it starts, taiHEN skips the plugins).
set -u
export PATH="$PATH:/c/Users/troy2/Downloads"
NEW=${1:?usage: $0 vitacompanion_kernel.skprx}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SAVED="$ROOT/runs/build/vitacompanion_kernel_backup_$(date +%Y%m%d_%H%M%S).skprx"
vitalab stats >/dev/null 2>&1 || { echo "Vita Lab does not answer: restart the Vita first"; exit 1; }
vitalab get ur0:tai/vitacompanion_kernel.skprx "$SAVED" >/dev/null || { echo "could not save the current plugin"; exit 1; }
echo "saved the current plugin: $SAVED ($(wc -c < "$SAVED") bytes)"
vitalab put "$NEW" ur0:tai/vitacompanion_kernel.skprx >/dev/null || { echo "upload failed"; exit 1; }
echo "uploaded $NEW ($(wc -c < "$NEW") bytes), rebooting"
vitalab reboot >/dev/null 2>&1
sleep 20
for i in $(seq 1 18); do
	if vitalab stats >/dev/null 2>&1; then echo "Vita Lab answers again after reboot"; break; fi
	sleep 10
done
vitalab cmd "nosleep on" >/dev/null 2>&1
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-24.04 -u root -- python3 /mnt/c/Users/troy2/Ps-vita-ps2-emu/tools/vita_rescue.py ping
