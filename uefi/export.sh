#!/bin/bash
# Save what HakiOS changed in mu_aloha_platforms back into uefi/: the patch of the tree's own files (hakios.patch), the
# patch of the submodule it changes (submodule.patch) and the files HakiOS adds (files/). Run it after changing the tree
# by hand; what is not saved this way is lost with the clone. The logo is not saved: branding/banner.py makes it.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(dirname "$HERE")
MU=${MUALOHA:-$ROOT/mu_aloha_platforms}
[ "$(git -C "$MU" rev-parse HEAD)" = "$(cat "$HERE/BASE")" ] || { echo "uefi/export.sh: $MU is not at BASE"; exit 1; }
LIB=MsCorePkg/Library/PlatformBootManagerLib

git -C "$MU" diff -- BootShim Platforms/AndromedaPkg/Andromeda.dsc.inc > "$HERE/hakios.patch"
git -C "$MU/Common/MU" diff -- "$LIB" > "$HERE/submodule.patch"

rm -rf "$HERE/files"
mkdir -p "$HERE/files/Common/MU/$LIB"
cp "$MU/Common/MU/$LIB"/XnuLoader.[ch] "$HERE/files/Common/MU/$LIB/"

for d in "$ROOT"/devices/*/uefi/export.sh; do [ -x "$d" ] && "$d"; done
echo "uefi/export.sh: patch $(wc -l < "$HERE/hakios.patch") + $(wc -l < "$HERE/submodule.patch") lines, $(find "$HERE/files" -type f | wc -l) files"
