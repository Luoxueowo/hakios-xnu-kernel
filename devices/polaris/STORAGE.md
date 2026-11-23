# MIX 2S system image, boot image, and storage updates

This is the Polaris workflow for the MIX 2S. This device did not expose a
`darwin` GPT partition, so its first installation carves a 116.002 GiB partition
from the end of Android `userdata`. The remaining `userdata` is about 116.26 GiB.
That one-time operation changes the GPT and discards the old data in the carved
range. Format Android `userdata` before booting Android again; direct HakiOS
boot uses the separate `darwin` partition and its own Data volume. Later HakiOS
updates do not change the GPT.

## The fixed layout

[`configs/polaris-storage.json`](../../configs/polaris-storage.json) is the
source of truth. The MIX 2S `darwin` partition is 124,556,128,256 bytes
(116.002 GiB, aligned to the UFS 2 MiB boundary). Offsets use 4096-byte blocks.

| Region | Start | Reserved size | Purpose |
| --- | ---: | ---: | --- |
| System | 0 | 10 GiB | Read-only Darwin root |
| OS Cryptex | 10 GiB (block 2621440) | 4 GiB | Shared cache and OS libraries |
| App Cryptex | 14 GiB (block 3670016) | 1 GiB | App resources |
| Data | 15 GiB (block 3932160) | 108,450,000,896 bytes, 101.002 GiB | Persistent, journaled HFSX mounted at `/private/var` |

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

## First installation on this MIX 2S

Do this once to create `darwin`. The `--wipe` planner does not shrink or inspect
the old F2FS filesystem; it reduces the `userdata` GPT entry and adds a 116.002 GiB
APFS entry named `darwin`. Its old tail data is discarded, and Android's
shortened `userdata` must be formatted before booting Android again.

1. Build the matching kernel, loader, and userland with `make boot`.
2. In Recovery, unmount `userdata` before changing the GPT. Save the first and
   last 8 blocks of `/dev/block/sda` in a new local directory. Do not reuse a
   previous GPT backup.

   ```sh
   mkdir -p out/storage/gpt-before-darwin
   adb exec-out sh -c 'dd if=/dev/block/sda bs=4096 count=8 2>/dev/null' > out/storage/gpt-before-darwin/gpt-head.bin
   sda_bytes=$(adb shell blockdev --getsize64 /dev/block/sda)
   sda_tail_block=$((sda_bytes / 4096 - 8))
   adb exec-out sh -c "dd if=/dev/block/sda bs=4096 skip=$sda_tail_block count=8 2>/dev/null" > out/storage/gpt-before-darwin/gpt-tail.bin
   ```
3. Generate and inspect the new GPT. The planner writes files on the computer;
   it does not write to the phone. Run the `dd` commands it prints in Recovery
   only after checking that they target `/dev/block/sda` and the expected
   `darwin` size.

   ```sh
   python3 tools/userdata-carve.py --wipe out/storage/gpt-before-darwin 116 darwin
   ```

4. Reboot Recovery so it rereads the GPT. Confirm `/dev/block/by-name/darwin`
   resolves to `/dev/block/sda22` and is 124,556,128,256 bytes. Before any
   Android boot, format the shortened `userdata` (for example, with
   `fastboot -w`); this erases Android user data. HakiOS direct boot does not
   mount Android `userdata`.
5. Prepare the three APFS containers and the empty Data image:

   ```sh
   python3 tools/polaris-storage.py pack \
     --system userland/system.apfs \
     --os out/storage/os.apfs \
     --app sysimage/cryptex-app.dmg \
     -o out/storage/system-update.json
   python3 tools/mkdatavol.py --layout configs/polaris-storage.json \
     out/storage/data-initial.simg /sbin/newfs_hfs
   ```

6. In fastboot, erase only the new `darwin` partition to remove the carved
   userdata contents, then reboot Recovery. Apply the fresh containers and
   initialize HakiOS Data from Recovery:

   ```sh
   fastboot erase darwin
   fastboot reboot recovery
   python3 tools/polaris-storage.py apply-system \
     out/storage/system-update.json --new-partition --stage-dir /tmp
   python3 tools/polaris-storage.py apply-data \
     out/storage/data-initial.simg --initialize-data --stage-dir /tmp
   ```

7. Flash the matching boot image directly to `boot` and restart:

   ```sh
   fastboot flash boot out/boot/polaris.img
   fastboot reboot
   ```

Do not use `fastboot boot` or `fastboot flash darwin`; regular updates use the
bounded writes below. `fastboot -w` is only needed to reset Android `userdata`,
not to initialize HakiOS Data inside `darwin`.

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
python3 tools/polaris-storage.py apply-system out/storage/system-update.json --stage-dir /tmp
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

Data is 101 GiB, or 108.45 decimal GB. iOS 16.5's About screen asks NSURL for important
usage capacity; that query returned zero on this writable HFSX volume even
though HFS free-space queries were positive. `libhakios.storage.dylib` adapts
the CFURL and NSURL entry points: only an original zero answer on writable HFS
can fall back to the filesystem's actual available block count. It does not
claim purgeable space, change total capacity, override a nonzero answer, or
replace zero when the volume is actually full. Read-only volumes and other
filesystem types keep their original results. This is a user-space reporting
fix; it does not resize or reformat storage.
