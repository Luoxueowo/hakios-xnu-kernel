# Build and install HakiOS on a phone

Installation steps are device-specific. The MIX 2S (`polaris`) uses the fixed
`darwin` layout and a boot image flashed directly to `boot`. It does not use
the old generic proposal to carve Android `userdata`, write a whole `darwin`
image with fastboot, or switch an A/B slot.

## Xiaomi MIX 2S

Build the matching kernel, UEFI loader, userland images, and boot image from the
repository root:

```sh
make boot
```

The configuration is [`configs/polaris-boot.conf`](../configs/polaris-boot.conf).
The Android boot image is `out/boot/polaris.img`; the System APFS container is
`userland/system.apfs`. The build does not flash either image.

The existing `darwin` partition is 128 GiB. Its first 15 GiB hold fixed 10/4/1
GiB System/OS/App slots; the remaining approximately 113 GiB is persistent
HFSX Data mounted at `/private/var`. The GPT and Android `userdata` are not
resized. The builder derives its boot offsets from the same layout file the
installer uses.

For the exact build, first-time conversion, system image update, Data
preservation, recovery-mode checks, and direct `boot` flash commands, follow
[MIX 2S system image, boot image, and storage updates](../devices/polaris/STORAGE.md).
The safety rules and the routine update sequence are also summarized in
[Install safely](install-safe.md).

## Other devices

Do not reuse the Polaris offsets or commands on another phone. The POCO X5 Pro
(`redwood`) has its own documented development and dual-boot flows:
[On the phone](getting-started/phone.md), [Installing beside Android](dual-boot.md),
and [U-Boot](u-boot.md). Those pages are for redwood and do not describe the
MIX 2S partition layout.
