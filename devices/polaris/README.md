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

UEFI boots successfully on the MIX 2S. Display output through the UEFI framebuffer is partially working. XNU has confirmed working
touch input, power and volume buttons, eight active CPUs, persistent storage, and battery-level and charging-status reporting. The
boot configuration uses `touchscreen=1`, `bootcpu=7 cpus=8 cpufreq=1`, and `battery=1`. CPU clocks remain at the boot loader's
setting: the SDM845 uses OSM, which the current CPU-frequency driver does not control.

With `battery=1`, the loader reads the resident Qualcomm ChargerEx/QcomCharger protocols and shares a read-only status cache with
XNU. The Polaris profile does not reinitialize QcomCharger, matching the old Polaris loader setting. XNU publishes the readings as
`HakiosBattery` (`IOPMPowerSource`) for Darwin's battery level and charging-status display. This reports charging state; it does not
control the phone's charging hardware.

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

- [x] **UEFI boot**: UEFI starts the loader and boots HakiOS on the MIX 2S.
- [ ] **Display (partial)**: UEFI framebuffer output works partially; display support still needs further device-specific work.
- [x] **Buttons**: `drivers/input/hakios.buttons.polaris.c` handles the MIX 2S PM8998 power and volume keys; both are confirmed working.
- [x] **Touch**: `drivers/input/hakios.synaptics.c` handles the MIX 2S Synaptics S3330 RMI4 touchscreen; touch input is confirmed working.
- [x] **Multi-core CPU**: XNU runs with eight active CPUs (`bootcpu=7 cpus=8`). CPU clock control is still pending; the SDM845 OSM is not driven by the current CPU-frequency driver.
- [x] **Storage**: UFS-backed persistent HFSX Data is mounted for user data; Settings reports about 120–121 GB available.
- [ ] **CPU clocks**: the SDM845 OSM path is not implemented; the existing EPSS path in `hakios.cpufreq.c` is for the SM7325.
- [ ] **GPU (partial)**: the MIX 2S Adreno 630 backend is built but does not start by default. `gpu=1` opts into guarded startup and opens HakiosGPU only after the client-root probe passes. The seven GPU firmware files have been pulled from the phone's read-only `/vendor/firmware` and their hashes match the device. Startup and rendering still need on-phone validation. The existing `hakios-metal` client targets the A635-family Adreno 642L (`0x06030500`); Polaris reports A630 (`0x06030001`), so Metal remains unsupported until its command and compiler profile is adapted and validated. See [the Polaris Adreno 630 notes](../../docs/porting/polaris-adreno-630.md).
- [ ] **Wi-Fi**: hakios.wifi.c, hakios.lx.c and wifi/lx/ (the WCN6750 radio of the reference phone)
- [ ] **Audio**: hakios.adsp.c, hakios.audio.c, hakios.sound.c, hakios.speaker.c, hakios.mic.c
- [x] **Battery status**: `battery=1` enables the Qualcomm UEFI status cache and `HakiosBattery`; battery level and charging status are confirmed visible.
- [ ] **Cellular**: hakios.mpss.c, hakios.qmi.c, hakios.nas.c, hakios.ipa.c, hakios.cell.c; soc.args carries the addresses, the IPA layout is for IPA 4.11 (docs/cellular.md)
- [ ] **Haptics and cameras**: hakios.haptic.c, hakios.camera.c

## Configuration

The kernel's settings are layered the way an Android kernel's are: the common kernel, the SoC's, this phone's
(`docs/configs.md`). Two short files make this phone a configuration:

    configs/polaris.config          CONFIG_DEVICE="polaris"   and whatever is true of this phone only
    configs/polaris_defconfig       #include common_defconfig
                                   #include sdm845.config
                                   #include polaris.config

Then `make polaris_defconfig`. `HAKIOS_ADRENO` is built so the Polaris port can be validated; `CONFIG_GPU` stays unset,
so the GPU is not started unless `gpu=1` is added explicitly after the staged path is ready to try.
`tools/hakios-config.py check` says what the choice needs.

The drivers are part of the configuration too (`docs/drivers-config.md`): every one is built unless a configuration leaves it out,
and the addresses, pins and rails in each are the reference phone's. Leave out in `configs/polaris.config` what has not been
ported to this phone yet (`# CONFIG_HAKIOS_GOODIX is not set`, ...; `configs/qemu_defconfig` leaves out all of them), and put
each one back when it has been made to work here. The Adreno backend is an opt-in exception: it is compiled but not started
by default, and `gpu=1` first runs its bounded startup and client-root probe. `tools/hakios-config.py show` lists what is built in.

## Build a boot image

Edit [`configs/polaris-boot.conf`](../../configs/polaris-boot.conf) to change the Polaris kernel config, boot arguments, input
images, or build switches, then run:

```sh
make boot
```

This builds the kernel, flash-root userland images, and loader, then packages `out/boot/polaris.img`. The `resident` entry is
`userland/flashboot.apfs` with `/sbin/launchd` and becomes md0; `ramdisk` is `userland/root.apfs` with `/sbin/init` and becomes
md1. `userland/system.apfs` is the System container for the fixed `darwin` layout. The script checks the APFS paths and boot
image size, and it does not write to the phone. Use `make boot BOOT_CONFIG=path/to/other.conf` to select another configuration.

Building the Android boot image does not update the installed System container. For the recovery ADB update procedure, the
first-time Data initialization, and the direct `fastboot flash boot` step, see [STORAGE.md](STORAGE.md).

## Firmware

With the MIX 2S in Recovery and visible to ADB, run:

```sh
ADB=/path/to/adb devices/polaris/pull-firmware.sh
```

The script reads `/vendor/firmware` only when the vendor mount is read-only. If Recovery has not mounted vendor, it mounts
`/dev/block/by-name/vendor` at `/mnt/hakios-vendor` with `ro,noload`, pulls the seven A630 GMU, SQE and ZAP files into the ignored
`fw-gpu/` directory, records their SHA-256 hashes locally, and unmounts only the mount it created. Vendor firmware stays out of Git.

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

The userland APFS volume is `polaris` and is mounted on `/polaris`. Rebuild
userland and the system image after changing this layout: the installed Darwin
image must contain the matching launchd and library symlinks. Legacy userland
prebuilts using `/redwood` are not compatible with this layout.

See [STORAGE.md](STORAGE.md) for the fixed 10/4/1 GiB system slots, persistent
Data volume, first installation and updates that preserve user files.
