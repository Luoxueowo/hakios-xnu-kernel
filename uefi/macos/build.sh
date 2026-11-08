#!/bin/bash
# Build the UEFI of $MUALOHA_DEVICE (polaris) on a Mac: what `cd mu_aloha_platforms && ./build_uefi.py -d xiaomi-polaris`
# does on Linux, with the environment from uefi/macos/env.sh. Run uefi/macos/setup.sh once first.
#
#   uefi/macos/build.sh                     a RELEASE build, the one the boot image carries
#   uefi/macos/build.sh -s 1                ... with secure boot on (a second FV, SDM845_EFI_SB.fd)
#   MUALOHA_DEVICE=qrd845 uefi/macos/build.sh
#
# It prints the FD and the boot shim, the two things mkxnuimg.py packs (or ./mkxnuimg.py does it itself).
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=env.sh
. "$HERE/env.sh"

[ "$(uname)" = Darwin ] || { echo "uefi/macos/build.sh: this is for macOS: on Linux, uefi/apply.sh build" >&2; exit 1; }
[ -x "$ROOT/.venv/bin/python3" ] || { echo "uefi/macos/build.sh: run uefi/macos/setup.sh first" >&2; exit 1; }
[ -n "$MUALOHA_TARGET" ] || { echo "uefi/macos/build.sh: $MUALOHA has no device $MUALOHA_DEVICE (uefi/README.md)" >&2; exit 1; }

cd "$MUALOHA"
python3 ./build_uefi.py -d "$MUALOHA_TARGET" "$@"

python3 "$ROOT/tools/mualoha.py" -d "$MUALOHA_DEVICE" built ||
	{ echo "uefi/macos/build.sh: the build finished without its FD (the log says why)" >&2; exit 1; }
echo "uefi/macos/build.sh: $(python3 "$ROOT/tools/mualoha.py" -d "$MUALOHA_DEVICE" fd)"
echo "uefi/macos/build.sh: $(python3 "$ROOT/tools/mualoha.py" -d "$MUALOHA_DEVICE" shim)"
