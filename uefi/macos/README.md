# The UEFI, built on a Mac

`mu_aloha_platforms` is written for Ubuntu 24.04: `build_setup.sh` installs apt packages, its BaseTools come from a
release that carries Linux and Windows binaries only, and EDK2 links AArch64 firmware with `lld-link`, which Xcode
does not ship. This directory does the same work with what a Mac has (Xcode, `make`, `curl`) and installs the rest
inside the repository:

```sh
uefi/apply.sh                  # clone mu_aloha_platforms at BASE and change it (uefi/README.md)
uefi/macos/setup.sh            # once: Python 3.12, LLVM, EDK2's tools, stuart's environment
uefi/macos/build.sh            # the FD, and the boot shim: what mkxnuimg.py packs
./mkxnuimg.py -d polaris       # -> xnu-polaris.img, from that FD (docs/uefi-boot.md)
```

`uefi/apply.sh build` runs the last one on a Mac by itself. Nothing is installed system-wide, and nothing needs sudo
or a package manager.

## What it brings, and why each thing is needed

| Where | What | The failure it prevents |
|---|---|---|
| `.tools/uv` | uv, a standalone installer | Xcode's `python3` is 3.9; `edk2-pytool-extensions` wants 3.10+ |
| `.tools/python`, `.venv` | Python 3.12 and `pip-requirements.txt` | `stuart` and `edk2-pytool-*`, which the build runs |
| `.tools/llvm` | LLVM's own macOS build: `clang`, `lld-link`, `llvm-objcopy`, `llvm-lib`, `llvm-rc` | Xcode's clang cannot link the firmware: CLANGPDB links with `lld-link`, and Xcode ships no lld at all |
| `MU_BASECORE/BaseTools/Source/C/bin` | EDK2's tools (`GenFw`, `GenSec`, `VfrCompile`, ...), built here | the tree expects `Mu-Basetools`, a release that carries `Linux-x86` and `Windows-x86` only: there is nothing on a Mac to run |
| `.../BaseTools/Bin/Mu-Basetools_extdep/<host>/` | those tools, where that release's ext dep looks for them | `Mu-Basetools is host specific, but does not appear to have support for Host(os='MacOs' ...)` |
| `.../BaseTools/Bin/{iasl,mu_nasm}_extdep/` | a note, not a tool | `Mono is required to run NuGet on non-Windows platforms` |

The ext dep entries for `iasl` and `nasm` are declared present and unusable on purpose: their packages hold Linux and
Windows binaries, `iasl` compiles `.asl` sources and `nasm` assembles x86 code, and an AARCH64 build has neither. If
one is ever needed the build stops with a sentence naming it, instead of failing on a download.

`setup.sh` reads the versions it declares out of the tree's own `iasl_ext_dep.yaml` and `nasm_ext_dep.yaml`, so a
`mu_aloha_platforms` that moves them keeps working.

## Measured, on an Intel Mac (i7-10750H, macOS 15.8, Xcode 21's clang)

| | |
|---|---|
| `uefi/macos/setup.sh` the first time | about 7 minutes (1.5 GB of LLVM, 1.5 GB of submodules) |
| `uefi/macos/build.sh` from nothing | **6m28s**, 3600+ files, "0116 Images Verified" |
| `uefi/macos/build.sh` after an edit | 49s |
| what comes out | `Build/NapaliPkg/RELEASE_CLANGPDB/FV/SDM845_EFI.fd`, 3,145,728 bytes (`UEFI_SIZE` in `build_cfg/sdm845.json`) |
| disk it takes | about 270 MB of LLVM after the tools are pruned, ~1.5 GB of tree and submodules, plus `Build/` |

The FD's size is not a coincidence: the boot shim copies exactly `UEFI_SIZE` bytes, and `mkxnuimg.py` refuses an FD of
another size.

## Limits

- **`iasl` and `nasm` are placeholders.** Nothing in an AARCH64 build of `NapaliPkg` uses them; a build that does will
  say so. If one is ever needed, build ACPICA's `iasl` from source (it compiles with `make` on macOS) and drop it into
  `BaseTools/Bin/edk2-acpica-iasl_extdep/<host>/iasl`.
- **Nothing was changed in `mu_aloha_platforms` or in `edk2-pytool-extensions`.** Everything here is environment and
  layout, so a fresh clone plus `uefi/macos/setup.sh` is the whole story; `uefi/export.sh` is unaffected.
- **Secure boot builds (`-s 1`) are untested here**: they add `SDM845_EFI_SB.fd` and sign it, which needs keys nobody
  has on a Mac; the boot image uses the non-SB FD.
- **The host directory name comes from `edk2toollib`**, so it is `MacOs-x86-64` on an Intel Mac and `MacOs-ARM-64`-like
  on Apple silicon; `setup.sh` asks the library rather than assuming. The LLVM release it downloads is pinned
  (19.1.7), which has both `macOS-X64` and `macOS-ARM64`.

## Starting over

```sh
rm -rf .tools .venv mu_aloha_platforms
uefi/apply.sh && uefi/macos/setup.sh
```

To rebuild only the firmware: `rm -rf mu_aloha_platforms/Build` then `uefi/macos/build.sh` (EDK2's tools stay).
`uefi/macos/setup.sh` is safe to run again: it checks before it installs anything.
