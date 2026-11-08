#!/bin/bash
# Refresh prebuilt/ from what is built in this tree: the kernel, the loader, our user-space programs
# and the pieces of the phone's UEFI. With them somebody without the cross toolchain can still make the
# images and run the system (QEMU or the phone); see README.md.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
# KERNEL=... to take the kernel from a tree built elsewhere (xnu/apply.sh makes one anywhere)
K=${KERNEL:-hakios-kernel}
DEV=${MUALOHA_DEVICE:-polaris}
M="python3 tools/mualoha.py -d $DEV"
mkdir -p prebuilt/userland devices/$DEV/prebuilt
cp $K prebuilt/hakios-kernel
# The loader in prebuilt/ is built with the PLACEHOLDER identity (xnuloader/mkidentity.sh --placeholder: no Apple data), in a
# temporary copy so that the real xnuloader/identity.h of this tree, made from your own IPSW, is left alone and never copied
# here. It boots the open kernel (make qemu-open); to start Darwin, build the loader yourself (make loader).
if command -v lld-link >/dev/null && command -v clang >/dev/null; then
	T=$(mktemp -d)
	mkdir "$T/xnuloader"
	cp xnuloader/loader.c xnuloader/efi.h xnuloader/build.sh xnuloader/mkidentity.sh xnuloader/mkidentity.py xnuloader/im4p.py "$T/xnuloader/"
	( cd "$T/xnuloader" && PLACEHOLDER=1 ./mkidentity.sh >/dev/null && ./build.sh >/dev/null ) && cp "$T/xnuloader/loader.efi" prebuilt/loader.efi ||
		echo "update-prebuilt.sh: the placeholder loader did not build; prebuilt/loader.efi kept"
	rm -rf "$T"
else
	echo "update-prebuilt.sh: no lld-link or clang here; prebuilt/loader.efi kept"
fi
cp userland/hello prebuilt/userland/
for f in init child stage0 hello_dyn svc_test surf_test rwtouch mg_test cpu_test sound_test libhakios.dylib libhakios.sb.dylib \
	libhakios.media.dylib libhakios.app.dylib libhakios.tweaks.dylib HakiosAudio net_test; do
	cp userland/obj/$f prebuilt/userland/
done
# The phone's UEFI: the image mu_aloha_platforms built (with HakiOS's changes, uefi/), the boot shim that starts it,
# and the device tree and ramdisk the boot image carries. tools/mualoha.py says where each of them is in that tree.
if [ -f "$($M fd)" ]; then
	cp "$($M fd)" devices/$DEV/prebuilt/UEFI.fd
	cp "$($M shim)" devices/$DEV/prebuilt/BootShim.bin
	cp "$($M dtb)" devices/$DEV/prebuilt/device-tree.dtb
	cp "$($M ramdisk)" devices/$DEV/prebuilt/ramdisk
	cp "$($M mkbootimg)" devices/$DEV/prebuilt/mkbootimg.py
	# mkbootimg.py imports this one, next to it in the tree's PythonLibs/
	cp "$(dirname "$($M mkbootimg)")/generate_gki_certificate.py" devices/$DEV/prebuilt/ 2>/dev/null || true
	{
		echo "uefi $DEV"
		$M target | while IFS='=' read -r k v; do
			case $k in package|soc|base|size) printf '\t%s\t%s\n' "$k" "$v" ;; esac
		done
	} > devices/$DEV/prebuilt/uefi.conf
fi
{
	echo "Built $(date '+%Y-%m-%d %H:%M') from this tree."
	echo
	sha256sum prebuilt/hakios-kernel prebuilt/loader.efi prebuilt/userland/* devices/*/prebuilt/* 2>/dev/null
} > prebuilt/SHA256SUMS
ls -la prebuilt prebuilt/userland devices/$DEV/prebuilt
