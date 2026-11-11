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
  3D core status; it checks the GCC GPU config-AHB clock before reading GPUCC and checks CX plus
  GPU MEMNOC before reading the SMMU. Step 2 also reports SMMU capabilities, the current
  SMR/S2CR stream routing (or the device-tree KGSL stream IDs when stream indexing is active),
  and the IDR1-sized context-bank window and its nonzero registers.
  It does not write registers or expose rendering. This probe has not been run on the phone yet.
- The A630 backend now builds software 4K, three-level ARM LPAE stage-1 tables for the shared
  `map/mapped/unmap` calls and performs the same cache maintenance as the existing GPU mapper.
  Table pages are suballocated from XNU pages, checked against the SMMU output-address size, and
  kept allocated for the backend lifetime. The diagnostic's IDR2 read supplies the OAS and 4K
  page-table capability; mapping fails closed until both have been observed. These tables are not
  connected to a live SMMU context or stream, and the backend `ready()` gate remains false.
- `adreno=5` checks the staged A630 firmware and PAS memory description without writing hardware.
  The local set contains a 32 KiB GMU image and a 6,684-byte ZAP MDT with three ELF program
  headers; its single relocatable `PT_LOAD` segment matches `a630_zap.b02` at 1,968 bytes. The
  header is 148 bytes and its 6,536-byte signed metadata segment is `a630_zap.b01`; both copies
  match Qualcomm's split-firmware contract
  ([`mdt_loader.c`](https://android.googlesource.com/kernel/common/+/d3d009847a119db499094aca61bd8a9935bc94ef/drivers/soc/qcom/mdt_loader.c)).
  The extracted SQE starts with version word `0`, which fails upstream Linux's A630 minimum of
  `0x190` or patched-firmware marker ([`a6xx_gpu.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)).
  This SQE must not be started through a user-command path; the full driver remains gated until
  suitable SQE firmware is available. Qualcomm's A6xx KGSL loader explicitly routes A630 through
  the legacy raw-image path, writes it to GMU ITCM, and caps the image at `0x8000` bytes
  ([`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm-modules/graphics/%2B/refs/heads/android-msm-p11-5.15-tm-wear-kr3-dr-p11/adreno_a6xx_gmu.c)).
  The device's 32 KiB `a630_gmu.bin` matches that contract; actual HakiOS GMU startup still needs
  a controlled device run.
- Firmware has been extracted read-only from the Polaris Recovery vendor partition. The local
  `fw-gpu/` directory is ignored by Git; the blobs must stay out of the repository and be staged
  into the image only by the existing firmware packaging path.
- On 2026-11-11, the connected MIX 2S was in Recovery with its vendor partition mounted
  `ro,noload`. SHA-256 hashes for all seven A630 firmware files matched `fw-gpu/SHA256SUMS`.
  Its live device tree reported model `Xiaomi Technologies, Inc. Polaris MP v2.1`, chip ID
  `0x06030001`, and the GPU, GPUCC, GMU and SMMUv2 windows recorded below. KGSL reported
  `Adreno630v2`, a 257 MHz idle clock and available frequencies through 710 MHz. These are
  stock Recovery/Linux observations; the HakiOS diagnostic has not yet run on the device.
- The live Recovery tree's `/soc/qcom,kgsl-hyp` identifies `qcom,pil-tz-generic`, PAS ID `13`,
  firmware name `a630_zap`, and a `memory-region` phandle that resolves to the `no-map` GPU region
  at `0x8c415000/0x2000`. The matching Qualcomm SDM845 GPU DTS declares the same PAS ID, firmware
  name, and memory-region relationship
  ([`sdm845-gpu.dtsi`](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi)).
- Before an `adreno=` probe or `gpu=1` request maps any block, the SDM845 backend now checks the
  live device tree's Polaris GPU compatible/chip ID, the exact GPU/GPUCC/GMU/SMMUv2 `reg` windows,
  SMMU phandle and 3-level-table flags, `gfx3d_user` GPU offset, and the user/secure/GMU stream
  tuples. Missing or mismatched entries abort the request before GPU MMIO access. The SMMU probe
  uses the validated runtime stream IDs when hardware reports indexed stream routing.
- `adreno=6` now asks TrustZone whether the SCM availability query and PAS 13 calls are exposed,
  then asks whether GPU image 13 is supported. It does not submit a firmware header or image and
  does not write GPU registers. This query has not yet run under HakiOS on the phone.
- `adreno=7` now exercises PAS ZAP staging and cleanup: it requires the DT-resolved reserved
  region, affirmative PAS 13 support, the required SCM calls, and already-powered CX/GMU and
  GX/GFX3D clocks. It sends the ELF header plus `.b01` hash metadata to `PAS_INIT_IMAGE`, assigns
  the aligned image span with `PAS_MEM_SETUP`, copies `.b02`, then shuts PAS down. It deliberately
  defers `PAS_AUTH_AND_RESET` until the secure GPU SMMU route is verified. This write stage has not
  been invoked on the device; `ready()` remains false.

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

The device-tree `iommus` tuples resolve through phandle `0x204` to `arm,smmu-kgsl`, whose
`#iommu-cells` is 1. Their arguments are stream IDs, not context-bank numbers:

| Client | Stream IDs | Device-tree node |
|---|---:|---|
| 3D user | `0` | `qcom,kgsl-iommu/gfx3d_user` (`qcom,gpu-offset = 0x48000`) |
| 3D secure | `2`, `1` | `qcom,kgsl-iommu/gfx3d_secure` |
| GMU kernel | `5` | `qcom,gmu/gmu_kernel` |
| GMU user | `4` | `qcom,gmu/gmu_user` |

The SMMU node is marked `qcom,dynamic` and `qcom,use-3-lvl-tables`; the KGSL node also has
`qcom,protect = <0x40000 0xc000>`. These facts identify the stream assignments and table mode,
but they do not establish context-bank numbers, SMR masks, secure-context ownership, or whether
the active hypervisor lets this kernel program those streams. Those still need validation against
the matching Qualcomm KGSL/SMMU implementation and the live device before SMMU writes.
The child node's `qcom,gpu-offset = <0x48000>` is an offset into the GPU register view for
GPU-side access to that context bank; it is not a context-bank number or an offset to add to the
SMMU physical base. KGSL's device-tree binding describes this property and notes that some
targets instead use a separate SMMU register aperture ([binding](https://android.googlesource.com/kernel/msm/+/e92ad0d204333c5b5dceb9d1573a71a49a713db6/Documentation/devicetree/bindings/gpu/adreno-iommu.txt)).
The read-only probe decodes the SMMU ID registers and current stream-routing registers using the
ARM SMMUv2 register layout cross-checked against upstream Linux [`arm-smmu.h`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h)
and [`arm-smmu.c`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c).
The probe calculates the context-bank aperture as `2^(NUMPAGENDXB+1) * register-page-size` and
checks it against the device-tree `reg` extent before reading context registers.
The v2 SMMU at `0x05040000` is not interchangeable with the Redwood driver's SMMUv500 at
`0x03da0000`.

The local firmware set is `a630_gmu.bin`, `a630_sqe.fw`, and `a630_zap.{mdt,b00,b01,b02,elf}`.
The `mdt` describes three program headers, with a single loadable segment matching `b02`; the
signed metadata/other chunks still need to follow the exact Qualcomm MDT/PAS loading contract.
Do not assume that copying the Redwood `mdt` plus `b02` path is sufficient. Confirm SQE format
and version compatibility from the image header and A630-specific upstream/vendor loader before
starting the CP.

## MSM capability profile

The shared service's `GET_PARAM` answers come from the selected GPU profile. For Polaris, the
profile values now recorded in `hakios.adreno.c` are:

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
implementation behind that boundary. Polaris selects its own profile and a not-ready operations
table. Its software map, commit, unmap and cache-maintenance callbacks are implemented; power,
firmware startup, active SMMU context/stream attachment, command submission, fences, stop and
thermal hooks are still missing. The permanent `ready()` gate prevents calls from reaching the
unfinished path. No callback falls through into Redwood's SMMUv500 or register state.

GPU VA allocation and release now dispatch through the selected backend too. Polaris has a
software-only, locked 32-bit IOVA allocator: the lower 1 GiB is reserved for backend-owned
firmware/ring mappings, while client allocations use `[0x40000000, 0x100000000)`. It records each
allocation and accepts only exact frees. The new 4K LPAE stage-1 tables can describe those IOVAs,
but no context bank or stream is programmed, so the SMMU does not consume them and rendering
remains disabled.

The table descriptor format follows the existing project mapper and the ARM LPAE definitions in
upstream Linux [`io-pgtable-arm.c`](https://github.com/torvalds/linux/blob/master/drivers/iommu/io-pgtable-arm.c).
The Polaris probe uses SMMUv2 IDR2 to confirm 4K support and decode OAS using upstream Linux
[`arm-smmu.c`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c).
The operations table includes software map, commit, unmap, cache-maintenance and bounded ASID
invalidation callbacks; ASID invalidation is reachable only after a later bring-up stage marks a
verified context active. The runtime SMMU probe now reads its stream IDs from the validated DT
tuples instead of relying on a duplicate hardcoded list.

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
   unchanged profile. The initial dispatcher selects the SDM845 backend from the SoC peripheral
   base (`0x00100000`) and rejects unknown bases before any Redwood GPU access. The diagnostic now
   validates the runtime device tree's A630 chip ID, GPU/GPUCC/GMU/SMMU resource windows, and
   IOMMU stream tuples before MMIO; still verify the powered GPU's hardware identity before writes.
   **Gate:** both profiles build; selecting an unknown SoC touches no GPU register.
3. **Read-only bring-up.** The initial A630-specific, boot-argument-gated reads are in place.
   Refine them against hardware logs and add any required clock/precondition checks before adding
   writes. Keep reads and writes in separate gates, with each write preceded by logged
   preconditions and a verified readback.
   **Gate:** repeated read-only runs complete without a hypervisor trap or stale state.
4. **Power, clocks and secure firmware.** The A630 raw GMU image path and size limit are now
   source-confirmed, and the local image fits. The signed ZAP metadata/segment loader and PAS 13
   shutdown path are implemented behind explicit `adreno=7`, gated on live DT and already-on
   domains. Implement SDM845 CX/GX sequencing, clocks and required RPMh votes; then exercise the
   PAS stage, verify the secure stream/context, authenticate ZAP, and bring up GMU firmware/HFI.
   Clean up in reverse on every failure.
   **Gate:** GMU firmware reaches its documented ready state; SMMU and secure firmware report no
   faults; stop returns hardware to the loader's prior state.
5. **SMMUv2 and command processor.** The software 4K page-table mapper and its table-cache
   maintenance are implemented, with OAS and PTFS_4K checks taken from the read-only ID registers.
   The local firmware probe validates the ZAP MDT layout and SQE security floor; the extracted SQE
   currently fails that floor. Still implement and validate the v2 stream-match/context-bank
   attachment using Polaris DT assignments, find acceptable A630 SQE firmware, then bring up GMU
   and core mappings, ring, CP protection and fences and exercise the mapper on-device. Keep the
   backend disabled until those pieces are verified.
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

- The live SMMU SMR masks, stream-to-context-bank assignments, and whether the secure/GMU streams
  are accessible to this kernel under the phone's hypervisor. Runtime DT now confirms the IDs.
- The `adreno=6` PAS capability result and `adreno=7` ZAP authentication result on this boot
  environment. Qualcomm's MDT loader consumes the `.mdt` ELF header, the indexed `.b01` hash
  metadata, and `.b02` as the sole `PT_LOAD` segment; `.b00` and the combined `.elf` are not part
  of that loader path.
- The local A630 SQE is below upstream's A630 safety floor; find an acceptable firmware version,
  then validate its register protection table and the MSM submission path against the 630's
  GMEM/UBWC configuration.
- Whether TrustZone PAS calls for GPU firmware are permitted from this boot environment.
- Cache coherency, interrupt availability, hang recovery, DVFS/thermal policy, and full
  client-memory isolation on the actual phone.

These are gates to resolve with source evidence and controlled device runs, not values to infer
from Redwood or fill with guessed constants. The Dipper repository is only a comparison reference:
its Adreno source is substantially the same Redwood backend and does not supply an A630/SDM845
implementation.
