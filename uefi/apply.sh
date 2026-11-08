#!/bin/bash
# HakiOS's UEFI: mu_aloha_platforms at the commit in BASE, with HakiOS's changes on top, and the platform of each device
# that has one (devices/<name>/uefi/files/). The result is mu_aloha_platforms/ at the top of the repository, which
# mkxnuimg.py and tools/update-prebuilt.sh take the image from (tools/mualoha.py says where in it).
#
#   uefi/apply.sh                clone mu_aloha_platforms and change it
#   uefi/apply.sh build          ... then build the UEFI of $MUALOHA_DEVICE (polaris) into Build/
#
# What HakiOS changes (hakios.patch, submodule.patch, files/, branding/):
#   BootShim.S       copies the "XNUP" container behind the UEFI image to 0xF0000000, where the loader finds the kernel
#   PlatformBootManagerLib (Common/MU, a submodule)
#                    starts the XNU loader from that container (XnuLoader.c) when BDS finds nothing to boot, instead of
#                    showing "No Operating System Found"
#   the vendor       the firmware says "HakiOS" (PcdFirmwareVendor, and so its SMBIOS tables)
#   the logo         BootLogo.bmp is HakiOS's, made by branding/banner.py
#
# A build needs a Linux x86_64 host (or the repository's Dockerfile): build_setup.sh, stuart and clang from there
# (uefi/README.md). Nothing here is redistributed: the image carries the phone vendor's own drivers.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(dirname "$HERE")
MU=${MUALOHA:-$ROOT/mu_aloha_platforms}
BASE=$(cat "$HERE/BASE")

if [ "${1:-}" = build ]; then
	# A Mac needs its own preparation (uefi/macos/README.md): tools, EDK2's tools, stuart's environment
	if [ "$(uname)" = Darwin ]; then exec "$HERE/macos/build.sh" "${@:2}"; fi
	DEV=${MUALOHA_DEVICE:-polaris}
	TARGET=$("$ROOT/tools/mualoha.py" -d "$DEV" target-device)
	[ -d "$MU" ] || { echo "uefi/apply.sh: $MU is not there: run uefi/apply.sh first"; exit 1; }
	( cd "$MU" && python3 ./build_uefi.py --init >/dev/null && python3 ./build_uefi.py -d "$TARGET" )
	shim=$("$ROOT/tools/mualoha.py" -d "$DEV" shim)
	fd=$("$ROOT/tools/mualoha.py" -d "$DEV" fd)
	echo "uefi/apply.sh: $fd"
	echo "uefi/apply.sh: $shim"
	exit 0
fi

[ -e "$MU" ] && { echo "uefi/apply.sh: $MU already exists (uefi/export.sh saves what is in it; move it away for a fresh one)"; exit 1; }
# MUALOHA_SHALLOW=1: no history (a Mac's first build does this anyway, uefi/macos/setup.sh)
git clone ${MUALOHA_SHALLOW:+--depth 1} https://github.com/Luoxueowo/mu_aloha_platforms.git "$MU"
git -C "$MU" checkout -q "$BASE"
# the one submodule HakiOS changes; build_uefi.py --init takes the rest
git -C "$MU" submodule update --init Common/MU
git -C "$MU" apply --whitespace=nowarn "$HERE/hakios.patch"
cp -r "$HERE/files/." "$MU/"
git -C "$MU/Common/MU" apply --whitespace=nowarn "$HERE/submodule.patch"
python3 "$HERE/branding/banner.py" "$MU/Platforms/AndromedaPkg/Include/Resources"
devices=("$@")
if [ ${#devices[@]} -eq 0 ]; then
	for d in "$ROOT"/devices/*/uefi/files; do [ -d "$d" ] && devices+=("$(basename "$(dirname "$(dirname "$d")")")"); done
fi
for d in "${devices[@]}"; do
	[ -d "$ROOT/devices/$d/uefi/files" ] || { echo "uefi/apply.sh: devices/$d has no uefi/files/" >&2; exit 1; }
	cp -r "$ROOT/devices/$d/uefi/files/." "$MU/"
	echo "uefi/apply.sh: the platform of $d"
done
echo "uefi/apply.sh: done; build with: uefi/apply.sh build (Linux; on a Mac run uefi/macos/setup.sh once first: uefi/README.md)"
