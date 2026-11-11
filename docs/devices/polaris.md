<!-- Made by tools/docs-gen.py from devices/polaris/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Xiaomi MIX 2S (`polaris`)

| | |
|---|---|
| Vendor | Xiaomi |
| SoC | `sdm845` |
| Configuration | `make polaris_defconfig` |
| Maintainer | Luoxue |
| Repository | [`devices/polaris/`](https://github.com/oosexgang/hakios-xnu-kernel/tree/main/devices/polaris) |
| Firmware | `devices/polaris/pull-firmware.sh` pulls it from your own phone |

## What works

| Part | State | |
|---|---|---|
| display | partly | UEFI boots successfully; display through the UEFI framebuffer is partially working |
| touch | works | Synaptics S3330 RMI4 touch input works on the MIX 2S (`touchscreen=1`) |
| buttons | works | MIX 2S PM8998 power and volume keys work (`touchscreen=1`) |
| cpu | works | all eight cores (`bootcpu=7 cpus=8 cpufreq=1`); the clock is the boot loader's, SDM845's OSM is not driven |
| storage | works | persistent HFSX Data volume; Settings reports about 120–121 GB available |
| battery | works | battery level and charging status are reported through HakiosBattery (`battery=1`) |

The state is what has been seen on this device, written down in `devices/polaris/device.conf`; a part the table does not name has not been looked at.
