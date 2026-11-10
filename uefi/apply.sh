#!/bin/bash
# HakiOS's UEFI: mu_aloha_platforms (a submodule of this repository, pinned by uefi/BASE) with HakiOS's
# changes on top, and the platform of each device that has one (devices/<name>/uefi/files/). The result
# is mu_aloha_platforms/ at the top of the repository, which mkxnuimg.py and tools/update-prebuilt.sh take
# the image from (tools/mualoha.py says where in it).
#
#   uefi/apply.sh                the tree, patched and ready to build -- run it as often as you like
#   uefi/apply.sh build          ... then build the UEFI of $MUALOHA_DEVICE (polaris) into Build/
#
# Every step is idempotent: the patches are applied only where they are not in yet, so a second run says
# "already applied" instead of failing, and a build after changing one of them needs no hand work.
#
# What HakiOS changes (hakios.patch, submodule.patch, files/, branding/):
#   BootShim.S       copies the "XNUP" container behind the UEFI image to 0xF0000000, where the loader finds the kernel
#   PlatformBootManagerLib (Common/MU, a submodule)
#                    starts the XNU loader from that container (XnuLoader.c) at the top of
#                    PlatformBootManagerPriorityBoot -- before BDS looks for a boot option, because the front page is
#                    one itself -- and again in UnableToBoot, for a payload without a loader
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

# apply_patch DIR PATCH: the patch, unless it is in already (then nothing, whatever a hand edit left behind)
apply_patch()
{
	local dir=$1 patch=$2

	if git -C "$dir" apply --check --whitespace=nowarn "$patch" 2>/dev/null; then
		git -C "$dir" apply --whitespace=nowarn "$patch"
	elif git -C "$dir" apply --check --reverse --whitespace=nowarn "$patch" 2>/dev/null; then
		echo "uefi/apply.sh: $(basename "$patch") is in already"
	else
		echo "uefi/apply.sh: $(basename "$patch") does not apply to $dir:" >&2
		echo "uefi/apply.sh: the tree is not at $BASE, or something edited it by hand (git -C $dir status)" >&2
		exit 1
	fi
}

# --- the tree, at the commit uefi/BASE pins ---------------------------------------------------------
if [ ! -e "$MU/.git" ]; then
	if [ "$MU" = "$ROOT/mu_aloha_platforms" ] && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1 &&
	    git -C "$ROOT" ls-files --error-unmatch mu_aloha_platforms >/dev/null 2>&1; then
		echo "uefi/apply.sh: the submodule"
		git -C "$ROOT" submodule update --init mu_aloha_platforms
	fi
fi
if [ ! -e "$MU/.git" ]; then
	# no submodule here (an older clone, or MUALOHA pointing elsewhere): make the tree where it is wanted
	git clone ${MUALOHA_SHALLOW:+--depth 1} https://github.com/Luoxueowo/mu_aloha_platforms.git "$MU"
fi
if ! git -C "$MU" cat-file -e "$BASE^{commit}" 2>/dev/null; then
	git -C "$MU" fetch --depth 1 origin "$BASE"      # a shallow clone does not carry every commit
fi
git -C "$MU" checkout -q "$BASE"
# the one submodule HakiOS changes; build_uefi.py --init takes the rest
git -C "$MU" submodule update --init Common/MU

# --- HakiOS's changes -------------------------------------------------------------------------------
apply_patch "$MU" "$HERE/hakios.patch"
cp -r "$HERE/files/." "$MU/"
apply_patch "$MU/Common/MU" "$HERE/submodule.patch"
python3 "$HERE/branding/banner.py" "$MU/Platforms/AndromedaPkg/Include/Resources"
if [ "${1:-}" = build ]; then devices=(); else devices=("$@"); fi
if [ ${#devices[@]} -eq 0 ]; then
	for d in "$ROOT"/devices/*/uefi/files; do [ -d "$d" ] && devices+=("$(basename "$(dirname "$(dirname "$d")")")"); done
fi
for d in "${devices[@]}"; do
	[ -n "$d" ] || continue
	[ -d "$ROOT/devices/$d/uefi/files" ] || { echo "uefi/apply.sh: devices/$d has no uefi/files/" >&2; exit 1; }
	cp -r "$ROOT/devices/$d/uefi/files/." "$MU/"
	echo "uefi/apply.sh: the platform of $d"
done

if [ "${1:-}" = build ]; then
	# A Mac needs its own preparation (uefi/macos/README.md): tools, EDK2's tools, stuart's environment
	if [ "$(uname)" = Darwin ]; then exec "$HERE/macos/build.sh" "${@:2}"; fi
	DEV=${MUALOHA_DEVICE:-polaris}
	TARGET=$("$ROOT/tools/mualoha.py" -d "$DEV" target-device)
	( cd "$MU" && python3 ./build_uefi.py --init >/dev/null && python3 ./build_uefi.py -d "$TARGET" )
	shim=$("$ROOT/tools/mualoha.py" -d "$DEV" shim)
	fd=$("$ROOT/tools/mualoha.py" -d "$DEV" fd)
	echo "uefi/apply.sh: $fd"
	echo "uefi/apply.sh: $shim"
	exit 0
fi
echo "uefi/apply.sh: done; build with: uefi/apply.sh build (Linux; on a Mac run uefi/macos/setup.sh once first: uefi/README.md)"
