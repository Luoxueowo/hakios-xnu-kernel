#!/bin/bash
# Pull the MIX 2S A630 firmware from its own vendor partition into ignored fw-gpu/.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ADB=${ADB:-adb}
MOUNTPOINT=/mnt/hakios-vendor
DEST="$ROOT/fw-gpu"
FILES=(a630_gmu.bin a630_sqe.fw a630_zap.mdt a630_zap.b00 a630_zap.b01 a630_zap.b02 a630_zap.elf)

adb() {
	if [[ -n "${ANDROID_SERIAL:-}" ]]; then
		command "$ADB" -s "$ANDROID_SERIAL" "$@"
	else
		command "$ADB" "$@"
	fi
}

state=$(adb get-state 2>/dev/null || true)
state=${state//$'\r'/}
case "$state" in
device|recovery) ;;
*) echo "pull-firmware: need one adb device in Recovery (state: ${state:-none})" >&2; exit 1 ;;
esac
VENDOR_DEVICE=$(adb shell 'readlink -f /dev/block/by-name/vendor' 2>/dev/null | tr -d '\r' | tail -n 1)
[[ "$VENDOR_DEVICE" == /dev/block/* ]] || { echo "pull-firmware: cannot resolve /dev/block/by-name/vendor" >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/polaris-gpu-fw.XXXXXX")
mounted_here=0
created_mountpoint=0
cleanup() {
	if (( mounted_here )); then
		adb shell umount "$MOUNTPOINT" >/dev/null 2>&1 || echo "pull-firmware: could not unmount temporary vendor mount $MOUNTPOINT" >&2
	fi
	if (( created_mountpoint )); then
		adb shell rmdir "$MOUNTPOINT" >/dev/null 2>&1 || true
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mount_record() {
	local path=$1
	adb shell "awk -v m='$path' '\$2 == m { print \$1 \":\" \$3 \":\" \$4 }' /proc/mounts" 2>/dev/null |
	    tr -d '\r' | tail -n 1
}

require_read_only() {
	local path=$1 record=$2 source filesystem options
	[[ -n "$record" ]] || { echo "pull-firmware: $path is not mounted" >&2; return 1; }
	source=${record%%:*}
	record=${record#*:}
	filesystem=${record%%:*}
	options=${record#*:}
	if [[ "$source" != "$VENDOR_DEVICE" || "$filesystem" != ext4 ]]; then
		echo "pull-firmware: refusing unexpected mount at $path ($source, $filesystem)" >&2
		return 1
	fi
	case ",$options," in
	*,ro,*) ;;
	*) echo "pull-firmware: refusing to read $path from a read-write mount ($record)" >&2; return 1 ;;
	esac
}

vendor_record=$(mount_record /vendor)
if [[ -n "$vendor_record" ]]; then
	require_read_only /vendor "$vendor_record"
	FIRMWARE=/vendor/firmware
else
	vendor_record=$(mount_record "$MOUNTPOINT")
	if [[ -n "$vendor_record" ]]; then
		require_read_only "$MOUNTPOINT" "$vendor_record"
	else
		if ! adb shell 'test -b /dev/block/by-name/vendor' >/dev/null 2>&1; then
			echo "pull-firmware: vendor is not mounted and /dev/block/by-name/vendor is absent" >&2
			exit 1
		fi
		if ! adb shell "test -d '$MOUNTPOINT'" >/dev/null 2>&1; then
			created_mountpoint=1
		fi
		adb shell "mkdir -p '$MOUNTPOINT' && mount -t ext4 -o ro,noload /dev/block/by-name/vendor '$MOUNTPOINT'" || {
			echo "pull-firmware: could not mount vendor read-only with journal replay disabled" >&2
			exit 1
		}
		mounted_here=1
		vendor_record=$(mount_record "$MOUNTPOINT")
		require_read_only "$MOUNTPOINT" "$vendor_record"
	fi
	FIRMWARE="$MOUNTPOINT/firmware"
fi

for file in "${FILES[@]}"; do
	remote="$FIRMWARE/$file"
	if ! adb shell "test -r '$remote' && test -s '$remote'" >/dev/null 2>&1; then
		echo "pull-firmware: missing or unreadable vendor firmware: $remote" >&2
		exit 1
	fi
	adb pull "$remote" "$tmp/$file" >/dev/null
	[[ -s "$tmp/$file" ]] || { echo "pull-firmware: adb pulled an empty file: $file" >&2; exit 1; }
	remote_hash=$(adb shell "sha256sum '$remote'" 2>/dev/null | tr -d '\r' | awk '{ print $1; exit }')
	local_hash=$(shasum -a 256 "$tmp/$file" | awk '{ print $1 }')
	if [[ ! "$remote_hash" =~ ^[[:xdigit:]]{64}$ || "$remote_hash" != "$local_hash" ]]; then
		echo "pull-firmware: SHA-256 mismatch for $remote (device=$remote_hash local=$local_hash)" >&2
		exit 1
	fi
done

mkdir -p "$DEST"
for file in "${FILES[@]}"; do
	cp -p "$tmp/$file" "$DEST/$file"
done
(
	cd "$DEST"
	shasum -a 256 "${FILES[@]}" > SHA256SUMS
)
echo "pull-firmware: staged ${#FILES[@]} A630 files in fw-gpu/ (ignored by Git)"
