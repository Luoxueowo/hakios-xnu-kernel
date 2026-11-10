<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | gpu | storage | audio | network |
|---|---|---|---|---|---|---|---|---|
| [Xiaomi MIX 2S](polaris.md) | `sdm845` | partly | partly |  |  |  |  |  |
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes | no | yes | yes | yes |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `sdm845` | Qualcomm SDM845 (Snapdragon 845) | carried by polaris (`devices/polaris/soc/`) |
