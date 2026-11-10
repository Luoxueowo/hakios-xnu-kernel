#!/bin/bash
# Build and package a device boot image from a shell configuration file.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

usage() {
	cat <<'EOF'
Usage: tools/build-boot.sh [CONFIG]

Builds the configured kernel, loader and boot ramdisks, then packages an Android
boot image. The default configuration is configs/polaris-boot.conf. It does not
flash the phone.
EOF
}

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
	usage
	exit 0
fi
if (( $# > 1 )); then
	usage >&2
	exit 2
fi

CONFIG=${1:-configs/polaris-boot.conf}
[[ "$CONFIG" = /* ]] || CONFIG="$ROOT/$CONFIG"
[[ -r "$CONFIG" ]] || { echo "build-boot: cannot read config: $CONFIG" >&2; exit 1; }
# Config files in this repository are trusted shell assignments.
# shellcheck source=/dev/null
source "$CONFIG"

: "${DEVICE:?set DEVICE in the boot config}"
: "${KERNEL_DEFCONFIG:?set KERNEL_DEFCONFIG in the boot config}"
: "${KERNEL_IMAGE:?set KERNEL_IMAGE in the boot config}"
: "${LOADER_IMAGE:?set LOADER_IMAGE in the boot config}"
: "${RESIDENT_IMAGE:?set RESIDENT_IMAGE in the boot config}"
: "${RESIDENT_VOLUME:?set RESIDENT_VOLUME in the boot config}"
: "${RAMDISK_IMAGE:?set RAMDISK_IMAGE in the boot config}"
: "${RAMDISK_VOLUME:?set RAMDISK_VOLUME in the boot config}"
: "${BOOT_ARGS:?set BOOT_ARGS in the boot config}"
: "${OUTPUT_IMAGE:?set OUTPUT_IMAGE in the boot config}"
: "${MAX_IMAGE_BYTES:?set MAX_IMAGE_BYTES in the boot config}"

BUILD_JOBS=${BUILD_JOBS:-8}
BUILD_KERNEL=${BUILD_KERNEL:-1}
BUILD_LOADER=${BUILD_LOADER:-1}
BUILD_USERLAND=${BUILD_USERLAND:-1}
USERLAND_REST=${USERLAND_REST:-0}
USERLAND_CRYPTEXES=${USERLAND_CRYPTEXES:-flash}

check_switch() {
	case "$2" in
	0|1) ;;
	*) echo "build-boot: $1 must be 0 or 1 (got '$2')" >&2; exit 2 ;;
	esac
}
check_switch BUILD_KERNEL "$BUILD_KERNEL"
check_switch BUILD_LOADER "$BUILD_LOADER"
check_switch BUILD_USERLAND "$BUILD_USERLAND"
check_switch USERLAND_REST "$USERLAND_REST"

repo_path() {
	case "$1" in
	/*) printf '%s\n' "$1" ;;
	*) printf '%s/%s\n' "$ROOT" "$1" ;;
	esac
}

KERNEL_PATH=$(repo_path "$KERNEL_IMAGE")
LOADER_PATH=$(repo_path "$LOADER_IMAGE")
RESIDENT_PATH=$(repo_path "$RESIDENT_IMAGE")
RAMDISK_PATH=$(repo_path "$RAMDISK_IMAGE")
OUTPUT_PATH=$(repo_path "$OUTPUT_IMAGE")
PAYLOAD_PATH="${OUTPUT_PATH%.img}.payload"

if [[ "$BUILD_KERNEL" == 1 ]]; then
	echo "build-boot: configure $DEVICE ($KERNEL_DEFCONFIG)"
	make DEVICE="$DEVICE" "$KERNEL_DEFCONFIG"
	echo "build-boot: compile XNU"
	make DEVICE="$DEVICE" JOBS="$BUILD_JOBS" kernel
fi

if [[ "$BUILD_USERLAND" == 1 ]]; then
	echo "build-boot: build flash-root ramdisks"
	if [[ "$USERLAND_REST" == 1 ]]; then
		(cd "$ROOT/userland" && FLASHBOOT=1 REST=1 CRYPTEXES="$USERLAND_CRYPTEXES" ./build.sh)
	else
		(cd "$ROOT/userland" && FLASHBOOT=1 CRYPTEXES="$USERLAND_CRYPTEXES" ./build.sh)
	fi
fi

if [[ "$BUILD_LOADER" == 1 ]]; then
	echo "build-boot: compile UEFI loader"
	make DEVICE="$DEVICE" loader
fi

for required in "$KERNEL_PATH" "$LOADER_PATH" "$RESIDENT_PATH" "$RAMDISK_PATH"; do
	[[ -s "$required" ]] || { echo "build-boot: required input missing or empty: $required" >&2; exit 1; }
done

echo "build-boot: check resident /sbin/launchd"
python3 tools/apfs/apfs.py stat "$RESIDENT_PATH" /sbin/launchd -v "$RESIDENT_VOLUME" >/dev/null
echo "build-boot: check ramdisk /sbin/init"
python3 tools/apfs/apfs.py stat "$RAMDISK_PATH" /sbin/init -v "$RAMDISK_VOLUME" >/dev/null

mkdir -p "$(dirname "$OUTPUT_PATH")"
echo "build-boot: create payload"
python3 xnuloader/mkpayload.py -o "$PAYLOAD_PATH" \
	"loader.efi=$LOADER_PATH" \
	"kernel=$KERNEL_PATH" \
	"resident=$RESIDENT_PATH" \
	"ramdisk=$RAMDISK_PATH" \
	--cmdline "$BOOT_ARGS"

# Guard the resident/md0 contract used by direct boots. A legacy `bootdisk=`
# entry is ignored by this loader and leaves root.apfs as md0.
python3 - "$PAYLOAD_PATH" "$BOOT_ARGS" <<'PY'
import struct
import sys

path, wanted_cmdline = sys.argv[1:]
data = open(path, "rb").read()
if len(data) < 32 or data[:4] != b"XNUP":
    raise SystemExit("build-boot: malformed XNUP payload")

entries = {}
offset = 8
while offset + 24 <= len(data):
    raw_name, size, _reserved = struct.unpack_from("<16sII", data, offset)
    offset += 24
    name = raw_name.split(b"\0", 1)[0].decode("ascii", "strict")
    if not name:
        break
    end = offset + size
    if end > len(data):
        raise SystemExit("build-boot: truncated payload entry " + name)
    entries[name] = data[offset:end]
    offset = end + (-size % 16)

required = {"loader.efi", "kernel", "resident", "ramdisk", "cmdline"}
missing = sorted(required - entries.keys())
if missing:
    raise SystemExit("build-boot: payload missing entries: " + ", ".join(missing))
if "bootdisk" in entries:
    raise SystemExit("build-boot: `bootdisk=` is not supported; use `resident=` for md0")
cmdline = entries["cmdline"].split(b"\0", 1)[0].decode("utf-8")
if cmdline != wanted_cmdline:
    raise SystemExit("build-boot: payload command line differs from BOOT_ARGS")
print("build-boot: payload verified (resident=md0, ramdisk=md1)")
PY

echo "build-boot: package Android boot image"
python3 mkxnuimg.py -d "$DEVICE" -p "$PAYLOAD_PATH" -o "$OUTPUT_PATH"

IMAGE_BYTES=$(python3 - "$OUTPUT_PATH" <<'PY'
import os
import sys
print(os.path.getsize(sys.argv[1]))
PY
)
if (( IMAGE_BYTES > MAX_IMAGE_BYTES )); then
	echo "build-boot: image is $IMAGE_BYTES bytes; configured boot partition limit is $MAX_IMAGE_BYTES" >&2
	exit 1
fi

echo "build-boot: ready: $OUTPUT_PATH ($IMAGE_BYTES / $MAX_IMAGE_BYTES bytes)"
echo "build-boot: boot args: $BOOT_ARGS"
