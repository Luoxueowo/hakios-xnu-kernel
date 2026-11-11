# Adreno 630 on Polaris

This is the implementation plan and evidence record for the Xiaomi MIX 2S (Polaris, SDM845,
Adreno 630). The goal is end-to-end 3D acceleration through the existing HakiOS GPU service,
Mesa/freedreno, and the existing Metal path where the A630 command stream and shader support
allow it. A successful firmware boot alone is not a completed driver.

## Current state

- The common `drivers/gpu/hakios.adreno.c` is a stepwise backend for the POCO X5 Pro / Redwood
  (SC7280, Adreno 642L). It hardcodes Redwood's GPUCC, GMU, GBIF, RSCC and SMMUv500 layout,
  firmware names, and chip ID. Its automatic `gpu=1` path rejects other SoCs.
- `drivers/gpu/hakios.gpu.cpp` and `tools/mesa/hakios-drm/` provide the existing HakiosGPU
  IOKit service and the MSM interface used by Mesa. This service is a candidate shared client
  interface; the A630 backend must report its own hardware capabilities and lifecycle.
- `CONFIG_HAKIOS_ADRENO` remains disabled in `devices/polaris/configs/polaris.config` until the
  Polaris path has passed staged hardware bring-up.
- A separate `hakios.adreno.sdm845.c` probe is now wired into the driver configuration. On an
  SDM845 build, `adreno=1..4` reads the GPUCC, KGSL SMMU, GMU and (only when powered and clocked)
  3D core status; it does not write registers or expose rendering. This probe has not been run on
  the phone yet.
- Firmware has been extracted read-only from the Polaris Recovery vendor partition. The local
  `fw-gpu/` directory is ignored by Git; the blobs must stay out of the repository and be staged
  into the image only by the existing firmware packaging path.

## Polaris hardware facts

The checked-in Android DTB is
`mu_aloha_platforms/Platforms/NapaliPkg/Device/xiaomi-polaris/DeviceTreeBlob/Android/android-polaris.dtb`.
Its relevant nodes identify an A630 (`qcom,chipid = 0x06030001`) and these blocks:

| Block | Device-tree evidence | Base and size |
|---|---|---:|
| 3D core | `qcom,kgsl-3d0@5000000`, three `reg` entries | `0x05000000/0x40000`, `0x05061000/0x800`, `0x0509e000/0x1000` |
| GMU | `qcom,gmu`, `qcom,gpu-gmu` | `0x0506a000/0x30000`; second region `0x0b200000/0x300000` |
| KGSL SMMU | `qcom,kgsl-iommu`, `qcom,kgsl-smmu-v2` | `0x05040000/0x10000` |
| SMMU node | `arm,smmu-kgsl`, `qcom,smmu-v2` | `0x05040000/0x10000` |
| GPU clock controller | `qcom,gpucc-sdm845-v2` | `0x05090000/0x9000` |
| GFX clock controller | `qcom,gfxcc-sdm845-v2`; supplies `core_clk` | same `0x05090000/0x9000` resource, separate DT phandle/controller |
| secure GPU firmware | `qcom,pil-tz-generic`, `qcom,firmware-name = "a630_zap"` | TrustZone/PAS path; reserved region `0x8c415000/0x2000` |

The SDM845 GPUCC registers are cross-checked against Linux
[`gpucc-sdm845.c`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/clk/qcom/gpucc-sdm845.c).
The separate SDM845 v2 GFX3D clock controller is present in Google's downstream
[`kernel/msm` GPUCC/GFXCC source](https://android.googlesource.com/kernel/msm/+/048cc82284cb0ee2c7381ec5d1b08a2f7d7aa0b4/drivers/clk/qcom/gpucc-sdm845.c)
and the matching Android device tree. A6xx GPU/GMU register XML is the reference for generation
shared registers; the Polaris tree and the phone's firmware remain authoritative for device data.

The IOMMU references in the tree include GMU kernel/user contexts 5/4, the 3D user context 0,
and secure contexts 2/1. `qcom,use-3-lvl-tables` is present. The final stream IDs/masks, context
bank policy, secure-context ownership, and which device-tree resource the firmware expects must
be re-read from the complete properties before any SMMU programming.
The v2 SMMU at `0x05040000` is not interchangeable with the Redwood driver's SMMUv500 at
`0x03da0000`.

The local firmware set is `a630_gmu.bin`, `a630_sqe.fw`, and `a630_zap.{mdt,b00,b01,b02,elf}`.
The `mdt` describes three program headers, with a single loadable segment matching `b02`; the
signed metadata/other chunks still need to follow the exact Qualcomm MDT/PAS loading contract.
Do not assume that copying the Redwood `mdt` plus `b02` path is sufficient. Confirm SQE format
and version compatibility from the image header and A630-specific upstream/vendor loader before
starting the CP.

## MSM capability profile

The shared service's `GET_PARAM` answers are being changed from Redwood constants to a selected
GPU profile. For Polaris, the profile values now recorded in `hakios.adreno.c` are:

| MSM parameter | Polaris value | Evidence |
|---|---:|---|
| `GPU_ID` | `630` | Linux A6xx catalog `.revn = 630` |
| `CHIP_ID` | `0x06030001` | Polaris DT `qcom,chipid` |
| `GMEM_BASE` | `0x00100000` | Linux MSM Adreno code returns `0x100000` for A6xx families before gen3 |
| `GMEM_SIZE` | `1 MiB` | Linux A6xx catalog A630 profile |
| `MAX_FREQ` | `710 MHz` | Polaris DT `qcom,gpu-pwrlevel@0/qcom,gpu-freq` |
| `HIGHEST_BANK_BIT` | `15` | Polaris DT and SDM845 UBWC profile |
| `UBWC_SWIZZLE` | `6` | SDM845 UBWC 2.0 profile (levels 2 and 3) |
| `MACROTILE_MODE` | `0` | SDM845 UBWC profile leaves this field unset |

References: Linux [`a6xx_catalog.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/master/drivers/gpu/drm/msm/adreno/a6xx_catalog.c),
[`adreno_gpu.c`](https://android.googlesource.com/kernel/common/+/e21ee273e6fa3879aec9a27251cfce98156e07c4/drivers/gpu/drm/msm/adreno/adreno_gpu.c),
and Qualcomm's [`ubwc_config.c`](https://android.googlesource.com/kernel/common/+/1b237f190eb3d36f52dffe07a40b5eb210280e00/drivers/soc/qcom/ubwc_config.c).
The profile only describes the hardware to the existing MSM client; it does not make the A630
backend ready or expose GPU rendering.

## Implementation shape

Keep the existing MSM/HakiosGPU client contract where it is genuinely hardware independent.
`hakios.gpu_backend.h` now defines the operations boundary, and `hakios.adreno.c` dispatches the
shared service and lifecycle hooks through the selected backend. Redwood keeps its existing
implementation behind that boundary. Polaris selects its own profile and an intentionally
not-ready operations table; unimplemented map, submit, fence, stop and thermal hooks therefore
cannot fall through into Redwood's SMMUv500 or register state. The A630 implementation must fill
those operations with SDM845/A630 register, clock, power, RPMh/GMU, PAS firmware, SMMUv2 and core
logic. Shared code may own page-table allocation where formats match, ring primitives and the
IOKit service only after each shared operation has an explicit compatibility reason.

The backend boundary must carry at least: SoC/GPU identity, firmware file set, MMIO resources,
GMEM and frequency capabilities, SMMU implementation and stream/context assignments, startup and
stop operations, and the GPU address/map/submit/fence operations consumed by `hakios.gpu.cpp`.
The public `HakiosGPU` service must only start after its selected backend has completed the CP
bring-up and a bounded test submission.

## Work and acceptance gates

1. **Freeze the inputs.** Parse all relevant DT properties (`reg-names`, clocks, resets, power
   domains, interconnects, `iommus`, firmware and reserved-memory links). Record firmware sizes,
   hashes and validated headers locally; keep the firmware blobs ignored. Identify the exact
   SDM845 GPUCC/GCC sequencing and A630 GMU/CP data from primary Linux and vendor sources.
   **Gate:** every address, clock, stream, context, firmware segment and chip value has a source.
2. **Separate the backends.** Extract a small shared backend contract and retain Redwood as an
   unchanged profile. The initial dispatcher selects the read-only SDM845 probe from the SoC
   peripheral base (`0x00100000`) and rejects unknown bases before any Redwood GPU access. Before
   enabling writes, also validate the Polaris GPU identity against the runtime DT/hardware.
   **Gate:** both profiles build; selecting an unknown SoC touches no GPU register.
3. **Read-only bring-up.** The initial A630-specific, boot-argument-gated reads are in place.
   Refine them against hardware logs and add any required clock/precondition checks before adding
   writes. Keep reads and writes in separate gates, with each write preceded by logged
   preconditions and a verified readback.
   **Gate:** repeated read-only runs complete without a hypervisor trap or stale state.
4. **Power, clocks and secure firmware.** Implement SDM845 CX/GX sequencing, clocks and required
   RPMh votes; then validate the A630 GMU firmware, MDT segments, PAS 13 authentication and zap
   load. Clean up in reverse on every failure.
   **Gate:** GMU firmware reaches its documented ready state; SMMU and secure firmware report no
   faults; stop returns hardware to the loader's prior state.
5. **SMMUv2 and command processor.** Implement the v2 stream-match/context-bank layout using
   Polaris DT assignments, a 4K page-table format verified against the v2 registers, correct DMA
   cache maintenance, GMU and core mappings, A630 SQE, ring, CP protection and fences.
   **Gate:** invalid IOVAs fault; protected kernel ring/fence pages cannot be overwritten by user
   command buffers; bounded test commands complete and write back expected data.
6. **End-to-end rendering.** Expose the A630 profile through HakiosGPU; exercise Mesa/freedreno
   buffer allocation, mappings, command submission, synchronization, surfaces and display; then
   exercise the Metal client on the same service where supported. Add hang timeout/recovery and
   resource limits needed for persistent system use.
   **Gate:** sustained UI rendering, client teardown, repeated GPU start/stop, and software
   fallback all work on Polaris without kernel faults or GPU hangs.
7. **Enable only proven support.** Update the Polaris config and device status only after the
   hardware gates pass. Add build/config checks and a GPU driver review covering the shared
   service and A630 backend.
   **Gate:** a clean Polaris image boots with acceleration enabled, and disabling the boot argument
   leaves GPU hardware untouched.

## Explicitly unresolved

- The Polaris SMMU's assigned stream IDs/masks and whether secure/GMU streams are accessible to
  this kernel under the phone's hypervisor.
- The exact GMU firmware memory contract and whether `a630_zap.b00/.b01/.elf` are needed by the
  kernel's package or PAS path in addition to `.mdt/.b02`.
- The A630 SQE firmware version/features, register protection table and compatibility of the
  current MSM submission path with the 630's GMEM/UBWC configuration.
- Whether TrustZone PAS calls for GPU firmware are permitted from this boot environment.
- Cache coherency, interrupt availability, hang recovery, DVFS/thermal policy, and full
  client-memory isolation on the actual phone.

These are gates to resolve with source evidence and controlled device runs, not values to infer
from Redwood or fill with guessed constants. The Dipper repository is only a comparison reference:
its Adreno source is substantially the same Redwood backend and does not supply an A630/SDM845
implementation.
