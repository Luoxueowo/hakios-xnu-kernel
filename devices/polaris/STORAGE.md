# MIX 2S system image, boot image, and storage updates

This is the current Polaris workflow. It replaces the old instructions based on
carving Android `userdata`, writing a combined `darwin.img` with fastboot, or
installing into an inactive A/B slot. The MIX 2S already has a dedicated
`darwin` GPT partition; these steps do not resize Android `userdata` or change
the GPT.

## The fixed layout

[`configs/polaris-storage.json`](../../configs/polaris-storage.json) is the
source of truth. The existing `darwin` partition is 137,438,932,992 bytes
(128 GiB). Offsets use 4096-byte blocks.

| Region | Start | Reserved size | Purpose |
| --- | ---: | ---: | --- |
| System | 0 | 10 GiB | Read-only Darwin root |
| OS Cryptex | 10 GiB (block 2621440) | 4 GiB | Shared cache and OS libraries |
| App Cryptex | 14 GiB (block 3670016) | 1 GiB | App resources |
| Data | 15 GiB (block 3932160) | 121,332,805,632 bytes, about 113 GiB | Persistent, journaled HFSX mounted at `/private/var` |

The first three sizes are reserved slots; each APFS image remains its actual
size. Data starts at a fixed 15 GiB boundary. Version 1 refuses changed
boundaries: a different layout requires a new version and a deliberate user
data migration.

## Build the images

If the local Darwin 16.5 inputs have not been extracted yet, unpack a compatible
20F66 IPSW you obtained yourself:

```sh
make unpack IPSW=/path/to/your-compatible-20F66.ipsw
```

Build the matching kernel, loader, userland, and Android boot image:

```sh
make boot
```

[`configs/polaris-boot.conf`](../../configs/polaris-boot.conf) selects the
Polaris defconfig, `FLASHBOOT=1`, `CRYPTEXES=flash`, the fixed storage layout,
boot arguments, and output paths. The build produces:

| Artifact | Use |
| --- | --- |
| `userland/system.apfs` | System container written to the System slot |
| `userland/flashboot.apfs` | Small resident image supplied as `md0` |
| `userland/root.apfs` | Init ramdisk supplied as `md1` and mounted at `/polaris` |
| `out/boot/polaris.img` | Packaged image for the phone's `boot` partition |

`make boot` builds and checks the boot image; it does not write to the phone or
update `darwin`. Set `BUILD_KERNEL=0`, `BUILD_LOADER=0`, or
`BUILD_USERLAND=0` in the boot config only when reusing matching existing
outputs. The builder derives the Cryptex and Data offsets from the layout JSON
and checks that the APFS paths and boot image fit their limits.

## Convert an older contiguous `darwin` layout once

Do this only when migrating from the old layout. The current MIX 2S already has
the fixed layout and initialized Data volume; skip this section for normal
updates.

1. Build the new images with `make boot`.
2. Boot the phone into recovery, connect USB, and confirm it appears in
   `adb devices`. Keep the `darwin` partition unmounted in recovery.
3. Save the existing APFS containers on the computer. Pick a new snapshot
   directory; the tool will not overwrite one.

   ```sh
   python3 tools/polaris-storage.py plan
   python3 tools/polaris-storage.py snapshot out/storage/before-fixed-layout
   ```

   This backs up System/OS/App, not files from a legacy persistent HFS volume.
   If the installer detects legacy HFS data, it stops. This tool has no
   in-place importer or override for that old volume, so conversion with
   persistent legacy data is not currently supported. Do not bypass the check
   or erase its header manually; keep the old layout until a dedicated
   migration procedure is available.
4. Package the new System with the saved OS and App containers:

   ```sh
   python3 tools/polaris-storage.py pack \
     --system userland/system.apfs \
     --os out/storage/before-fixed-layout/os.apfs \
     --app out/storage/before-fixed-layout/app.apfs \
     -o out/storage/system-update.json
   ```

5. Create the empty Data image. On macOS, use Apple's formatter; on Linux,
   omit the last argument to use `mkfs.hfsplus` from `PATH`.

   ```sh
   python3 tools/mkdatavol.py --layout configs/polaris-storage.json \
     out/storage/data-initial.simg /sbin/newfs_hfs
   ```

6. Still in recovery, write the three APFS images and initialize Data. Do not
   reboot between these commands.

   ```sh
   python3 tools/polaris-storage.py apply-system \
     out/storage/system-update.json --first-install
   python3 tools/polaris-storage.py apply-data \
     out/storage/data-initial.simg --initialize-data
   ```

7. Flash the matching boot image directly to `boot` and restart:

   ```sh
   adb reboot bootloader
   fastboot devices
   fastboot flash boot out/boot/polaris.img
   fastboot reboot
   ```

Do not use `fastboot boot`, `fastboot flash darwin`, `fastboot -w`, or
`tools/userdata-carve.py` for this Polaris layout.

## Update System and boot

For a regular update, keep the installed layout and Data volume. Rebuild the
boot image, snapshot the current containers in recovery, package the new System
with the unchanged OS/App containers, and apply that manifest:

```sh
make boot
adb reboot recovery
# Confirm `adb devices` shows recovery, and use a fresh directory each time.
python3 tools/polaris-storage.py snapshot out/storage/before-update-01
python3 tools/polaris-storage.py pack \
  --system userland/system.apfs \
  --os out/storage/before-update-01/os.apfs \
  --app out/storage/before-update-01/app.apfs \
  -o out/storage/system-update.json
python3 tools/polaris-storage.py apply-system out/storage/system-update.json
adb reboot bootloader
fastboot flash boot out/boot/polaris.img
fastboot reboot
```

If OS or App Cryptexes are being changed, supply the matching raw APFS
containers to `pack` instead of the saved copies. `pack` validates APFS
headers, image lengths, hashes, and the 10/4/1 GiB slot limits. It does not
accept a DMG in place of an APFS container.

`apply-system` checks the connected device, `darwin` mapping and capacity, and
that the partition is not mounted. It validates the full manifest before
writing, stages and hashes each image, writes only the corresponding System,
OS, and App extents, then reads them back and verifies their SHA-256 hashes.
Already identical images are skipped. A write or readback error means: stay in
recovery and do not boot until the matching update has been applied and
verified.

**Never run `apply-data` for an update.** It is a first-initialization command;
it refuses an existing HFS+/HFSX volume. The Data region is outside all three
system slots and `apply-system` never writes it. Keep separate backups of user
files; an APFS snapshot does not back up Data.

## Capacity shown in Settings

Data is about 113 GiB, or 121.33 decimal GB. On the MIX 2S, Settings now reports
about 120–121 GB available. iOS 16.5's About screen asks NSURL for important
usage capacity; that query returned zero on this writable HFSX volume even
though HFS free-space queries were positive. `libhakios.storage.dylib` adapts
the CFURL and NSURL entry points: only an original zero answer on writable HFS
can fall back to the filesystem's actual available block count. It does not
claim purgeable space, change total capacity, override a nonzero answer, or
replace zero when the volume is actually full. Read-only volumes and other
filesystem types keep their original results. This is a user-space reporting
fix; it does not resize or reformat storage.
