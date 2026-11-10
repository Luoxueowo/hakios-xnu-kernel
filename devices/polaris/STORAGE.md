# Fixed storage layout (v1)

`configs/polaris-storage.json` describes the layout inside the existing `darwin`
partition on this MIX 2S. GPT and Android's userdata partition are not resized.
One GiB means 1,073,741,824 bytes. Offsets passed to the kernel use 4096-byte blocks.

| Region | Start | Reserved size | Role |
| --- | ---: | ---: | --- |
| System | 0 | 10 GiB | Read-only Darwin root |
| OS Cryptex | 10 GiB (2621440 blocks) | 4 GiB | Shared cache and OS libraries |
| App Cryptex | 14 GiB (3670016 blocks) | 1 GiB | Apple's App resources |
| Data | 15 GiB (3932160 blocks) | 121332805632 bytes, almost 113 GiB | Journaled, case-sensitive HFS Plus on `/private/var` |

The first three regions reserve space for future images; each APFS container keeps
its actual size. They are mounted at explicit offsets (`b=`), so gaps between them
are allowed. System updates can replace each container within its own limit, and
must never change Data's start. Version 1 rejects a changed 10/4/1 boundary: such a
change needs an explicit new layout version and a user-file migration.

The boot builder derives `cryptex_os_block`, `cryptex_app_block`, `darwin_data`,
`var_hfs` and `data_required` from this JSON. The loader and kernel both enforce
Data's write floor. A failed Data mount stops boot instead of silently creating an
empty memory-backed user volume. **Do not flash this boot before installing the
matching storage layout and Data volume.**

## First installation from the previous contiguous layout

Use recovery ADB. `snapshot` saves the currently installed System/OS/App containers
and their checksums on the computer. It does not back up user files. If the legacy
layout contains a persistent HFS user volume, the installer refuses migration;
back up its files and restore them onto the new Data volume separately.

```sh
python3 tools/polaris-storage.py plan
python3 tools/polaris-storage.py snapshot out/storage/before-v1
python3 tools/polaris-storage.py pack \
  --system userland/system.apfs \
  --os out/storage/before-v1/os.apfs \
  --app out/storage/before-v1/app.apfs

# macOS (Linux: replace /sbin/newfs_hfs with mkfs.hfsplus)
python3 tools/mkdatavol.py --layout configs/polaris-storage.json \
  out/storage/data-initial.simg /sbin/newfs_hfs

# These two commands write the phone. Do not reboot between them.
python3 tools/polaris-storage.py apply-system out/storage/system-update.json --first-install
python3 tools/polaris-storage.py apply-data out/storage/data-initial.simg --initialize-data

# Build/install the matching kernel, loader, and userland.
make boot
adb reboot bootloader
fastboot flash boot out/boot/polaris.img
fastboot reboot
```

The recovery installer checks the phone, partition mapping and capacity, image
headers, slot limits and SHA-256. It stages each payload on Android's `/data`,
writes only its selected range, then reads that range back and verifies it.
Keep recovery connected and do not interrupt the initial migration. A failed
system write is retried from recovery before booting.

`apply-data` refuses an existing HFS+/HFSX volume. It decodes sparse chunks itself,
checks every write falls inside Data and explicitly skips DONT_CARE chunks;
it does not erase/discard the rest of the partition. Primary HFS metadata is
published after the other metadata so an interrupted initialization can be retried.

## Subsequent updates

Build a new System image, and use the matching OS/App containers if those were
updated. Pack and apply the new manifest, then flash the matching boot. Omit
`apply-data`: formatting is strictly a first-install operation. `apply-system`
never writes Data. An image exceeding its reserved slot is rejected before any
system write. Keep independent user-data backups; fixed offsets protect against
system-update overlap, not drive failure or an incompatible OS upgrade.

## Capacity shown by Settings

The almost-113-GiB Data volume is about 121.33 GB in decimal units. HFSX provides
its live free/available counters through `statfs` and `getattrlist`. On firmware
20F66, NSURL's important/opportunistic capacity queries return zero on this HFS
volume even while those counters and ordinary Foundation free-size queries are
positive. About uses the important-usage capacity query.

`libhakios.storage.dylib`, linked by the Preferences injection library, adapts the
CFURL and NSURL entry points: only an original zero policy-capacity answer on a
writable HFS volume may fall back to its actual `f_bavail * f_bsize`. It does not
claim purgeable space, change total capacity, override nonzero answers, or replace
a zero when the volume really is full. Read-only and other filesystem types keep
their existing answers. This is a user-space capacity adapter, not a filesystem
format or partition change.

`/polaris/bin/storage_info` reports the kernel, Foundation, NSURL and MobileGestalt
answers together. For a diagnostic boot, build userland with
`RC_JOBS=rc.jobs.storage`; normal builds use the normal command list.
