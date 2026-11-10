# Safe system and boot updates on the MIX 2S

These instructions describe the implemented Polaris layout. They replace the
older plan that proposed shrinking `userdata`, installing into an inactive
slot, or using `fastboot boot` as the installed boot path. For the complete
first-install and update commands, see
[the MIX 2S storage guide](../devices/polaris/STORAGE.md).

## What an update writes

| Action | Destination | Effect |
|---|---|---|
| `polaris-storage.py apply-system` | `darwin` System/OS/App slots | Writes bounded APFS images and verifies each written range by SHA-256 |
| `fastboot flash boot out/boot/polaris.img` | `boot` | Installs the matching kernel, loader, and flash-root ramdisks |
| `polaris-storage.py apply-data --initialize-data` | `darwin` Data region | First initialization only; never part of a system update |

The current v1 layout keeps System, OS Cryptex, and App Cryptex in fixed
10/4/1 GiB slots. Data begins at byte 16,106,127,360 (15 GiB) and occupies the
rest of the existing 128 GiB `darwin` partition. The updater does not change
the GPT or Android `userdata`.

## Before you start

- Unlock the bootloader and have `adb` and `fastboot` available.
- Back up user files separately. `snapshot` saves System/OS/App containers,
  not HFSX Data or `/private/var` contents.
- Apply APFS updates from recovery ADB with `darwin` unmounted. The tool checks
  the Polaris device, `/dev/block/sda22`, partition size, and mount state.
- Build the boot image and System container from the same source/configuration
  before writing either one.

## Routine update

1. Build the matching boot and userland images:

   ```sh
   make boot
   ```

2. Enter recovery and connect USB. Confirm `adb devices` lists the recovery
   device. Leave `darwin` unmounted; the storage tool checks the device name,
   partition mapping, capacity, and mount state itself.
3. Snapshot current APFS containers to a new directory on the computer, then
   package the new System image together with the unchanged OS/App containers:

   ```sh
   python3 tools/polaris-storage.py snapshot out/storage/before-update-01
   python3 tools/polaris-storage.py pack \
     --system userland/system.apfs \
     --os out/storage/before-update-01/os.apfs \
     --app out/storage/before-update-01/app.apfs \
     -o out/storage/system-update.json
   ```

   Choose a fresh snapshot directory for each run. Snapshot files are APFS
   image backups; they do not contain user files from Data.
4. Apply and verify the System/OS/App images from recovery:

   ```sh
   python3 tools/polaris-storage.py apply-system out/storage/system-update.json
   ```

5. Flash the matching boot image directly to `boot`:

   ```sh
   adb reboot bootloader
   fastboot devices
   fastboot flash boot out/boot/polaris.img
   fastboot reboot
   ```

The updater validates the full manifest before its first write, checks the
device is the MIX 2S and `darwin` is `/dev/block/sda22`, stages and hashes each
image, then reads back and hashes each written range. It rejects images larger
than their reserved slot and skips a range that already has the expected hash.
If a write or readback check fails, stay in recovery and do not boot that
partially updated system.

## First initialization and layout changes

Use `--first-install` only when converting the old contiguous APFS layout with
no legacy persistent HFS data. If the tool detects an old HFS volume, conversion
is not currently supported: the installer has no in-place importer or override.
Do not bypass that check or erase the volume header manually. Create the new
Data image with `tools/mkdatavol.py` and run `apply-data ... --initialize-data`
once, after `apply-system`; it refuses to format an existing HFS+/HFSX volume.
Never run it for an update or to repair a system image.

Do not use `fastboot flash darwin`, `fastboot -w`, or
`tools/userdata-carve.py` for Polaris. Do not change the 10/4/1 GiB boundaries
under layout v1. A boundary change needs a new layout version, a data migration
plan, and matching loader/kernel changes. The current implementation does not
provide an in-place resize operation.

For a different device, follow its own guide. The POCO X5 Pro installation
pages describe a separate layout and boot flow.
