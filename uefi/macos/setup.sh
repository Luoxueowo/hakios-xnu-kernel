#!/bin/bash
# What mu_aloha_platforms' build_setup.sh does on Ubuntu 24.04, done on a Mac with nothing but Xcode:
#
#   uefi/macos/setup.sh            ... one time, then uefi/macos/build.sh (or uefi/apply.sh build)
#
# It installs into this repository, nothing system-wide (no Homebrew, no sudo):
#   .tools/uv              a standalone uv, which brings Python 3.12 (the tree wants 3.10+; Xcode's is 3.9)
#   .venv                  stuart, edk2-pytool-* and the rest of pip-requirements.txt
#   .tools/llvm            LLVM's own macOS build: clang, and the lld-link CLANGPDB links with (Xcode has no lld)
#   MU_BASECORE/BaseTools  EDK2's tools, built here from source -- Mu's own ext dep ships Linux and Windows
#                          binaries only ("Mu-Basetools is host specific, but does not appear to have support
#                          for Host(os='MacOs' ...)"); the built ones go where that ext dep looks for them
#
# The two tools nothing on a Mac provides are declared to the tree's ext dep system as present but unusable, so
# that a build that ever needs them says so instead of failing on a download: iasl (ACPICA, for .asl sources) and
# nasm (x86 assembly). An AARCH64 build uses neither.
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOLS=$ROOT/.tools
LLVM_VERSION=19.1.7
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

say() { echo "uefi/macos/setup.sh: $*"; }
die() { echo "uefi/macos/setup.sh: $*" >&2; exit 1; }

[ "$(uname)" = Darwin ] || die 'this is for macOS: on Linux the tree builds build_setup.sh as it is'
[ -d "${MUALOHA:-$ROOT/mu_aloha_platforms}" ] ||
	die 'mu_aloha_platforms is not there yet: run uefi/apply.sh (uefi/README.md)'
mkdir -p "$TOOLS"

# --- Python 3.12 and the build's own dependencies -------------------------------------------------------------
if [ ! -x "$TOOLS/uv" ]; then
	case "$(uname -m)" in
	arm64) uv_arch=aarch64-apple-darwin ;;
	*)     uv_arch=x86_64-apple-darwin ;;
	esac
	say "uv ($uv_arch)"
	curl -sfL --retry 2 -o "$TOOLS/uv.tar.gz" "https://github.com/astral-sh/uv/releases/latest/download/uv-$uv_arch.tar.gz"
	tar -xf "$TOOLS/uv.tar.gz" -C "$TOOLS" --strip-components=1
	rm -f "$TOOLS/uv.tar.gz"
fi
if [ ! -x "$ROOT/.venv/bin/python3" ]; then
	UV_PYTHON_INSTALL_DIR=$TOOLS/python "$TOOLS/uv" python install 3.12
	"$TOOLS/uv" venv --python 3.12 "$ROOT/.venv"
fi
"$TOOLS/uv" pip install --quiet --python "$ROOT/.venv/bin/python3" \
	-r "${MUALOHA:-$ROOT/mu_aloha_platforms}/pip-requirements.txt" -c "$HERE/constraints.txt"

# --- LLVM: clang, lld-link, llvm-objcopy, llvm-lib, llvm-rc ---------------------------------------------------
if [ ! -x "$TOOLS/llvm/bin/lld-link" ]; then
	case "$(uname -m)" in
	arm64) llvm_arch=ARM64 ;;
	*)     llvm_arch=X64 ;;
	esac
	name=LLVM-$LLVM_VERSION-macOS-$llvm_arch
	say "$name (1.5 GB: clang and lld, which Xcode does not ship)"
	curl -fL --retry 2 -o "$TOOLS/llvm.tar.xz" \
		"https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$name.tar.xz"
	mkdir -p "$TOOLS/llvm"
	tar -xf "$TOOLS/llvm.tar.xz" -C "$TOOLS/llvm" --strip-components=1 "$name/bin/*" "$name/lib/clang/*"
	rm -f "$TOOLS/llvm.tar.xz"
	# the archive carries every LLVM tool; keep what a firmware build asks for
	for f in "$TOOLS"/llvm/bin/*; do
		case "$(basename "$f")" in
		clang|clang++|lld|lld-link|llvm-objcopy|llvm-lib|llvm-rc|llvm-ar|llvm-nm|llvm-objdump|llvm-readobj|\
		llvm-size|llvm-strip|llvm-dlltool|llvm-ranlib|llvm-cxxfilt|llvm-config) ;;
		*) rm -f "$f" ;;
		esac
	done
	rm -rf "$TOOLS"/llvm/lib/clang/*/lib
fi

# --- the tree's submodules ------------------------------------------------------------------------------------
(say "submodules"; cd "${MUALOHA:-$ROOT/mu_aloha_platforms}" && git submodule update --init --depth 1 --quiet) 2>/dev/null ||
	say 'submodules: some did not come (offline?); the build says which it needs'

# --- EDK2's tools, built from source --------------------------------------------------------------------------
EDK=${MUALOHA:-$ROOT/mu_aloha_platforms}/MU_BASECORE/BaseTools
if [ ! -x "$EDK/Source/C/bin/GenFw" ]; then
	say "BaseTools (make -C MU_BASECORE/BaseTools -j$NPROC)"
	make -C "$EDK" -j"$NPROC" >"$TOOLS/basetools.log" 2>&1 || {
		tail -20 "$TOOLS/basetools.log" >&2
		die "BaseTools did not build (the whole log: $TOOLS/basetools.log)"
	}
fi

# --- what the tree's ext dep system looks for -----------------------------------------------------------------
# Mu's BaseTools ext dep is "host specific": it looks for a directory named after the host inside its contents.
HOST=$("$ROOT/.venv/bin/python3" -c 'from edk2toollib.utility_functions import GetHostInfo as g; h=g(); print("-".join((h.os, h.arch, h.bit)))')
if [ ! -x "$EDK/Bin/Mu-Basetools_extdep/$HOST/GenFw" ]; then
	say "BaseTools as the ext dep expects them ($HOST)"
	mkdir -p "$EDK/Bin/Mu-Basetools_extdep/$HOST"
	cp "$EDK"/Source/C/bin/* "$EDK/Bin/Mu-Basetools_extdep/$HOST/"
fi
# iasl and nasm: the packages hold Linux and Windows binaries only. Declared, so stuart_update is happy, and
# unusable, so that a build which really needs one stops with a sentence rather than a 404.
for spec in "$EDK/Bin/iasl_ext_dep.yaml:iasl:edk2-acpica-iasl_extdep" \
            "$EDK/Bin/nasm_ext_dep.yaml:nasm:mu_nasm_extdep"; do
	yaml=${spec%%:*}; rest=${spec#*:}; tool=${rest%%:*}; dir=${rest#*:}
	[ -f "$yaml" ] || continue
	# the descriptor is JSON behind ## comment lines, so the version is read out of it rather than parsed
	version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$yaml" | head -1)
	[ -x "$EDK/Bin/$dir/$HOST/$tool" ] && continue
	say "$tool $version: declared (no macOS build exists; nothing in an AARCH64 build uses it)"
	mkdir -p "$EDK/Bin/$dir/$HOST"
	printf '#!/bin/sh\necho "%s is not available on macOS; nothing in an AARCH64 build uses it" >&2\nexit 1\n' "$tool" \
		> "$EDK/Bin/$dir/$HOST/$tool"
	chmod +x "$EDK/Bin/$dir/$HOST/$tool"
	printf 'version: "%s"\n' "$version" > "$EDK/Bin/$dir/extdep_state.yaml"
done

# --- stuart's own setup/update --------------------------------------------------------------------------------
# shellcheck source=env.sh
. "$HERE/env.sh"
say 'stuart setup and update'
( cd "$MUALOHA" && python3 ./build_uefi.py --init >"$TOOLS/stuart-init.log" 2>&1 ) || {
	tail -20 "$TOOLS/stuart-init.log" >&2
	die "stuart did not finish (the whole log: $TOOLS/stuart-init.log)"
}
say "ready: uefi/macos/build.sh (device $MUALOHA_DEVICE, target $MUALOHA_TARGET)"
