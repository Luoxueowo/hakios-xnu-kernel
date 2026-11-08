#!/bin/bash
# The environment a macOS build of the UEFI needs, sourced by setup.sh and build.sh (and useful by hand):
#
#   source uefi/macos/env.sh
#
# mu_aloha_platforms is written for Ubuntu 24.04: build_setup.sh installs apt packages, and the tree expects
# aarch64-linux-gnu binutils and mu_basecore's prebuilt BaseTools for Linux. On a Mac the same tools come from
# Xcode's clang plus a standalone LLVM (lld-link, the linker CLANGPDB uses) and BaseTools built here from source
# (uefi/macos/setup.sh); these variables are what EDK2's tools_def.template reads.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export MUALOHA=${MUALOHA:-$ROOT/mu_aloha_platforms}
export MUALOHA_DEVICE=${MUALOHA_DEVICE:-polaris}
export MUALOHA_TARGET=$(python3 "$ROOT/tools/mualoha.py" -d "$MUALOHA_DEVICE" target-device 2>/dev/null || echo "")
TOOLS=$ROOT/.tools
EDK=$MUALOHA/MU_BASECORE/BaseTools

export PATH=$ROOT/.venv/bin:$TOOLS/llvm/bin:$EDK/BinWrappers/PosixLike:$PATH
# tools_def.template: DEFINE CLANGPDB_AARCH64_PREFIX = ENV(CLANG_BIN), so the tools are found by that prefix
export CLANG_BIN=$TOOLS/llvm/bin/
export CLANGPDB_BIN=$CLANG_BIN
export CLANGPDB_AARCH64_PREFIX=$CLANG_BIN
export PYTHON_COMMAND=python3
export EDK_TOOLS_PATH=$EDK
# EDK2's own tools are the ones built here (setup.sh puts them where mu_basecore's ext dep looks for them)
export EDK_TOOLS_BIN=$EDK/Source/C/bin
export PYTHONPATH=$EDK/Source/Python${PYTHONPATH:+:$PYTHONPATH}
