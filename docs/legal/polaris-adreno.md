# MIX 2S Adreno 630 port

The Polaris backend in `devices/polaris/drivers/gpu/hakios.adreno.polaris.c` is adapted from `bsd/dev/mi8_adreno.c` in
[youzjuzi/dipper-xnu](https://github.com/youzjuzi/dipper-xnu), commit
[`5fcd75220a6bec12f55e2c8284f4d0854c73c24c`](https://github.com/youzjuzi/dipper-xnu/commit/5fcd75220a6bec12f55e2c8284f4d0854c73c24c).
The A630 initialization table is from Dipper's `bsd/dev/mi8_a630_hw_init.inc`. The port uses HakiOS's common `HakiosGPU`
service and firmware loader, with Polaris device configuration and lifecycle entrypoints.

## Scope and current status

The port carries the SDM845 Adreno 630 GPUCC/GCC power-up, GMU and HFI startup, GPU SMMUv2 setup, firmware loading, ZAP
authentication, command processor initialization, IOVA allocation/mapping, command submission, fences, and shutdown paths.
The Polaris framebuffer now has a separate SDM845 DPU 4.0 page-flip path adapted from Dipper's
`MiniIOMobileFramebuffer.cpp` work in commit `5fcd75220a6bec12f55e2c8284f4d0854c73c24c`. It checks the DPU version and active
plane geometry, recovers `SRC0_ADDR=0` to the firmware framebuffer, maps a second scanout buffer from the Polaris 36 MiB
reserved display region, and submits flips through `CTL0.FLUSH`. It keeps the existing CPU composition/scaling path; Dipper's
native-resolution scaler and zero-copy IOSurface pool are not part of this first port. Panel/backlight, CPU-frequency, DDR
governor, and thermal governor code remain excluded. The flip path is enabled by `fb_flip=1` and awaits on-device verification.

The backend is compiled into the Polaris defconfig. `gpu=1` requests the full initialization sequence, and `adreno_keep=1` keeps
the initialized 3D core running and starts the shared `HakiosGPU` service after the hardware checks pass. The standard Polaris boot
arguments include `gpu=1 adreno_keep=1 gpu_log=1`; `USERLAND_GL=1` includes Mesa's OpenGL ES renderer. The fresh on-device log
from 2026-11-24 confirms GMU/HFI startup, successful ZAP authentication, CP ring consumption and memory write, and the GPU staying
on for HakiosGPU. CoreAnimation reports `the server has an accelerated renderer`; Mesa reports OpenGL ES 3.2, Mesa 25.0.7, and
renderer `FD630`. Two desktop frames were submitted through HakiosGPU with fences 1 and 2. No panic was recorded.

The 116.002 GiB `darwin` partition now exists, and System, OS, App, and initial HFSX Data images were written and read back. The
latest boot mounted HFSX Data at `/private/var`; `userdata` remains unformatted for Android. Hardware rendering now works through
Mesa Freedreno. DPU page flipping is integrated, but the 2026-11-24 log predates this change and does not validate the new scanout
path.

Firmware is read from `fw/` in the image, populated by `userland/build.sh` from files the user places in `fw-gpu/`:
`a630_gmu.bin`, `a630_sqe.fw`, `a630_zap.mdt`, and `a630_zap.b02`. Vendor firmware is not included in this repository.

The platform constants are carried from Dipper's SDM845 implementation and matched against Polaris sources available in this
checkout: GCC base `0x00100000` in the Polaris loader profile; command database `0x85fe0000` in the Polaris UEFI memory map;
GPUCC `0x05090000`, GMU `0x0506a000`, GPU SMMU `0x05040000`, and 3D core `0x05000000` in the SDM845 device tree; and the ZAP
reserved region `0x8c415000` in the Polaris device tree. These source matches are not runtime validation of the register sequence.

## Licensing and redistribution

The separate table file `devices/polaris/drivers/gpu/hakios.adreno.a630_hw_init.inc` retains its `GPL-2.0-only` SPDX marker,
original copyright notices, and Dipper attribution. The complete GPL 2.0 text is in
[`GPL-2.0-only.txt`](GPL-2.0-only.txt).

Dipper records its `mi8_adreno.c` license as **UNRESOLVED** because the driver includes and executes the GPL-derived table;
it does not claim that this combination is compatible with the APSL kernel. This port preserves that open status and does not
relabel the table or driver as MIT or APSL. See Dipper's [GPU license review](https://github.com/youzjuzi/dipper-xnu/blob/main/docs/legal/GPU_LICENSE.md).

The imported source can be reviewed and built locally, but this repository will not distribute a merged kernel, boot image, or
other binary containing the table unless the licensing issue is resolved. The local kernel build is a development artifact,
not a release binary.
