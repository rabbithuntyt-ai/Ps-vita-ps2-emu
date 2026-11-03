#!/bin/bash
# Builds the Vita VPK in WSL (Ubuntu with VitaSDK in /usr/local/vitasdk; see
# docs/HANDOFF.md). Sources stay on the Windows side, the build directory is in
# the Linux file system (faster). Copies VitaPS2.vpk and eboot.bin to runs/build.
#
#   wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/Users/<you>/Ps-vita-ps2-emu/tools/build_vita_wsl.sh
. /etc/profile.d/vitasdk.sh
git config --global --add safe.directory '*'
SRC=$(cd "$(dirname "$0")/.." && pwd)
B=${BUILD_DIR:-/root/build-vita}
if [ ! -f "$B/build.ninja" ]; then
	cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DVITAPS2_COMMIT="$(git -C "$SRC" rev-parse --short HEAD)-local" || exit 1
fi
cmake --build "$B" -j"$(nproc)" || exit 1
mkdir -p "$SRC/runs/build"
cp "$B/VitaPS2.vpk" "$B/eboot.bin" "$SRC/runs/build/"
echo "built: $SRC/runs/build/VitaPS2.vpk"
