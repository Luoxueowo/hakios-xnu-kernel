#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
# the iPhone's identity for XNU, from the device tree of the builder's own IPSW (not in the repository)
[ -f identity.h ] || ./mkidentity.sh
# the serial number and the UDID's inputs: the device's own file (devices/polaris/identity.conf), which
# is compiled in as identity_conf.h. IDENTITY_CONF names another one; without any, mkidentityconf.py's
# defaults are used, so that a device with no such file still builds.
CONF=${IDENTITY_CONF:-../devices/${DEVICE:-polaris}/identity.conf}
if [ -f "$CONF" ]; then
	./mkidentityconf.py "$CONF" > identity_conf.h.new
	mv identity_conf.h.new identity_conf.h
else
	echo "xnuloader/build.sh: no $CONF; the serial number and the UDID are mkidentityconf.py's defaults" >&2
	./mkidentityconf.py > identity_conf.h
fi

# The Android tree is the hardware description for Polaris. The loader embeds only the DTB's
# /soc and /reserved-memory subtrees in its Apple-format tree; other device profiles do not need it.
BUILD_DEVICE=${DEVICE:-polaris}
POLARIS_DTB=../mu_aloha_platforms/Platforms/NapaliPkg/Device/xiaomi-polaris/DeviceTreeBlob/Android/android-polaris.dtb
if [ "$BUILD_DEVICE" = polaris ]; then
	if [ ! -f "$POLARIS_DTB" ]; then
		echo "xnuloader/build.sh: Polaris Android DTB is missing: $POLARIS_DTB" >&2
		exit 1
	fi
	python3 ./mkpolarisdtb.py "$POLARIS_DTB" > polaris_android_dtb.h.new
	mv polaris_android_dtb.h.new polaris_android_dtb.h
fi

DTB_CFLAGS=
if [ "$BUILD_DEVICE" = polaris ]; then
	DTB_CFLAGS=-DHAVE_POLARIS_ANDROID_DTB
fi
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter $DTB_CFLAGS -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
