# HakiOS UEFI

The UEFI firmware HakiOS phones start with: [mu_aloha_platforms](https://github.com/Luoxueowo/mu_aloha_platforms)
(Project Mu for Snapdragon phones, whose device packages are Project Aloha's) at a fixed commit, with HakiOS's changes
on top, and the platform of each phone from its device repository.

| What | Where |
|---|---|
| the mu_aloha_platforms commit it is made on | `BASE` (its `polaris-device-support` branch) |
| HakiOS's changes to the tree's own files | `hakios.patch` |
| HakiOS's changes to the submodule it changes (`Common/MU`) | `submodule.patch` |
| HakiOS's new files (the XNU loader in the boot manager) | `files/` |
| the picture it shows | `branding/banner.py` makes it |
| a phone's platform (memory map, drivers list, its device tree) | `devices/<name>/uefi/files/` in the device's repository |

## Where the pieces are

```
mu_aloha_platforms/                                  uefi/apply.sh makes it
├── BootShim/                                        starts the UEFI: our patch copies the payload on
├── Common/MU/                                       a submodule (microsoft/mu_plus): Mu's boot managers
│   └── MsCorePkg/Library/PlatformBootManagerLib/    ... and the one our XnuLoader.c is called from
├── Platforms/NapaliPkg/                             the SDM845 package (polaris is its device)
│   ├── Device/xiaomi-polaris/                       the MIX 2S: drivers, memory map, android-polaris.dtb
│   └── Napali.dsc                                   what the platform builds
├── PythonLibs/                                      mkbootimg.py, emptyramdisk
└── Build/NapaliPkg/RELEASE_CLANGPDB/FV/SDM845_EFI.fd    what comes out (build_uefi.py)
```

`tools/mualoha.py` reads that tree: the phone's folder gives the package, `build_cfg/sdm845.json` gives the shim's
base and size (`0x9FC00000`, `0x300000`) and the SoC that names the FD, and the device folder gives the device tree. Nothing about the firmware is written down twice, so `mkxnuimg.py`, `xl` and `tools/update-prebuilt.sh` agree.

## What HakiOS changes

| Part | Change |
|---|---|
| `BootShim/BootShim.S` | copies the `XNUP` container behind the UEFI image to `0xF0000000`, where the loader finds the kernel |
| `Common/MU/.../PlatformBootManagerLib/MsPlatform.c` | when BDS finds nothing to boot, hands over to the XNU loader (`XnuLoader.c`) instead of "No Operating System Found" |
| `Platforms/AndromedaPkg/Andromeda.dsc.inc` | `PcdFirmwareVendor` is `HakiOS` (what the firmware and its SMBIOS tables say they are) |
| `Platforms/AndromedaPkg/Include/Resources/BootLogo.bmp` | the site's mark, white on black, in place of Project Aloha's |

`XnuLoader.c` runs when BDS has nothing to boot. It looks in the payload for `loader.efi` and starts it straight from
memory, so the loader is iterated on without rebuilding the firmware. When there is none, it writes what the loader
will need -- exception level, CPU feature registers, framebuffer, memory map -- as text to the raw `logdump`
partition, and cold-resets: the phone comes back to Android by itself.

The tree's own code keeps its licence and its authors' notices; this is a fork of it in the open, as its licences
allow. `NOTICE` and `THIRD_PARTY_LICENSES.md` say which parts came from where.

## Build it

The tree is written for Ubuntu 24.04 (`build_setup.sh` installs packages with apt):

```sh
uefi/apply.sh                                   # clone mu_aloha_platforms at BASE and change it
cd mu_aloha_platforms
./build_setup.sh
pip install --upgrade -r pip-requirements.txt
./build_uefi.py --init
./build_uefi.py -d xiaomi-polaris               # secure boot off (-s 0, the default)
```

**On a Mac** it builds too, with nothing installed system-wide (no Homebrew, no sudo, no Docker): `uefi/macos/setup.sh`
once brings Python 3.12 and LLVM's clang with `lld-link` (Xcode has none), builds EDK2's tools from source, and gives
the tree's ext dep system paths where a Mac has no binaries; then `uefi/macos/build.sh` builds the same FD
([macOS](macos/README.md), measured: 6m28s the first time, 49s after a change).

`uefi/apply.sh build` chooses between the two: `uefi/macos/build.sh` on a Mac, the last three lines above elsewhere,
for `$MUALOHA_DEVICE` (polaris). Either way it also prints the boot shim, and both paths are what `./mkxnuimg.py`
packs. What it leaves is `Build/NapaliPkg/RELEASE_CLANGPDB/FV/SDM845_EFI.fd` (3 MiB); `./mkxnuimg.py` picks it up
with `tools/mualoha.py`, or `tools/update-prebuilt.sh` copies it into `devices/<name>/prebuilt/` for a machine
without the tree.

The boot shim is built here, not by that tree: `tools/mualoha.py` compiles `BootShim.S` with clang for
`aarch64-linux-gnu` and turns the object into `BootShim.bin` itself, so a Mac with Xcode and no other toolchain does
it; `llvm-objcopy` is used when the machine has one (`MUALOHA_OBJCOPY=<path>` says where).

## The pictures

```sh
python3 uefi/branding/banner.py /tmp/logo        # BootLogo.bmp, 236x236
```

It is the website's mark (hakios-web: a lowercase h and a red dot) white on black, drawn from its shapes and smoothed,
the size the FDF embeds. Nothing to install; change the size or the colours in the file and run `uefi/apply.sh`
again.

## Limits

- **Nothing here was run on a phone.** The port of the loader's hook is written from the Mu-Silicium port's design
  (`XnuLoader.c` is that port's file, unchanged); the build itself, and what the phone does with it, is untested.
- **The payload region has to be free.** The loader takes 64 MiB at `0xF0000000`; whether mu_aloha_platforms leaves it
  free at the point where BDS gives up is not known. A firmware that keeps it reserved refuses, and the loader says so.
- **The UEFI image is not in the repository**: it carries the phone vendor's own drivers, taken from the phone's own
  firmware the way Project Aloha's packages take them, and those are not redistributed here.
- **`logdump`**: `XnuLoader.c` writes its log to the raw `logdump` partition if the device tree has one. `polaris`
  does not, so there is no log to read back (`USE_SCREEN_FOR_SERIAL_OUTPUT` is 0 in `Napali.dsc`: the screen shows the
  logo, not text).
