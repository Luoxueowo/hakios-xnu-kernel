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
- The Dipper reference was checked at upstream `main` commit `17077d07`. Its
  `xnu/files/bsd/dev/hakios.adreno.c` follows the Redwood/SC7280 register path; the Polaris
  implementation therefore stays in this repository's separate SDM845 backend, with Dipper used
  for lifecycle and service comparisons.
- `CONFIG_HAKIOS_ADRENO` remains disabled in `devices/polaris/configs/polaris.config` until the
  Polaris path has passed staged hardware bring-up.
- A separate `hakios.adreno.sdm845.c` probe is now wired into the driver configuration. On an
  SDM845 build, `adreno=1..4` reads the GPUCC, KGSL SMMU, GMU and (only when powered and clocked)
  3D core status; it checks the GCC GPU config-AHB clock before reading GPUCC and checks CX plus
  GPU MEMNOC before reading the SMMU. Step 2 also reports SMMU capabilities, the current
  SMR/S2CR stream routing (or the device-tree KGSL stream IDs when stream indexing is active),
  and the IDR1-sized context-bank window and its nonzero registers.
  It does not write registers or expose rendering. This probe has not been run on the phone yet.
- The A630 backend now has separate software stage-1 page-table roots for the 3D core,
  `gmu_kernel`, and `gmu_user`; table pages are suballocated from XNU pages, checked against the
  SMMU output-address size, and kept for the backend lifetime. The diagnostic's IDR2 read supplies
  the OAS and 4-KiB table capability. Shared `map/mapped/unmap` uses the 3D root. Leaf permissions
  are per address space: 3D and `gmu_user` use unprivileged mappings, while `gmu_kernel` keeps
  privileged mappings for HFI memory. `adreno=9` also allocates contiguous HFI memory, creates a
  write-combined CPU alias, initializes its legacy HFI table, and maps its five 4-KiB pages into the
  detached `gmu_kernel` software root at `0x60000000`. It also builds a detached AArch64 SMMUv2
  context-register image from the root PA, ASID, decoded OAS and IDR0 coherent-walk capability.
  Noncoherent walks use noncacheable outer-shareable TCR attributes while the mapper cleans table
  pages. No context bank, stream route, SMMU register, GMU interrupt, or firmware state is changed;
  these tables remain detached and `ready()` remains false. This preparation step has not been run
  on the phone.
- `adreno=5` checks the staged A630 firmware and PAS memory description without writing hardware.
  The local set contains a 32 KiB GMU image and a 6,684-byte ZAP MDT with three ELF program
  headers; its single relocatable `PT_LOAD` segment matches `a630_zap.b02` at 1,968 bytes. The
  header is 148 bytes and its 6,536-byte signed metadata segment is `a630_zap.b01`; both copies
  match Qualcomm's split-firmware contract
  ([`mdt_loader.c`](https://android.googlesource.com/kernel/common/+/d3d009847a119db499094aca61bd8a9935bc94ef/drivers/soc/qcom/mdt_loader.c)).
  Upstream MSM strips the first 4-byte file prefix before creating the SQE GPU buffer
  ([`adreno_gpu.c`](https://linux.googlesource.com/linux/kernel/git/torvalds/linux/+/1f57f68c4dd101e5e8ffc9ffa6428f45bcdd776a/drivers/gpu/drm/msm/adreno/adreno_gpu.c)).
  The extracted file prefix is `0`; the actual payload version word is `0x016ee19a` (low 12 bits
  `0x19a`), and its patch marker also matches upstream's A630 acceptance rule
  ([`a6xx_gpu.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)).
  The SQE therefore passes that firmware version gate. This alone does not establish CP register
  protection or make command submission safe; the backend remains disabled until those and the
  runtime bring-up gates are verified. Qualcomm's A6xx KGSL loader explicitly routes A630 through
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
  live device tree's Polaris GPU compatible/chip ID, the exact GPU/GPUCC/GFXCC/GMU/SMMUv2 `reg`
  windows, GPU and GMU clock names/specifiers/provider phandles, shared GPU/GMU supply domains,
  GPUCC CX/MX and GFXCC GFX regulator links, SMMU phandle and 3-level-table flags, `gfx3d_user`
  GPU offset, the exact KGSL protect aperture, HYP secure-allocation and ATOS policy, static-CB
  absence, and the user/secure/GMU stream tuples. Missing or mismatched entries abort the request
  before GPU MMIO access. The SMMU probe uses the validated runtime stream IDs when hardware
  reports indexed stream routing.
- The same preflight now validates the Polaris Apps RPMh DRV2 node, its separate DRV/TCS windows,
  its active/sleep/wake/control TCS counts, and the `cx.lvl`/`gfx.lvl` regulator mailbox links.
  `adreno=8` runs the existing read-only steps 1–6, skips PAS staging step 7, then reads DRV2
  revision/configuration/status and the AOP CMD_DB CX/GFX level tables. It accepts only the
  register layouts documented for RPMh RSC v2.7 and v3.0, and sends no request or TCS write.
  `adreno=9` runs steps 1–6, skips PAS staging, and prepares the detached HFI allocation and GMU
  kernel software page tables. This allocates kernel memory but does not write SMMU/GMU registers,
  send an HFI message, or start firmware. Neither diagnostic has run under HakiOS on the phone.
  `adreno=10` is an explicit GMU ITCM load stage. It runs read-only GPUCC, GMU, core, and firmware
  checks, requires CX/GMU clock on with the 3D core off and CM3 already held in reset, loads the
  raw A630 image word-by-word, and verifies the ITCM readback. It leaves reset asserted; it does
  not write the SMMU, release CM3, start HFI, or enable rendering. The load contract follows
  Qualcomm's A630 `_load_legacy_gmu_fw()` path in
  [`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/815ef3fd67546132532651279885563ff35a4605/drivers/gpu/msm/adreno_a6xx_gmu.c).
- `adreno=6` now asks TrustZone whether the SCM availability query and PAS 13 calls are exposed,
  then asks whether GPU image 13 is supported. It does not submit a firmware header or image and
  does not write GPU registers. This query has not yet run under HakiOS on the phone.
- `adreno=7` now exercises PAS ZAP staging and cleanup: it requires the DT-resolved reserved
  region, affirmative PAS 13 support, the required SCM calls, and already-powered CX/GMU and
  GX/GFX3D clocks. It sends the ELF header plus `.b01` hash metadata to `PAS_INIT_IMAGE`, assigns
  the aligned image span with `PAS_MEM_SETUP`, copies `.b02`, then shuts PAS down. It deliberately
  defers `PAS_AUTH_AND_RESET` until the secure GPU SMMU route is verified. This write stage has not
  been invoked on the device; `ready()` remains false.
- `adreno=11` is an explicit SMMUv2 write stage. It runs steps 1–6, prepares the detached HFI
  allocation/page tables, then performs a fresh read-only SMMU scan. It proceeds only when the
  runtime DT allows dynamic non-static routes, the SMMU client interface is enabled, the GMU is
  clocked with CM3 held in reset, the 3D core is off, and the selected GMU-kernel context and
  SID5 route still match the blank/untranslated snapshot. It installs only the GMU-kernel
  stage-1 context and SID5 route, verifies register readbacks, and attempts rollback on failure.
  It does not attach SID4 or SID0, authenticate ZAP, start the GMU/HFI, or enable rendering. This
  stage has not been run on the phone because SMMU write ownership under the active hypervisor is
  still unverified; `ready()` remains false.

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

The runtime DT carries `qcom,dynamic`, `qcom,use-3-lvl-tables`, and `qcom,disable-atos` on the
SMMU node. The KGSL node has `qcom,protect = <0x40000 0xc000>` and
`qcom,hyp_secure_alloc`; `qcom,enable-static-cb` is absent. Runtime validation now checks those
exact Polaris properties before the diagnostic reads registers. Qualcomm defines static-CB mode
as TrustZone-programmed SMR/S2CR state that the OS must not alter ([binding](https://android.googlesource.com/kernel/msm/+/975cca6a674717743cd614b8728cf27efb0b9215/Documentation/devicetree/bindings/iommu/arm%2Csmmu.txt));
the backend refuses its presence. ATOS remains disabled and is never probed.

The Qualcomm SDM845 GPU DT source comments that CB5 (ATOS) and CB5/6/7 are HYP-protected
([SDM845 GPU DT](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/arch/arm64/boot/dts/qcom/sdm845-gpu.dtsi)).
Qualcomm's Adreno SMMU code assigns CB0 to the GPU device
([Adreno SMMU context-bank allocator](https://android.googlesource.com/kernel/common/+/748bf810c00543177d454aa7370ea1ddfe08dd06/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c)).
The `qcom,protect` numeric pair describes a protected GPU register aperture; its values are not
context-bank indices. HakiOS therefore treats CB0 as GPU-reserved and excludes CB5-7 from GMU
candidates based on the Qualcomm target source, while keeping the secure GPU SIDs 1/2 untouched.
These source rules do not establish the phone's live stream-to-context assignment or prove that
the active hypervisor permits HakiOS to program any of these routes.
The child node's `qcom,gpu-offset = <0x48000>` is an offset into the GPU register view for
GPU-side access to that context bank; it is not a context-bank number or an offset to add to the
SMMU physical base. KGSL's device-tree binding describes this property and notes that some
targets instead use a separate SMMU register aperture ([binding](https://android.googlesource.com/kernel/msm/+/e92ad0d204333c5b5dceb9d1573a71a49a713db6/Documentation/devicetree/bindings/gpu/adreno-iommu.txt)).
The read-only probe decodes the SMMU ID registers and current stream-routing registers using the
ARM SMMUv2 register layout cross-checked against upstream Linux [`arm-smmu.h`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h)
and Qualcomm's [`arm-smmu.c`](https://android.googlesource.com/kernel/msm/+/ac6e850d96251d8cd68bd1049009a04e9d5354a6/drivers/iommu/arm-smmu.c).
For the DT-validated stream IDs, step 2 correlates stream-match IDs/masks to S2CR slots (or reads
the SID-indexed S2CR directly), then reports the unique route, target CB's CBAR type, CBA2R width,
and SCTLR translation state. Multiple matching SMRs are reported as ambiguous. This remains a
read-only snapshot; it does not claim that a route is safe to adopt or program. The probe calculates
the context-bank aperture as `2^(NUMPAGENDXB+1) * register-page-size` and checks it against the
device-tree `reg` extent before reading context registers.
Step 2 now also prints a read-only attach plan. It reserves Qualcomm's CB0 for GPU SID0, never
plans secure GPU SIDs1/2, and considers only CB1-4 for separate GMU-kernel and GMU-user contexts.
Candidate GMU banks must have zero in the sampled CBAR/CBA2R and architected stage-1 context
registers (SCTLR, ACTLR, TTBR0/1, TCR/TCR2, CONTEXTIDR, MAIR0/1 and FSR), and no translation
route may target them. With stream matching, the planner records invalid SMR slots; without it,
it scans every SID-indexed S2CR entry that fits the mapped register window and refuses an
incomplete scan. Existing translation routes, ambiguous matches, masked SMRs that could cover
on the target streams, ambiguous matches, masked SMRs that could cover other clients, incomplete
snapshots, and static-CB policy all prevent a complete plan. The result is only a candidate: it
does not attach a context or write any SMMU register, and live ownership and HYP access still need
confirmation before a write path can be enabled.

A read-only check from Recovery found KGSL `active_cnt=0` and an empty KGSL context list. The
available IOMMU debugfs page-table view exposed only its `pages` counter, not the stream-to-CB
assignment. The same Recovery DT snapshot had the Polaris `qcom,protect` pair, HYP secure
allocation and disabled-ATOS properties, with no `qcom,enable-static-cb`. This corroborates the
runtime-DT policy checks but does not identify live context-bank ownership; no HakiOS `adreno=`
stage was invoked and no Recovery MMIO/SMMU writes were made.
The v2 SMMU at `0x05040000` is not interchangeable with the Redwood driver's SMMUv500 at
`0x03da0000`.

The runtime clock tree is split across four providers: the core's `core_clk` comes from GFXCC,
the GMU and RBBM timer clocks come from GPUCC, the GPU/GMU AXI and MEMNOC clocks come from GCC,
and `l3_vote` comes from CPUCC. The GPU and GMU nodes' `vddcx`/`vdd` supplies resolve to the CX/GX
GDSC nodes at `0x0509106c`/`0x0509100c`; their parent supplies resolve to GPUCC's CX rail and
GFXCC's GFX rail. GPUCC also names a separate MX rail. The preflight now resolves these GDSC
phandles, parent links, controller rails, and every clock tuple before allowing later stages.
Qualcomm's SDM845 GPUCC source marks CX GDSC as votable and gives GX GDSC a no-op CPU power-on
callback because the GMU controls GX; HakiOS must preserve that ownership rather than force the GX
switch from the CPU
([`gpucc-sdm845.c`](https://android.googlesource.com/kernel/common/+/b4ec805464a4a0299216a003278351d0b4806450/drivers/clk/qcom/gpucc-sdm845.c)).

The Polaris runtime tree exposes Apps RSC DRV2 as `/soc/mailbox@179e0000` (`qcom,tcs-drv`):
DRV registers at `0x179e0000/0x100`, TCS registers at `0x179e0d00/0x3000`, `drv-id = 2`, and
active/sleep/wake/control counts of `2/3/3/1`. Both RPMh regulators used by the GPU point to
that mailbox provider on channel 0. Qualcomm's SDM845 DTS and RPMh binding define the same DRV2
layout and TCS classes ([SDM845 DTS](https://android.googlesource.com/kernel/common/+/4b95dc87362aa57bdd0dcbad109ca5e5ef3cbb6c/arch/arm64/boot/dts/qcom/sdm845.dtsi),
[`qcom,rpmh-rsc.yaml`](https://android.googlesource.com/kernel/common/+/4cb1fc6fffe4910845e183d1a2dfe9509ba1062c/Documentation/devicetree/bindings/soc/qcom/qcom,rpmh-rsc.yaml)).
The AOP command database's standard reserved range is `0x85fe0000/0x20000`. Polaris' runtime DT
folds that range into a larger `no-map` `removed_region`; its `/soc/qcom,cmd-db@861e0000` marker
has `reg = <0x0c3f000c 8>`, while Recovery `/proc/iomem` reports the mapped CMD_DB at
`0x85fe0000-0x85fe0dc0`. The preflight checks that the standard CMD_DB range lies inside the
runtime reservation before mapping it for the explicit read-only probe. Qualcomm's CMD_DB parser defines
the header, resource tables, and auxiliary level data consumed by that probe
([SDM845 DTS](https://android.googlesource.com/kernel/common/+/f40f97aaf7fa6222f4ec073c24fb14f04ffb6f80/arch/arm64/boot/dts/qcom/sdm845.dtsi),
[`cmd-db.c`](https://android.googlesource.com/kernel/common/+/29d66b3902962d0a89408abaf6d24a3df5eef518/drivers/soc/qcom/cmd-db.c)).
The same probe now validates and reports the runtime GPU and GMU operating-point arrays. Recovery's
live tree has eight GPU entries (710, 675, 596, 520, 414, 342, 257, and 0 MHz) with per-entry bus
frequency/minimum/maximum indices, and three GMU entries (0, 200, and 500 MHz). These values are
read from DT; the driver does not assume that the checked-in or upstream frequency table matches
the live tree.

The Qualcomm A6xx KGSL flow uses the `gfx.lvl`/`cx.lvl` ARC tables to build GMU voltage votes from
GPU/GMU operating points, then sends performance and bandwidth indices through the GMU DCVS/HFI
path. This indicates the GPU-specific ARC vote owner is the GMU; HakiOS should not duplicate those
votes with CPU-side active TCS requests. Qualcomm's A630v2 target identifies GMU 1.3 and enables
RPMh, while KGSL labels the A630/A615 firmware path as legacy HFI. HakiOS therefore needs the
A630 legacy queue/message contract, not assumptions taken from newer HFI variants
([`kgsl_gmu.c`](https://android.googlesource.com/kernel/msm/+/9b7afd65fddcb21b51a2db8f4a82b08942225263/drivers/gpu/msm/kgsl_gmu.c),
[`adreno-gpulist.h`](https://android.googlesource.com/kernel/msm.git/+/7bbb67403d95f83d95e0ea464f972f247475d0d4/drivers/gpu/msm/adreno-gpulist.h),
[`kgsl_hfi.c`](https://android.googlesource.com/kernel/msm/+/541919e72ee1156c0054952dd11d5066372095a8/drivers/gpu/msm/kgsl_hfi.c),
[`sdm845-v2.dtsi`](https://android.googlesource.com/kernel/msm/+/5f65a2ac2505d5e0c122d5e54a58194c873b426c/arch/arm64/boot/dts/qcom/sdm845-v2.dtsi)).
The current step 8 remains read-only: it discovers the tables and TCS state but sends neither an
HFI message nor an RPMh request.

The A630 legacy HFI v1 wire format is now isolated in `drivers/gpu/hakios.adreno.hfi.h`. It
provides bounded shared-table initialization, command queue writes, response queue reads,
sequence-number generation, the GMU init command, and v1 ACK matching. The queue header is 12
dwords, the queue table is 54 dwords, and each queue contains 1024 dwords. A630's legacy queue IDs
are 0/4/5/1 for command/message/debug/dispatch; the dispatch queue remains disabled. The host
allocation is page-rounded to 20 KiB and its GMU virtual address belongs to the noncached kernel
range beginning at `0x60000000`. `adreno=3` checks and reports this protocol layout. The HFI
helpers manage the wire format only; step 9 supplies the detached software mapping, while context
attachment, register programming, cache-policy setup, interrupts, and HFI startup remain pending.
The queue-table header still reports four active slots with dispatch disabled in its queue header,
matching Qualcomm's A630 `hfi_init` (`num_active_q = HFI_QUEUE_MAX`); HakiOS preserves that v1
contract ([`kgsl_hfi.c`](https://android.googlesource.com/kernel/msm/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_hfi.c)).

Qualcomm's matching A630 KGSL implementation gives the GMU separate `gmu_user` and `gmu_kernel`
SMMU contexts and allocates HFI memory through the GMU-kernel context. Polaris preflight currently
validates their stream IDs (4 and 5), but does not discover or establish either context bank. The
HFI queue must be mapped into the verified GMU-kernel address space; the GPU user page table cannot
be assumed to serve the GMU stream. See the primary Qualcomm [`kgsl_hfi.h`](https://android.googlesource.com/kernel/msm.git/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_hfi.h) and
[`kgsl_gmu.c`](https://android.googlesource.com/kernel/msm.git/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_gmu.c) sources.

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
verified context active. The detached HFI preparation now forms the stage-1 register image with
the measured output-address encoding, AArch64 ASID tags in TTBR0/TTBR1, 39-bit input size and
walk-coherency attributes. Explicit `adreno=11` now installs that image only for the GMU-kernel
bank, writes the S2CR before enabling a new SMR, and keeps `SCTLR.M` last in context setup. It
rechecks the selected context and route, verifies readback, and rolls back the route/context if a
write is rejected. The register order follows upstream Linux's SMMUv2 context-bank setup
([`arm-smmu.c`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c)).
The runtime SMMU probe now reads its stream IDs from the validated DT tuples instead of relying on
a duplicate hardcoded list. Only source-level build evidence exists for this write stage; it has
not been executed on Polaris, and hypervisor write access remains an unresolved requirement.

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
   validates the runtime device tree's A630 chip ID, GPU/GPUCC/GFXCC/GMU/SMMUv2 resource windows,
   clock providers/specifiers, supply links, and IOMMU stream tuples before MMIO; still verify the
   powered GPU's hardware identity before writes.
   **Gate:** both profiles build; selecting an unknown SoC touches no GPU register.
3. **Read-only bring-up.** The initial A630-specific, boot-argument-gated reads are in place.
   Refine them against hardware logs and add any required clock/precondition checks before adding
   writes. Keep reads and writes in separate gates, with each write preceded by logged
   preconditions and a verified readback.
   **Gate:** repeated read-only runs complete without a hypervisor trap or stale state.
4. **Power, clocks and secure firmware.** The A630 raw GMU image path and size limit are now
   source-confirmed, and the local image fits. The signed ZAP metadata/segment loader and PAS 13
   shutdown path are implemented behind explicit `adreno=7`, gated on live DT and already-on
   domains. The Apps RSC DRV2 and AOP CMD_DB windows and GPU/GMU operating-point tables are now
   validated from runtime DT; an explicit read-only step 8 inspects the TCS and CX/GFX ARC tables.
   Implement SDM845 CX/GX sequencing and clocks, then start the A630 GMU and route GPU DCVS/ARC
   votes through its verified HFI path. Exercise the PAS stage, verify the secure stream/context,
   authenticate ZAP, and bring up GMU firmware/HFI. Clean up in reverse on every failure.
   **Gate:** GMU firmware reaches its documented ready state; SMMU and secure firmware report no
   faults; stop returns hardware to the loader's prior state.
5. **SMMUv2 and command processor.** The software 4K page-table mapper and its table-cache
   maintenance are implemented, with OAS and PTFS_4K checks taken from the read-only ID registers.
   The local firmware probe validates the ZAP MDT layout and applies the upstream SQE check to the
   payload after its 4-byte file prefix; the extracted SQE passes the version floor. The read-only
   SMMUv2 probe scans stream routes and context registers and produces conditional CB/SMR
   candidates; step 9 builds a detached GMU-kernel context image and step 11 can install just that
   context/SID route with readback and rollback. Neither step 11 nor any SMMU write stage has been
   run on the phone. Do not invoke step 11 until HYP ownership and write access are verified from
   a controlled HakiOS boot. Then verify the GMU-kernel attach and extend it to the GMU-user and
   3D contexts with verified attributes, validate A630 CP register protection, bring up GMU and
   core mappings, ring and fences, and exercise the mapper on-device. Keep the backend disabled
   until those pieces are verified.
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
- The extracted A630 SQE passes upstream's version floor after the 4-byte file prefix is removed;
  its register-protection table and the MSM submission path still need validation against the
  630's GMEM/UBWC configuration.
- Whether TrustZone PAS calls for GPU firmware are permitted from this boot environment.
- Whether A630 GMU HFI initializes and accepts DCVS requests under HakiOS and the Polaris
  hypervisor, and whether the runtime `gfx.lvl`/`cx.lvl` tables pair correctly with the live GPU/GMU
  operating points. The GPU backend has issued no HFI or RPMh writes.
- Cache coherency, interrupt availability, hang recovery, DVFS/thermal policy, and full
  client-memory isolation on the actual phone.

These are gates to resolve with source evidence and controlled device runs, not values to infer
from Redwood or fill with guessed constants. The Dipper repository is only a comparison reference:
its Adreno source is substantially the same Redwood backend and does not supply an A630/SDM845
implementation.
