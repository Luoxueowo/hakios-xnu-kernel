<!-- Made by tools/docs-gen.py from devices/polaris/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi MIX 2S (`polaris`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sdm845` |
| Configuration | `make polaris_defconfig` |
| Maintainer | Luoxue |
| Repository | [`devices/polaris/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/polaris) |

## What works

| Part | State | |
|---|---|---|
| display | partly | UEFI's framebuffer, through the loader (to be checked) |
| touch | partly | Synaptics S3330 driver ported from the old Polaris project; not yet validated from this repository |
| buttons | partly | PM8998 key driver ported from the old Polaris project; not yet validated from this repository |
| battery | partly | Qualcomm UEFI charger cache reported through HakiosBattery; ported from the old Polaris setup, not yet validated here |

The state is what has been seen on this device, written down in `devices/polaris/device.conf`; a part the table does not name has not been looked at.
