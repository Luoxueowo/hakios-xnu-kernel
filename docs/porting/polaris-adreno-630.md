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
- The supplied [Dipper reference](https://github.com/manysuq/hakios-xnu-kernel-dipper) was checked
  at `main` commit `17077d07`. Despite the repository name, the checked-in tree contains
  `devices/poco-x5-pro/`, and its
  [`hakios.adreno.c`](https://github.com/manysuq/hakios-xnu-kernel-dipper/blob/17077d07e0b0b6a09760f95f4ff544c3387e2460/xnu/files/bsd/dev/hakios.adreno.c)
  identifies the POCO X5 Pro / Redwood Adreno 642L, chip ID `0x06030500`, and `a660_*` firmware.
  It does not contain a Polaris A630 backend. Use it only to compare general HakiOS service and
  lifecycle structure; its register, clock, power, firmware, PAS, and SMMU assumptions do not
  apply here. The Polaris backend stays separate and follows the MIX 2S tree plus A630-specific
  Qualcomm sources.
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
  privileged mappings for GMU shared memory. `adreno=9` allocates one contiguous 40 KiB region and
  creates a write-combined CPU alias. It initializes the legacy HFI table at GMU VA `0x60000000`,
  places the 16 KiB crash-dump area at `0x60100000` and the 4 KiB GMU log at `0x60200000`, then maps
  all 10 pages into the detached `gmu_kernel` software root. It also builds a detached AArch64 SMMUv2
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
  into the image only by the existing firmware packaging path. `devices/polaris/pull-firmware.sh`
  verifies that the source is the vendor ext4 block device mounted read-only, supports the Recovery
  `ro,noload` mount when needed, and records hashes in the ignored directory.
- On 2026-11-11, the connected MIX 2S was in Recovery with its vendor partition mounted
  `ro,noload`. SHA-256 hashes for all seven A630 firmware files matched `fw-gpu/SHA256SUMS`.
  Its live device tree reported model `Xiaomi Technologies, Inc. Polaris MP v2.1`, chip ID
  `0x06030001`, and the GPU, GPUCC, GMU and SMMUv2 windows recorded below. KGSL reported
  `Adreno630v2`, `default_pwrlevel=6`/initial-pwrlevel 6 (257 MHz), and frequencies through
  710 MHz. These are stock Recovery/Linux observations; the HakiOS diagnostic has not yet run on
  the device.
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
  revision/configuration/status, the AOP CMD_DB CX/GFX level tables, and GPU/GMU bus vectors from
  runtime DT. It accepts only the register layouts documented for RPMh RSC v2.7 and v3.0, verifies
  all 13 `grp3d` and 2 `cnoc` usecases plus every GPU powerlevel's bus indices, and sends no RPMh
  request or TCS write. This validates source data only; it does not generate the per-usecase TCS
  commands required by the GMU HFI bandwidth table.
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
  the aligned image span with `PAS_MEM_SETUP`, copies `.b02`, then shuts PAS down without
  authentication. Step 19 restages and authenticates ZAP through `PAS_AUTH_AND_RESET` before
  releasing SQE halt, matching Qualcomm's A630 `a6xx_microcode_load()` order. HakiOS still keeps
  the GPU in secure mode until a later `CP_SET_SECURE_MODE(0)` stage. This write stage has not
  been invoked on the device;
  `ready()` remains false.
- `adreno=11` is an explicit SMMUv2 write stage. It runs steps 1–6, prepares detached HFI buffers
  and separate GPU, GMU-kernel, and GMU-user page-table roots, then performs a fresh read-only SMMU
  scan. It proceeds only when the runtime DT allows dynamic non-static routes, the SMMU client
  interface is enabled, the GMU is clocked with CM3 held in reset, the 3D core is off, and two
  clear/unrouted GMU context banks plus the SID5/SID4 routes still match the scan. It installs
  independent contexts and routes for GMU-kernel SID5 and GMU-user SID4, verifies register
  readbacks, and attempts rollback on a failed individual attach. It also installs 3D SID0 at
  Qualcomm's fixed CB0 only when CB0 is blank/unrouted and SID0 has a safe route candidate. The 3D
  and GMU-user roots start empty; HFI buffers remain in the GMU-kernel context. This stage has not
  been run on the phone because SMMU write ownership under the active hypervisor is still
  unverified; `ready()` remains false.
- `adreno=12` is a separate explicit GMU/HFI bring-up stage. It runs steps 1–6, prepares and maps
  HFI, 16 KiB dump, and 4 KiB log buffers, loads and verifies the raw A630 GMU image, refreshes the
  SMMU snapshot, then installs the GMU-kernel SID5 and empty GMU-user SID4 contexts, and adds 3D
  SID0/CB0 when that fixed context is blank and unrouted. It programs the A6xx TCM-boot, HFI
  table, log, SFR chip-ID, cache, power-collapse baseline, and AHB-fence registers; submits the
  DT-selected temporary DDR vote through Apps RSC active TCS0 when its bus index is nonzero;
  releases CM3; polls the
  firmware-ready status; requests the DT-selected initial GX/MX ARC level through the
  A630 boot/slumber OOB registers; initializes the HFI queue; sends the legacy five-dword
  `GMU_INIT`; then sends `FW_VERSION`, checks the matching ACK, and requires A630 GMU 1.3 or newer
  within major version 1. Runtime validation requires the initial powerlevel to be a non-idle row;
  the current Recovery tree selects powerlevel 6 (257 MHz). The OOB path checks register readbacks
  and the matching acknowledgement, and later startup failures clear it before reset. The DDR
  request requires Apps RSC DRV2 v2.7, an idle TCS0 with no enabled commands, and enough command
  slots; it is serialized with other HakiOS RPMh users through `hakios_rsc_lock`, waits for each
  BCM VCD commit, and checks command statuses. A busy or unsupported RSC is refused without a
  request, and a still-busy timed-out TCS is left untouched. Step 12 keeps the host vote because it
  does not yet hand bandwidth ownership to GMU DCVS. If SID0 is not eligible, it leaves that route
  untouched. It does not load or authenticate ZAP, submit HFI
  performance/bandwidth tables or default DCVS, start command processing, or enable rendering. On
  timeout or an incompatible version after CM3 release, it reasserts reset and marks the backend
  faulted while leaving GMU SID5/SID4 attached.
  This stage has not been invoked on the phone. The sequence follows Qualcomm's A6xx GMU TCM boot
  and HFI queue setup plus its A630 legacy `GMU_INIT`/`FW_VERSION` path
  ([`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/815ef3fd67546132532651279885563ff35a4605/drivers/gpu/msm/adreno_a6xx_gmu.c),
  [`kgsl_hfi.c`](https://android.googlesource.com/kernel/msm/%2B/541919e72ee1156c0054952dd11d5066372095a8/drivers/gpu/msm/kgsl_hfi.c),
  [`A630 GMU profile`](https://android.googlesource.com/kernel/msm/%2B/9b7afd65fddcb21b51a2db8f4a82b08942225263/drivers/gpu/msm/adreno-gpulist.h),
  [`A6xx boot rail OOB`](https://android.googlesource.com/kernel/msm/%2B/b64dfa3b0499765d780d4583b1e74af013614ca7/drivers/gpu/msm/adreno_a6xx.c),
  [`A6xx GMU registers`](https://android.googlesource.com/kernel/msm/%2B/33ba973747392d08905b14b9d47a1bfb9ffb20d0/drivers/gpu/msm/a6xx_reg.h)).
- `adreno=13` reuses the step 12 GMU/HFI startup, then builds and sends only the A630 v1
  `H2F_MSG_PERF_TBL`. It reads `gfx.lvl`, `cx.lvl`, and `mx.lvl` from CMD_DB, maps Polaris GPU
  and GMU frequencies to SDM845 voltage corners, and packs the ARC indices and kHz frequencies
  into the fixed 43-dword message. Startup also holds the step 12 temporary Apps RSC DDR vote;
  without a bandwidth table and default DCVS handoff, this stage leaves a nonzero vote active. GPU rows
  follow KGSL order (idle through 710 MHz); the live device-tree order is reversed while building
  the table. It does not send `BW_VOTE_TBL` or an additional RPMh TCS request. Qualcomm's A630 flow
  omits `H2F_MSG_START`, which the A6xx driver sends only
  for A640. If construction or acknowledgement fails, it reasserts
  CM3 reset, marks the backend faulted, and leaves GMU SID5/SID4 attached. This write stage has
  not been run on the phone.
- `adreno=14` repeats the guarded GMU/HFI startup and its temporary Apps RSC DDR vote, sends
  `PERF_TBL`, then builds and sends the A630 v1 `H2F_MSG_BW_VOTE_TBL`. The builder validates the
  runtime bus graph and Apps RSC BCM
  links, obtains BCM addresses/widths/unit sizes/clock domains from CMD_DB, and converts each DT
  bus vector into sorted TCS commands. The runtime Recovery graph routes GPU-to-DDR through
  `mas-qxm-gpu → slv-qns-llcc → mas-llcc-mc → slv-ebi` (SH0, MC0, ACV), and GPU-to-CNOC through
  `mas-qxm-gpu → slv-qns-memnoc-snoc → mas-qnm-memnoc → slv-qns-cnoc` (SH2, SN15, SN3). It
  mirrors Qualcomm's bus aggregation/division, BCM commit/valid encoding, VCD ordering, and A630
  KGSL HFI wait-mask construction; the direct TCS request derives its wait mask from the
  per-command commit flags. The HFI payload has 13 DDR rows and 2 CNOC rows within the A630
  fixed-size limits. The stage sends legacy `H2F_MSG_TEST` to close HFI messaging, then uses the
  A630 register-based DCVS OOB path to request the validated DT initial GPU powerlevel and its
  `qcom,bus-freq` row. It reverses the DT powerlevel index into HFI order and checks the DCVS ACK
  and return register. The Recovery default is powerlevel 6 (257 MHz), read from DT rather than
  hard-coded. The initial boot/slumber OOB rail request remains active for the later GPU-start
  phase, matching KGSL's lifecycle. Polaris DT sets `qcom,gpu-quirk-hfi-use-reg`, the upstream
  condition for the register-based DCVS path. A630 does not use the A640-only `H2F_MSG_START`.
  The DCVS frequency vote uses the 4-bit `OPTION_AT_LEAST` field and the bus index is encoded in
  `gpu_bw_vote.bw_idx` bits 0–7, matching Qualcomm's A630 4.9 HFI_USE_REG path. After that DCVS
  acknowledgement, HakiOS releases any temporary vote through Apps RSC TCS0 with the zero-bandwidth
  row; GMU remains the owner of HFI-described runtime votes. The TCS write path accepts only Apps RSC
  DRV2 v2.7, protects the shared sequencer with `hakios_rsc_lock`, and checks TCS completion and
  per-command status. ZAP authentication, command processor startup, full
  stop/thermal handling, and rendering remain unavailable. Rejection of either table,
  `HFI_TEST`, or initial DCVS reasserts CM3 reset and marks the backend faulted. This write stage has
  not been run on the phone.
- `adreno=15` runs read-only steps 1–6, then builds the A630 `CP_ME_INIT` packet (12 dwords) and
  the `CP_SET_SECURE_MODE(0)` packet (2 dwords) in local stack buffers and prints them for review.
  The `CP_ME_INIT` ordinals follow the A630 KGSL 4.9 mask and values; the type-7 headers use
  Qualcomm's opcode/count parity encoding. It does not map SQE or ring memory, write GPU/SMMU
  registers, issue PAS calls, or start the CP. This is packet construction only, not proof that the
  GPU accepts the packets
  ([`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c),
  [`adreno_pm4types.h`](https://android.googlesource.com/kernel/msm/%2B/android-8.1.0_r0.78/drivers/gpu/msm/adreno_pm4types.h)).
- `adreno=16` runs read-only steps 1–6, then allocates and maps the validated SQE payload (after
  its 4-byte file prefix), a 32 KiB CP ring containing the 12-dword `CP_ME_INIT`, and a one-page
  RPTR scratch buffer into the detached GPU SID0 page table. The SQE and ring are GPU read-only;
  the RPTR page is GPU writable. It cleans the CPU cache lines and commits the software page tables,
  but does not attach SID0, write SMMU/GPU registers, start the SQE/CP, call PAS, or enable rendering.
  Buffers remain allocated for review and are released by the backend stop path only when both the
  3D context and core are inactive. The 32 KiB ring size follows the A630 KGSL definitions
  ([`kgsl.h`](https://android.googlesource.com/kernel/msm.git/%2B/a11d0d49d2b949d401f8899369db331102d23cc3/drivers/gpu/msm/kgsl.h),
  [`adreno_ringbuffer.h`](https://android.googlesource.com/kernel/msm/%2B/7a5cb50be049b0de03e13c98b65cb3770ad15481/drivers/gpu/msm/adreno_ringbuffer.h));
  this stage has not been run under HakiOS on the phone.
- `adreno=17` is an explicit hardware stage. It requires the GPU core initially off and
  SID0 detached, prepares the step 16 SQE/ring/RPTR mappings, starts the GMU/HFI with its separate
  SID5/SID4 contexts, sends and verifies `PERF_TBL`, `BW_VOTE_TBL`, `HFI_TEST`, and initial DCVS,
  then releases the temporary host DDR vote. With those prerequisites satisfied, it attaches the
  populated GPU SID0 context, requests GX power using the A630 `OOB_GPU_SET_MASK` handshake, and
  verifies the GX GDSC plus GFX3D clock readback. It leaves the GPU OOB request active to hold the
  core for a later initialization stage. It deliberately stops before A6xx core setup, ring/SQE
  register writes, `CP_ME_INIT`, ZAP authentication, or rendering. The OOB bit/ack protocol and
  ordering follow Qualcomm's A630 KGSL [`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c)
  and [`adreno.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno.c).
  This stage adds hardware writes but has not been run on the phone; `ready()` remains false.
- `adreno=18` repeats the guarded GMU/HFI, SID0 and GX startup, then ports Qualcomm's A630 core
  baseline: the full HWCG table and GMU SPTPRAC ordering, A630 VBIF settings, GPU busy counter,
  UCHE trap/write-through/1-MiB GMEM ranges, CP thresholds and pool size, address-mode attributes
  derived from the runtime `highest-bank-bit`, `min-access-length` and `ubwc-mode`, hang detection,
  and the A630 CP register-protection table. It also converts Polaris's validated
  `qcom,protect = <0x40000 0xc000>` aperture using KGSL's IOMMU encoding and adds its SMMU range
  to the 27 fixed A630 protection groups, for 28 of the 32 available descriptors. Selected core,
  VBIF, UCHE, CP and protection registers are read back; a mismatch faults the backend and leaves
  GPU OOB asserted for diagnosis. The register sequence follows Qualcomm's A630
  [`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c)
  and [`kgsl_iommu.c`](https://android.googlesource.com/kernel/msm/%2B/f39cb888297d4560766734fe0c42177714f8f9d5/drivers/gpu/msm/kgsl_iommu.c).
  It stops before SQE/ring register programming, `CP_ME_INIT`, ZAP authentication or submission.
  This write stage has not been run on the phone; `ready()` remains false.
- `adreno=19` repeats the guarded GMU/GX startup and step 18 core setup, programs the SQE,
  read-only 32-KiB CP ring and writable RPTR page, then restages the validated ZAP image and calls
  PAS `AUTH_AND_RESET` before releasing SQE halt and publishing the 12-dword `CP_ME_INIT` packet.
  An authenticated image remains active until GPU shutdown. It verifies
  the programmed IOVAs and ring control, verifies the published WPTR, then waits up to two seconds
  for RPTR to retire the packet, RBBM CP/AHB busy bits to clear, and CP error, hardware-fault and
  protection status to stay clear. Failures fault the backend and retain buffers and power for
  diagnosis. This follows Qualcomm's A630 `a6xx_rb_start()` / `a6xx_microcode_load()` /
  `a6xx_send_cp_init()` order in [`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c) and
  the ring-size definition in [`adreno_a6xx.h`](https://android.googlesource.com/kernel/msm/%2B/33ba973747392d08905b14b9d47a1bfb9ffb20d0/drivers/gpu/msm/adreno_a6xx.h).
  Secure mode remains enabled; `CP_SET_SECURE_MODE(0)`, fences and user submissions are not
  attempted. This write stage has not been run on the phone; `ready()` remains false.
- `adreno=20` repeats the guarded GMU/GX/core/ZAP/CP bootstrap, verifies CP is idle at RPTR/WPTR
  12, appends the two-dword A630 `CP_SET_SECURE_MODE(0)` packet, publishes WPTR 14, and waits up
  to two seconds for RPTR 14, CP/AHB idle, and clear CP error/fault/protection state. Any mismatch
  faults the backend and leaves ordinary command submission disabled. This matches Qualcomm's
  `adreno_set_unsecured_mode()` call after `a6xx_send_cp_init()` in [`adreno.c`](https://android.googlesource.com/kernel/msm/%2B/604f9dc59bcb86b3967048ebe36c48aff59b2592/drivers/gpu/msm/adreno.c) and the `CP_SET_SECURE_MODE` packet helper in [`adreno_ringbuffer.c`](https://android.googlesource.com/kernel/msm/%2B/3a9ea3d5335330e66dc313253c055442c9db7b8e/drivers/gpu/msm/adreno_ringbuffer.c).
  It still does not enable fences, user submissions or rendering. This write stage has not been run
  on the phone; `ready()` remains false.
- `adreno=21` repeats the guarded setup through unsecured CP mode, then submits one kernel-owned
  `CP_MEM_WRITE` to IOVA `RPTR + 64`, followed by `CP_WAIT_MEM_WRITES`. The destination is a
  dedicated writable cache line in the existing SID0 scratch page, separate from the CP RPTR word;
  the command ring remains GPU read-only. It waits for RPTR/WPTR 19, CP/AHB idle and clear CP error
  status, invalidates the CPU cache line and checks the fence magic value. This exercises a bounded
  GPU write and fence readback without opening the ring to clients. It follows the A630 PM4 packet
  definitions and KGSL fence-write ordering in [`adreno_pm4types.h`](https://android.googlesource.com/kernel/msm/%2B/f5996ba85abd9d67bdb9506421177a9625ce29c4/drivers/gpu/msm/adreno_pm4types.h) and
  [`adreno_ringbuffer.c`](https://android.googlesource.com/kernel/msm/%2B/037b70a47d04f86b930d349f38a8dcfd7fd07944/drivers/gpu/msm/adreno_ringbuffer.c).
  After checking the magic value, it clears that word so it cannot satisfy the first production
  fence. `ready()` remains false and user submissions are disabled. This write stage has not been
  run on the phone.
- Once SQE halt has been released, `sdm845_gpu_stop()` now leaves the GMU OOB vote, PAS image,
  SMMU mappings and CP buffers intact. A630's graceful idle/slumber, ring stop and reset sequence
  is not implemented yet, so stop deliberately refuses to tear resources down underneath a live
  CP. This is a safety guard, not a completed power-down path; runtime start/stop and recovery
  remain unavailable.
- There is an earlier lifecycle gap: the shared `hakios_adreno_stop()` only calls `HakiosGPUStop()`
  when `hakios_gpu_ready()` is true, while A630 deliberately keeps `ready()` false and its backend
  has no `quiesce` callback. Therefore the A630 stop guard is not reached through the normal stop
  entry, and `loader_return()` does not quiesce this backend before returning memory to the loader.
  After a write stage has started GMU/HFI or attached SMMU streams, this can leave live hardware
  referencing kernel-owned queues or page tables. Do not run those stages on-device or treat a
  return to the loader as safe until lifecycle dispatch and a verified A630 quiesce path are in
  place. Do not fix this by merely removing the readiness check: A630 needs a stage-aware shutdown
  that proves the GMU and CP are idle before releasing votes, routes, or backing memory.

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
live tree has eight GPU entries (710, 675, 596, 520, 414, 342, 257, and 0 MHz), with the idle
entry last and per-entry bus frequency/minimum/maximum indices, plus three GMU entries (0, 200,
and 500 MHz). Preflight reads these values from DT; the step 13 ARC builder accepts only
frequencies with a source-verified SDM845 v2 corner mapping. The GPU client `grp3d` exposes 13
one-path `qcom,msm-bus,vectors-KBps` usecases and
the GMU client `cnoc` exposes 2. Preflight checks stable source/destination IDs, zero bandwidth in
the first case, monotonically nondecreasing AB/IB values, and that each GPU powerlevel's
`qcom,bus-freq`, `qcom,bus-min`, and `qcom,bus-max` indices fit the 13-case table. The read-only
step logs each case in KB/s. Qualcomm's KGSL asks the bus framework for a TCS command set for every
case and copies its command addresses, wait mask, and data into `H2F_MSG_BW_VOTE_TBL`; bus vectors
alone are not interchangeable with those commands
([`kgsl_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/9b7afd65fddcb21b51a2db8f4a82b08942225263/drivers/gpu/msm/kgsl_gmu.c),
[`adreno-gmu.txt`](https://android.googlesource.com/kernel/msm/%2B/refs/heads/android-msm-coral-4.14-android10-qpr1/Documentation/devicetree/bindings/gpu/adreno-gmu.txt)).
The runtime Recovery graph resolves the GPU DDR path through the LLCC and EBI nodes, with BCMs
SH0, MC0, and ACV; the GMU `cnoc` vector resolves to SH2, SN15, and SN3. The HakiOS builder checks
the cell IDs, phandle edges, BCM attachment order, and Apps RSC links before using those paths.
It reads each BCM's 8-byte CMD_DB auxiliary record (`unit_size`, `width`, and clock domain) and
resource address. It then applies the downstream MSM-bus aggregation and `msm_bus_div64()`
semantics, encodes the 14-bit votes plus valid/commit bits, and orders commands by clock domain.
ACV follows Qualcomm's special Apps-RSC vote bit. A630 HFI allows 8 DDR commands, 6 CNOC
commands, 16 DDR levels, and 2 CNOC levels; the resulting wire message is 160 dwords
([`kgsl_gmu_core.h`](https://android.googlesource.com/kernel/msm/%2B/2f6b519315371029d73e92ac468cc846daef5e8f/drivers/gpu/msm/kgsl_gmu_core.h),
[`msm_bus_arb_rpmh.c`](https://android.googlesource.com/kernel/msm/%2B/refs/heads/android-msm-coral-4.14-android10-qpr1/drivers/soc/qcom/msm_bus/msm_bus_arb_rpmh.c),
[`msm_bus_fabric_rpmh.c`](https://android.googlesource.com/kernel/msm/%2B/refs/heads/android-msm-coral-4.14-android10-qpr1/drivers/soc/qcom/msm_bus/msm_bus_fabric_rpmh.c)).

The Qualcomm A6xx KGSL flow uses `gfx.lvl`, `cx.lvl`, and `mx.lvl` ARC tables to build the GPU and
GMU voltage votes, then sends those rows through the GMU DCVS/HFI path. HakiOS now builds that
fixed-size performance table in `adreno=13`; the GPU corner mapping follows SDM845 GPUCC's v2
frequency/corner table, and GMU 200/500 MHz use the SDM845 CX minimum/low corners. This indicates
the GPU-specific ARC vote owner is the GMU; HakiOS should not duplicate those rail votes with
CPU-side active TCS requests. Its host TCS transaction only supplies the temporary DDR bandwidth
usecase. HakiOS now sends the default GX/MX boot OOB request before HFI queue startup,
then sends an initial GMU DCVS OOB request after `PERF_TBL`, `BW_VOTE_TBL`, and the legacy
`HFI_TEST` closeout. It brackets GMU boot and that initial DCVS handoff with the DT-selected DDR
bus usecase through Apps RSC TCS0. Qualcomm's A630v2 target identifies GMU 1.3 and enables RPMh,
while KGSL labels the A630/A615 firmware path as legacy HFI. Step 13 sends `PERF_TBL` only; step 14
sends `PERF_TBL`, `BW_VOTE_TBL`, then the legacy `HFI_TEST` closeout and initial DCVS register
request. Polaris DT sets
`qcom,gpu-quirk-hfi-use-reg`, the upstream condition for sending that command. The A6xx
`a6xx_gmu_hfi_start_msg()` returns without sending `H2F_MSG_START` unless the target is A640, so
this command is not an A630 bring-up requirement. Qualcomm's `gmu_start()` enables the GMU
domains/clocks, establishes a startup bus vote, runs `GMU_FW_START`, starts HFI, then requests the
default GPU power level; HakiOS now implements the temporary startup DDR vote and its release,
but not the complete CX/GX clock sequence. The host vote is sent only when Apps RSC DRV2 reports
v2.7, TCS0 is idle with no commands enabled, and the command capacity fits. It uses the shared
`hakios_rsc_lock`, writes each BCM address/data pair, waits on the commands whose BCM data carries
the commit bit, checks command completion status, then disables the TCS. A timeout that leaves
TCS0 busy does not trigger cleanup writes to that live sequencer. The TCS write path supports v2.7
only; v3.0 remains read-only until its exact write layout is verified. The `HFI_TEST` ACK completes
the legacy message phase; the DCVS ACK only confirms that GMU firmware accepted the initial request,
not that HakiOS has verified sustained rail, clock, bus, or GPU operation. The startup vote order
follows Qualcomm's 4.9 KGSL [`gmu_start()`](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/drivers/gpu/msm/kgsl_gmu.c).
VCD commit flags follow its [MSM bus TCS generator](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/drivers/soc/qcom/msm_bus/msm_bus_fabric_rpmh.c).
See the primary
[`gpucc-sdm845.c`](https://android.googlesource.com/kernel/msm/%2B/048cc82284cb0ee2c7381ec5d1b08a2f7d7aa0b4/drivers/clk/qcom/gpucc-sdm845.c),
[`SDM845 RPMh level bindings`](https://android.googlesource.com/kernel/common/%2B/3135f5b73592988af0eb1b11ccbb72a8667be201/include/dt-bindings/power/qcom-rpmpd.h),
[`kgsl_gmu.c`](https://android.googlesource.com/kernel/msm/+/9b7afd65fddcb21b51a2db8f4a82b08942225263/drivers/gpu/msm/kgsl_gmu.c),
[`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/79a5a3af469e5d38c649dbe3dc7340d96990fd68/drivers/gpu/msm/adreno_a6xx_gmu.c),
[`kgsl_hfi.h`](https://android.googlesource.com/kernel/msm/%2B/ae8d33fa220cfd210a0983ed27d09662753c5b75/drivers/gpu/msm/kgsl_hfi.h),
[`adreno-gpulist.h`](https://android.googlesource.com/kernel/msm.git/+/7bbb67403d95f83d95e0ea464f972f247475d0d4/drivers/gpu/msm/adreno-gpulist.h),
[`kgsl_hfi.c`](https://android.googlesource.com/kernel/msm/+/541919e72ee1156c0054952dd11d5066372095a8/drivers/gpu/msm/kgsl_hfi.c),
[`sdm845-v2.dtsi`](https://android.googlesource.com/kernel/msm/+/5f65a2ac2505d5e0c122d5e54a58194c873b426c/arch/arm64/boot/dts/qcom/sdm845-v2.dtsi),
[`A630 4.9 DCVS`](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/drivers/gpu/msm/adreno_a6xx.c),
[`A630 4.9 HFI bitfields`](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/drivers/gpu/msm/kgsl_hfi.h),
[`A630 4.9 OOB masks`](https://android.googlesource.com/kernel/msm/+/refs/heads/android-msm-baiji-4.9-pie-wear-dr/drivers/gpu/msm/kgsl_gmu.h)).
For the runtime Polaris frequencies, the builder maps GPU 257/342/414/520/596/675/710 MHz to
VDD_GX corner values 64/128/192/256/320/384/416; the source's 180 MHz floor maps to 48 but is not
present in this DT. GMU 200/500 MHz map to VDD_CX corner values 48/128. A zero-frequency row uses
corner requirement zero, then KGSL's ARC lookup selects the first available primary level.
Step 8 remains read-only: it validates ARC and bus-vector inputs and reports active TCS status,
but does not issue a host TCS request. Steps 12–14 reconstruct the DDR commands from the validated
bus table; startup temporarily votes the DT-selected row through Apps RSC, and step 14 releases it
after GMU accepts initial DCVS. This path is code-only and has not been run on the phone. Sustained
DVFS is not operational because verified CX/GX sequencing, GMU/GPU clocks, GPU-user/3D mappings,
runtime frequency transitions, and complete stop-time vote handling are still missing.
`H2F_MSG_START` is A640-specific in the cited KGSL flow and is not part of the A630 gate.

The A630 legacy HFI v1 wire format is now isolated in `drivers/gpu/hakios.adreno.hfi.h`. It
provides bounded shared-table initialization, command queue writes, response queue reads,
sequence-number generation, the GMU init command, and v1 ACK matching. The queue header is 12
dwords, the queue table is 54 dwords, and each queue contains 1024 dwords. A630's legacy queue IDs
are 0/4/5/1 for command/message/debug/dispatch; the dispatch queue remains disabled. The HFI
table/queue allocation is page-rounded to 20 KiB and its GMU virtual address belongs to the
noncached kernel range beginning at `0x60000000`. `adreno=3` checks and reports this protocol
layout. Step 9 prepares detached HFI/dump/log mappings and the GPU/GMU roots; step 11 attaches
separate GMU-kernel and GMU-user contexts plus 3D SID0/CB0 when that fixed context is available.
Step 12 starts the GMU, applies the default GX/MX boot
vote, sends `GMU_INIT`, and verifies the A630 GMU firmware version through `FW_VERSION`. Step 13 builds and sends the 43-dword HFI v1
`PERF_TBL`; step 14 sends that table with the 160-dword `BW_VOTE_TBL`, then sends `HFI_TEST` and
requests the initial register-based DCVS level. Runtime frequency changes, ZAP authentication,
command processor startup, and rendering remain pending. The cited A630 HFI flow does not
use the A640-only `H2F_MSG_START`.
Steps 12–14 are code-only bring-up stages and have not
been invoked on the phone.
The queue-table header still reports four active slots with dispatch disabled in its queue header,
matching Qualcomm's A630 `hfi_init` (`num_active_q = HFI_QUEUE_MAX`); HakiOS preserves that v1
contract ([`kgsl_hfi.c`](https://android.googlesource.com/kernel/msm/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_hfi.c)).

Qualcomm's matching A630 KGSL implementation gives the GMU separate `gmu_user` and `gmu_kernel`
SMMU contexts and allocates HFI memory through the GMU-kernel context. HakiOS now prepares separate
detached roots and route candidates for SIDs 5 and 4; explicit steps 11 and 12 can install the
GMU-kernel and GMU-user contexts, with the HFI queues mapped only into GMU-kernel. Those write
stages have not run under HakiOS, so live ownership and hypervisor permission are still unverified;
the runtime-DT checks and read-only route scan only make candidates. The GPU user page table cannot
be assumed to serve the GMU stream. See the primary Qualcomm [`kgsl_hfi.h`](https://android.googlesource.com/kernel/msm.git/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_hfi.h) and
[`kgsl_gmu.c`](https://android.googlesource.com/kernel/msm.git/+/965345c9413fe4ce2e92c8b55f1892c0395eb91d/drivers/gpu/msm/kgsl_gmu.c) sources.

The local firmware set is `a630_gmu.bin`, `a630_sqe.fw`, and `a630_zap.{mdt,b00,b01,b02,elf}`.
`devices/polaris/pull-firmware.sh` has now pulled all seven from the MIX 2S in Recovery: `/vendor`
was mounted read-only from `/dev/block/sde47`, with fingerprint
`Xiaomi/polaris/polaris:8.0.0/OPR1.170623.032/V9.5.19.0.ODGMIFA:user/release-keys` and
incremental build `1793965292`. Device-side and local SHA-256 hashes match for every file, and the
script refreshed `fw-gpu/SHA256SUMS`. `userland/build.sh` copies the ignored local `fw-gpu/*` set
into the image's `fw/` directory when building.
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
table. Its software map, commit, unmap and cache-maintenance callbacks are implemented; GMU
firmware/HFI startup and initial ARC/DCVS requests are staged, while complete CX/GX sequencing,
active GMU-user/3D SMMU context attachment, recoverable stop and thermal hooks are still missing.
The A630 backend now has dormant `submit`, `done`, `last`, and `report` callbacks: submit builds
the A6xx CCU-invalidate / `CP_INDIRECT_BUFFER_PFE` / `CACHE_FLUSH_TS` sequence, validates each IB
against the client IOVA aperture, handles ring wrap and bounded space polling, and keeps the
completion word in the private RPTR allocation. These callbacks remain unreachable from the public
service because the current `ready()` gate requires an active SID0 context, ready/sealed GMU/HFI,
GPU OOB/core setup, authenticated PAS/ZAP, initialized CP in unsecured mode, and the completed
step 21 fence smoke test. The separate backend-enabled flag still has no writer, so `ready()` stays
false. No callback falls through into Redwood's SMMUv500 or register state.

GPU VA allocation and release now dispatch through the selected backend too. Polaris has a
software-only, locked 32-bit IOVA allocator. Backend SQE/ring/RPTR buffers use the private range
`[0x00010000, 0x40000000)`; the shared service receives client allocations only from
`[0x40000000, 0x100000000)`. Both allocators round to 64-KiB extents and accept exact frees. This
address partition does not isolate command streams: the 3D SID0 context still uses one page table
for backend and client mappings. The new 4K LPAE stage-1 tables can describe both ranges, but no
context bank or stream is programmed until a guarded bring-up step, and rendering remains disabled.

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
The shared `HakiosGPUUserClient::submit()` rejects zero-byte command buffers before forming an IB
entry, in addition to its existing alignment, relocation and bounds checks. The A630 backend accepts
at most 128 IBs and only client IOVAs in `[0x40000000, 0x100000000)`; this does not validate IB
contents or isolate command streams on the shared SID0 page table. The public `HakiosGPU` service
must only start after its selected backend has completed CP bring-up, command isolation review, and
bounded hardware submission checks.

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
   The A630 GMU/HFI bootstrap, temporary startup DDR TCS vote, `PERF_TBL`, `BW_VOTE_TBL`, initial
   boot-rail OOB, and default DCVS stages are implemented behind explicit `adreno=12`/`adreno=13`/
   `adreno=14`, but none has been run on hardware. Steps 17–21 now add the A630 GMU GPU OOB
   request, GX/GFX3D verification, core HWCG/VBIF/UCHE defaults, CP register protection, bounded
   SQE/`CP_ME_INIT` startup, PAS ZAP authentication before SQE release, the
   `CP_SET_SECURE_MODE(0)` transition, and a kernel-owned CP memory-write fence smoke test. None
   of these write stages has been run. Verify the Apps RSC transaction, sustained bandwidth/DCVS
   behavior, GX sequencing, core readbacks, CP idle/error status, fence visibility and PAS cleanup
   against Polaris before proceeding. Keep step 7 limited to PAS staging and shutdown. The dormant
   production ring/fence callbacks are now implemented from the upstream A6xx submit ordering
   ([`a6xx_gpu.c`](https://android.googlesource.com/kernel/common/%2B/a0497251f2b055a137d62ed065286ba999647b3c/drivers/gpu/drm/msm/adreno/a6xx_gpu.c)); they have not been exercised on hardware. Next validate the packets,
   fence visibility, register protection and ring mappings on Polaris, then implement and validate
   recoverable power-down before accepting client submissions.
   **Gate:** GMU firmware reaches its documented ready state; SMMU and secure firmware report no
   faults; stop returns hardware to the loader's prior state.
5. **SMMUv2 and command processor.** The software 4K page-table mapper and its table-cache
   maintenance are implemented, with OAS and PTFS_4K checks taken from the read-only ID registers.
   Step 15 now constructs the A630 `CP_ME_INIT` and secure-mode packets offline. The local firmware
   probe validates the ZAP MDT layout and applies the upstream SQE check to the payload after its
   4-byte file prefix; the extracted SQE passes the version floor. The read-only
   SMMUv2 probe scans stream routes and context registers and produces conditional CB/SMR
   candidates; step 9 builds detached GPU, GMU-kernel, and GMU-user roots and step 11 can install
   GMU routes and a conditional 3D SID0/CB0 route with readback and rollback. Neither step 11 nor
   any SMMU write stage has
   run on the phone. Do not invoke step 11 until HYP ownership and write access are verified from
   a controlled HakiOS boot. Then verify the GMU contexts and conditional 3D context attachments,
   verify steps 19–21's bounded SQE/CP bootstrap, ZAP authentication, secure-mode transition and
   kernel fence write, then validate production register protection, ring/fence mappings and mapper
   behavior. Qualcomm's
   A630 KGSL path sets the SQE base and authenticates ZAP through PIL before releasing SQE halt,
   submits `CP_ME_INIT`, then uses `CP_SET_SECURE_MODE(0)` to leave secure mode
   ([`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c),
   [`adreno.c`](https://android.googlesource.com/kernel/msm/%2B/604f9dc59bcb86b3967048ebe36c48aff59b2592/drivers/gpu/msm/adreno.c),
   [`adreno_ringbuffer.c`](https://android.googlesource.com/kernel/msm/%2B/3a9ea3d5335330e66dc313253c055442c9db7b8e/drivers/gpu/msm/adreno_ringbuffer.c)). Keep the backend disabled
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
- The `adreno=6` PAS capability result and `adreno=7` PAS staging result on this boot environment.
  ZAP authentication is not implemented until SQE/CP startup and the secure-mode transition are
  available. Qualcomm's MDT loader consumes the `.mdt` ELF header, the indexed `.b01` hash
  metadata, and `.b02` as the sole `PT_LOAD` segment; `.b00` and the combined `.elf` are not part
  of that loader path.
- The extracted A630 SQE passes upstream's version floor after the 4-byte file prefix is removed;
  its register-protection table and the MSM submission path still need validation against the
  630's GMEM/UBWC configuration.
- Whether TrustZone PAS calls for GPU firmware are permitted from this boot environment.
- Whether A630 GMU HFI initializes and accepts the performance/bandwidth tables, boot-rail OOB,
  and initial DCVS OOB under HakiOS and the Polaris hypervisor; whether Apps RSC v2.7 accepts the
  generated startup/release command rows and wait masks; and whether the runtime
  `gfx.lvl`/`cx.lvl`/`mx.lvl` tables pair correctly with the live GPU/GMU operating points. The
  `adreno=12`/`adreno=13`/`adreno=14` write stages have not been invoked on the phone; the direct
  Apps RSC active-TCS path remains unverified on hardware.
- Cache coherency, interrupt availability, hang recovery, DVFS/thermal policy, and full
  client-memory isolation on the actual phone.
- Do not assume CP APRIV can separate kernel and client mappings on this device: Qualcomm's KGSL
  `a630v2` catalog entry does not advertise `ADRENO_APRIV`, unlike the deprecated A630v1 profile
  ([`adreno-gpulist.h`](https://android.googlesource.com/kernel/msm/%2B/090c2c5774db4807fadaa2e283922264ae9267d1/drivers/gpu/msm/adreno-gpulist.h)). The current IOVA ranges are disjoint but share the same SID0 page table; a safe command-stream/private-memory isolation design remains a readiness blocker.
- A630 shutdown after SQE startup: the current stop hook conservatively retains hardware votes,
  PAS and buffers. Implement and validate the GMU idle/slumber and CP stop/reset sequence before
  enabling client submissions or claiming recoverable power-down. Upstream A6xx shutdown checks
  GMU idle, clears pending bus transactions, requests slumber, then stops HFI/interrupts and removes
  RPMh, power-domain, and clock votes; a separate force-off path handles a hung GMU. See Qualcomm's
  [`a6xx_gmu.c`](https://android.googlesource.com/kernel/common/%2B/0d534518ce87317e884dbd1485111b0f1606a194/drivers/gpu/drm/msm/adreno/a6xx_gmu.c).
  This is a sequence to audit against Polaris' A630 legacy HFI, RSC and SMMUv2 setup, not code to
  copy verbatim.

These are gates to resolve with source evidence and controlled device runs, not values to infer
from Redwood or fill with guessed constants. The Dipper repository is only a comparison reference:
its Adreno source is substantially the same Redwood backend and does not supply an A630/SDM845
implementation.
