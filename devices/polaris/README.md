# Device: Xiaomi Technologies, Inc. Polaris MP v2.1 (polaris)

Made by `tools/new-device.py` on 2026-11-10, from the file polaris.dtb. Edit it as the port goes.

| What | Value |
|---|---|
| Board (the tree's `model`) | Xiaomi Technologies, Inc. Polaris P2 v2.1 |
| Compatible | `qcom,sdm845-mtp qcom,sdm845 qcom,mtp` |

## XNU loader platform

The common loader selects the Polaris/SDM845 profile when UEFI reports the framebuffer at `0x9D400000`, the
`Display Reserved` region in this device's memory map. The profile is based on the checked-in [Android device tree](../../mu_aloha_platforms/Platforms/NapaliPkg/Device/xiaomi-polaris/DeviceTreeBlob/Android/android-polaris.dtb)
and [UEFI memory map](../../mu_aloha_platforms/Platforms/NapaliPkg/Device/xiaomi-polaris/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c):

| Property | Value |
|---|---|
| GICv3 distributor | `0x17A00000` |
| GICv3 redistributors | `0x17A60000` |
| EL1 virtual timer PPI (GIC INTID) | `19` (old Polaris loader profile and the device's timer DT) |
| PSCI method | SMC |
| CPU MPIDRs | `0x000` through `0x700`, Aff1 (8 CPUs) |
| Generic timer frequency | `19.2 MHz` |

This loader profile is integrated, and XNU has now booted to the system on the MIX 2S. The GICv3 virtual timer uses INTID 19,
as specified by the old Polaris loader profile and the device's timer description.

With `battery=1`, the loader reads the resident Qualcomm ChargerEx/QcomCharger protocols and shares a read-only status cache with
XNU. The Polaris profile does not reinitialize QcomCharger, matching the old Polaris loader setting. XNU publishes the readings as
`HakiosBattery` (`IOPMPowerSource`) so Darwin can show the level and charging state; this path is integrated but not yet validated
on the phone. Include `battery=1` in the payload command line to enable it; the defconfig allows the argument but does not add it
automatically.

## The SoC addresses

No reference tree to compare with (devices/redwood/prebuilt/device-tree.dtb is missing).

| Argument | Value | Against the reference |
|---|---|---|
| `soc_ipa_base` | `0x1e40000` |  |
| `soc_gsi_base` | `0x1e04000` |  |
| `soc_ipcc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_aoss` | `0xc300000` |  |
| `soc_rsc` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_apps_smmu` | `0x15000000` |  |
| `soc_cmd_db` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_mpss_mem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `modem_ipa_fw_at` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_smem` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_imem` | `0x14600000` |  |
| `soc_ipa_stream` | `0x720` |  |
| `soc_ipa_stream_uc` | `0x722` |  |
| `soc_pas_mpss` | `(not found)` | **missing**: the tree has no node this was read from; see the table in devices/redwood/README.md |
| `soc_pas_ipa` | `0xf` |  |
| `soc_ipa_fw` | `ipa_fws` |  |

`soc.args` is what `tools/soc-args.py` read out of the tree; `pull-soc.sh` makes it again from the phone on
adb. The run scripts add it to the kernel command line. Where each argument comes from in the tree:
`devices/redwood/README.md`.

## What is this phone's own

The kernel, loader and user space know nothing about a phone: addresses, interrupts, rails and firmware
come from its tree and its Android. What is written for one phone is the list below. The head of each file
says where its numbers came from; that is the map of what to rewrite.

- [ ] **UEFI**: the firmware that starts the loader: devices/redwood/uefi/ is a port of mu_aloha_platforms; the vendor's drivers are extracted from your own phone (uefi/README.md)
- [ ] **Display**: framebuffer path still needs device-specific validation.
- [ ] **Buttons**: `drivers/input/hakios.buttons.polaris.c` reads the MIX 2S PM8998 power and volume keys; ported from the old project, pending validation from this repository.
- [ ] **Touch**: `drivers/input/hakios.synaptics.c` is the MIX 2S Synaptics S3330 RMI4 driver, ported from the old Polaris project; verify it from this repository on the phone.
- [ ] **Storage**: hakios.ufs.c and hakios.ufshc.c (UFS); a phone with eMMC needs another driver
- [ ] **CPU clocks**: hakios.cpufreq.c
- [ ] **GPU**: hakios.adreno.c (Adreno 642L here: another generation needs its own firmware and registers)
- [ ] **Wi-Fi**: hakios.wifi.c, hakios.lx.c and wifi/lx/ (the WCN6750 radio of the reference phone)
- [ ] **Audio**: hakios.adsp.c, hakios.audio.c, hakios.sound.c, hakios.speaker.c, hakios.mic.c
- [ ] **Battery**: `battery=1` enables the Qualcomm UEFI status cache and `HakiosBattery`; not yet validated on this phone.
- [ ] **Cellular**: hakios.mpss.c, hakios.qmi.c, hakios.nas.c, hakios.ipa.c, hakios.cell.c; soc.args carries the addresses, the IPA layout is for IPA 4.11 (docs/cellular.md)
- [ ] **Haptics and cameras**: hakios.haptic.c, hakios.camera.c

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/polaris.config          CONFIG_DEVICE="polaris"   and whatever is true of this phone only
    configs/polaris_defconfig       #include common_defconfig
                                   #include <the SoC's fragment: sc7280.config for the SC7280 family>
                                   #include polaris.config

Then `make polaris_defconfig`. Turn on only what this phone has been shown to do; `tools/hakios-config.py check` says what the
choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails in each are the reference phone's. Leave out in `configs/polaris.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...; `configs/qemu_defconfig` leaves out all of them), and put
each one back when it has been made to work here. `tools/hakios-config.py show` lists what is built in.

## Firmware

Pulled from your own phone, never committed: `devices/redwood/pull-firmware.sh`, or
`pull-firmware-root.sh` where the firmware is readable by root only. Check with `make doctor`.

## Before you open a pull request

`tools/provenance-check.py` must pass: no vendor firmware, no UEFI drivers, no Apple files in the tree.

## The repository

A HakiOS device repository: what is true of this device and of no other. The kernel, the loader, the drivers and the tools are
the common repository's; the sdm845 family's facts are in `soc/`, carried with the device. How the three are put together:
[docs/porting/repositories.md](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/porting/repositories.md).

| File | What |
|---|---|
| `metal.args` | what `tools/metal-run.sh` gives this phone: its cores, its panel's rate, the threads that copy frames, the touch poll |
| `device.conf` | what the device is, and how far each part of it works: its page in the documentation is made from it |
| `configs/polaris_defconfig` | the common kernel, the SoC's fragment, this device's: `make polaris_defconfig` |
| `configs/polaris.config` | what is this device's own: the drivers built in, the arguments it needs |
| `soc/` | its SoC family's layer (sdm845): `soc.conf`, `configs/sdm845.config`, the SoC's drivers; carried here so that this one repository is all the device needs |
| `drivers/` | drivers of its own, if it needs any (`drivers/<subsystem>/conf`, as in the common repository) |

Every phone driver of the common repository is left out to begin with: their addresses and pins are the phone they were written
for. Put one back in `configs/polaris.config` when it has been made to work here, and say so in `device.conf`.

Check with `tools/hakios-device.py check polaris`.
