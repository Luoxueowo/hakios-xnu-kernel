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
- `CONFIG_HAKIOS_ADRENO=y` now includes the Polaris backend in the MIX 2S kernel build. The driver
  does not start the GPU by default; the explicit `gpu=1` boot argument runs guarded stages 21–23,
  then starts HakiosGPU only if the client-root IB and kernel-root fence probe passes. If IOKit
  service creation fails, it closes the client gate and requests the existing bounded orderly stop.
  A failed `gpu=1` bring-up also calls that stop path; it can quiesce a verified healthy boundary
  and deliberately retains resources if startup is faulted or safety cannot be confirmed. This
  runtime cleanup path has not yet been validated on the phone.
- On 2026-11-12, `make DEVICE=polaris JOBS=8 kernel` selected 10 Polaris drivers and compiled
  `hakios.adreno.sdm845.o` (including the guarded A630 hang snapshot), `hakios.adreno.o`,
  `MiniIOSurface.cpo`, and the shared IOKit GPU service, including the new resource caps. The build
  emitted `hakios-kernel`, then exited 2 on the known XNU kext symbol-set import-list failures;
  this is compile evidence only, not a device boot or GPU runtime result.
- The Polaris loader builds the XNU-facing `/arm-io/gic` node from its SDM845 platform profile:
  the GICv3 distributor/redistributor addresses and virtual-timer PPI match the device profile,
  while the `arm-io` `ranges` property supplies the nonzero SoC base expected by XNU. It now also
  embeds the checked-in Android DTB at build time and imports its `/soc` and `/reserved-memory`
  subtrees into the Apple-format tree, preserving the root `interrupt-parent`. Names longer than
  the Apple tree's 32-byte fields are omitted. The loader reserves 2 MiB for the resulting tree
  and aborts before kernel handoff if it does not fit. This exposes the runtime DT paths to XNU;
  confirming that the drivers match them still needs runtime validation. A read-only
  Recovery inspection on 2026-11-12 found the KGSL interrupt tuple `<0 0x12c 0>`. The KGSL node
  inherits root interrupt-parent phandle `1` (`qcom,pdc-sdm845-v2`), whose parent is the GICv3
  node at `0x17a00000` (phandle `0xa2`). The existing generic XNU GICv3 path has an SPI attach
  API, but the GPU backend does not attach its interrupt yet: completion waits still poll every
  200 microseconds. The Recovery parent chain and loader profile establish the interrupt
  description only; PDC-to-GIC routing, delivery, source acknowledgement, and fence behavior
  remain to be validated on the running HakiOS kernel.
- On 2026-11-12, a temporary `fastboot boot` diagnostic image (not flashed) reached the XNU
  handoff with `gpu=1 bootcpu=7 cpus=8 loader_return_after=120`; `ufs_write=1` was absent. The
  loader surveyed UFS, imported `/soc` and `/reserved-memory`, received PSCI status 0 (request
  accepted) for MPIDR `0x700`, and jumped to the kernel entry. The captured live-log/message-buffer
  sections were empty: there was no XNU panic/exception text, timer-return message, or Adreno
  stage output. A matching `gpu=0`/`bootcpu=7` boot also returned to Recovery before the timer,
  so the available logs do not implicate A630 initialization. `gpu=0 bootcpu=0 cpus=8` remained
  off USB longer, then returned to Recovery without an XNU log. `gpu=0 bootcpu=0 cpus=1` remained
  unenumerated by USB for more than 180 seconds, then reappeared in Recovery. Its final logdump
  still ends at the kernel jump, with no XNU text, panic/exception, or timer-return message.
  Recovery exposed neither pstore data nor `/proc/last_kmsg`, and its boot-reason properties were
  `unknown`.
- The project's `xl enter` route could not establish its USB loader session: its loader log says
  `usb: device protocol not found`, and no kernel payload was uploaded. The firmware did not
  expose the USB device protocol this mode requires. The direct-boot attempts confirm kernel
  handoff but do not show whether XNU's normal logging path starts. Keep the A630 stages
  unvalidated until XNU logging works on Polaris.
- The shared service now separates its one-shot diagnostic latch from the unresponsive-GPU latch.
  A completion wait of at least one second that expires blocks later GEM submissions before cache
  maintenance or CP work, but leaves backend readiness unchanged so possibly in-flight DMA memory
  remains retained. A ring-full report alone no longer short-circuits later fence waits. The A630
  report also samples the selected SID0 SMMUv2 context FSR/FAR/FSYNR0 and global SMMU fault state
  when its existing mappings and bounds permit. Before reading GPU core registers it now checks
  the live GX power and GFX3D clock state, then records RBBM STATUS3 and active CP IB1/IB2 base and
  remaining-size registers. Their offsets match Qualcomm's A6xx
  [`a6xx_reg.h`](https://android.googlesource.com/kernel/msm/%2B/d5d2256a441456eb3548ee03b95323f111732bb5/drivers/gpu/msm/a6xx_reg.h).
  This read-only snapshot improves hang diagnosis; it does not recover the GPU.
  The offsets and Linux fault-read path follow upstream [`arm-smmu.h`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.h)
  and [`arm-smmu.c`](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c).
  These changes have compile/source evidence only and still need a controlled Polaris run.
- The in-tree `metal2vulkan` route has a matching Mesa/Turnip device profile for Polaris: Mesa 25.0.7 maps GPU ID 630 to
  A6xx gen1, and the Polaris kernel profile reports GPU ID 630, chip ID `0x06030001`, GMEM base `0x00100000`, and 1 MiB GMEM.
  Mesa's A630 device data specifies two CCUs, 32 VSC pipes, 32 KiB shared compute memory, and 2048 fibers per SP. This makes
  metal2vulkan/Turnip one Metal path to validate on the MIX 2S. It is a profile match only: no Vulkan workload, Metal
  compositor, or command stream has been validated on Polaris. `hakios-metal` now also has a separate build-time A630 profile
  based on the same Mesa device entry; it has source/build evidence only and still needs controlled phone validation.
- The Turnip DRM shim's timeline syncobjs now grow their pending-fence list as needed. When a
  fence queue reaches its polling interval, it only advances a point after a nonblocking kernel
  wait confirms completion; it no longer discards the oldest point after an unchecked 10-second
  wait. Submit syncobj arrays are copied and bounds-checked, and output handles/capacity slots are
  reserved before submission so concurrent syncobj operations cannot consume them before the
  returned fence is recorded. `drmSyncobjTransfer` now returns `EINVAL` for an unmaterialized
  source point unless `WAIT_FOR_SUBMIT` was requested; that flag waits up to five seconds for the
  point to be submitted, then returns `ETIME` on timeout. It pins both handles while waiting and
  preserves a binary destination if it cannot reserve the replacement fence. This follows the
  upstream [Linux DRM syncobj implementation](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/drm_syncobj.c).
  The shim compiled to an arm64 iOS object on 2026-11-12; no Vulkan workload has run on the phone.
- On 2026-11-12, Recovery ADB confirmed `/vendor` is the phone's `ext4 ro` mount from
  `/dev/block/sde47`. `devices/polaris/pull-firmware.sh` pulled the seven A630 files from
  `/vendor/firmware` and compared each pulled file's SHA-256 against the device-side hash. The
  local blobs and `fw-gpu/SHA256SUMS` remain ignored by Git. This confirms firmware provenance and
  integrity, not that HakiOS can load or run it.
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
- A second read-only Recovery inspection on 2026-11-12 identified the device as
  `fox_polaris`/`polaris` on `sdm845` with Qualcomm hardware; the running Recovery kernel was
  `4.9.337-OrangeFox-polaris-LOS21+`, and `/vendor` was mounted read-only from `/dev/block/sde47`.
  All seven firmware files again matched the local ignored copies byte-for-byte. Their SHA-256
  values were: `a630_gmu.bin` `b07f90299ff2ae33948fcfd6ed67f53fbd287b5bf85ff2d43db11d4d32c1621f`,
  `a630_sqe.fw` `36298bd53ea3666413d288a3a3f5266f67fcb6db038a0a481af5869d1c17859c`,
  `a630_zap.mdt` `e1e11d7f124b53efe73c2b03fab613fc1075d4caa5f41f6a19dc2ac56ea736dd`,
  `a630_zap.b00` `b2975f6a4c28a98197c1d694f6e275e71b23ec7e31e32ff5d1f83fdb80a94282`,
  `a630_zap.b01` `a6d10b93e1d83b53d4cf501c78945ab324bf2a1622bcfd08cafe3d7a6f49aacb`,
  `a630_zap.b02` `519b5538809f54ee566246b0520746a8ea82ca9384ea6bddb0b0e0d3c2de520c`, and
  `a630_zap.elf` `fe7a870b3adc6f0d8e58d682ba54c4689188c679ad1433a68e6616896e57882f`. No phone
  storage was written.
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
  a post-release failure, cleanup reasserts and verifies reset, attempts to detach GMU SID5/SID4 and
  any attached SID0, and frees CP buffers only after all routes detach and GX remains off. The
  backend stays faulted; unresolved routes and HFI memory stay allocated.
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
  for A640. After successful startup, a `PERF_TBL` construction or acknowledgement failure runs
  the verified-reset cleanup described below. This write stage has not been run on the phone.
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
  per-command status. ZAP authentication, command processor startup, recovery, thermal handling,
  and rendering remain unavailable. Loader-return quiesce is implemented but not hardware-validated.
  Rejection of either table,
  `HFI_TEST`, or initial DCVS invokes failure cleanup. Cleanup verifies CM3 reset before releasing
  the temporary Apps RSC DDR vote; after that readback, it attempts to disconnect the GMU SID4/SID5
  and any attached SID0 routes, invalidate the ASIDs, blank the context banks, and restore the
  original S2CR values. It clears `firmware_started` only after verified reset so the detach checks
  can succeed. It leaves the backend faulted if any readback or synchronization fails. If reset
  cannot be confirmed, it retains HFI/OOB/SMMU state and votes. The GX/MX boot-rail vote stays
  marked active because this failure path does not perform the normal slumber/power-down sequence.
  This write stage has not been run on the phone.
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
  CP scratch/fence buffer into the detached GPU SID0 page table. The SQE and ring use ordinary
  unprivileged, read-only SMMU mappings; the writable scratch is mapped only in the kernel GPU
  root. This matches the Polaris A630v2 profile, which does not advertise APRIV. It cleans the CPU
  cache lines and commits the software page tables,
  but does not attach SID0, write SMMU/GPU registers, start the SQE/CP, call PAS, or enable rendering.
  Buffers remain allocated for review until the backend stop path confirms that the 3D context
  is detached and the core is inactive. The 32 KiB ring size follows the A630 KGSL definitions
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
  and the A630 CP register-protection table. A source recheck against Polaris's own
  [`a630_hwcg_regs` and A6xx HWCG helpers](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno_a6xx.c)
  corrected seven values: five RAC/TSE-RAS/GPC registers and two GMU AO CGC settings. It also converts Polaris's validated
  `qcom,protect = <0x40000 0xc000>` aperture using KGSL's IOMMU encoding and adds its SMMU range
  to the 27 fixed A630 protection groups, for 28 of the 32 available descriptors. Selected core,
  VBIF, UCHE, CP and protection registers are read back; a mismatch faults the backend and leaves
  GPU OOB asserted for diagnosis. The register sequence follows Qualcomm's A630
  [`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c)
  and [`kgsl_iommu.c`](https://android.googlesource.com/kernel/msm/%2B/f39cb888297d4560766734fe0c42177714f8f9d5/drivers/gpu/msm/kgsl_iommu.c).
  It stops before SQE/ring register programming, `CP_ME_INIT`, ZAP authentication or submission.
  This write stage has not been run on the phone; `ready()` remains false.
- `adreno=19` repeats the guarded GMU/GX startup and step 18 core setup, programs the SQE,
  read-only 32-KiB CP ring and writable CP scratch page, sets `CP_RB_CNTL_NO_UPDATE` so CP does
  not write the RPTR shadow to memory, then restages the validated ZAP image and calls
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
  `CP_MEM_WRITE` to IOVA `CP scratch + 64`, followed by `CP_WAIT_MEM_WRITES`. The destination is a
  dedicated writable cache line in the existing SID0 scratch page. The command ring remains GPU
  read-only. It waits for the MMIO CP RPTR/WPTR registers to reach 19, CP/AHB idle and clear CP error
  status, invalidates the CPU cache line and checks the fence magic value. This exercises a bounded
  GPU write and fence readback without opening the ring to clients. It follows the A630 PM4 packet
  definitions and KGSL fence-write ordering in [`adreno_pm4types.h`](https://android.googlesource.com/kernel/msm/%2B/f5996ba85abd9d67bdb9506421177a9625ce29c4/drivers/gpu/msm/adreno_pm4types.h) and
  [`adreno_ringbuffer.c`](https://android.googlesource.com/kernel/msm/%2B/037b70a47d04f86b930d349f38a8dcfd7fd07944/drivers/gpu/msm/adreno_ringbuffer.c).
  After checking the magic value, it clears that word so it cannot satisfy the first production
  fence. `ready()` also requires a separate public-client enable latch, which this bring-up path
  never raises. User submissions therefore remain disabled even after the kernel-owned smoke test.
  This write stage has not been run on the phone.
- `adreno=22` repeats the verified kernel fence setup, builds a temporary client root with only the
  ordinary read-only SQE/ring mappings, then tests a CPU-driven SID0 root switch to that ASID and
  back. It waits for the CP ring and A6xx RBBM clients to be idle, faults SID0 in S2CR, disables
  CB0, synchronizes the old/new ASIDs, updates TTBR0/TTBR1, re-enables CB0, and restores the
  translation route. It reclaims the temporary tables only after SID0 is verified back on the
  kernel root. This exercises context-register switching only: it submits no client IB, does not
  prove that the root translates actual GPU accesses, and does not open the client gate. The idle
  predicate follows Linux's A6xx ring-drain, RBBM-idle and hang-detection checks
  ([`a6xx_gpu.c`](https://android.googlesource.com/kernel/common/%2B/63407d3081a6b406d94bd7d96ff2ca38fa7df236/drivers/gpu/drm/msm/adreno/a6xx_gpu.c));
  context-bank and ASID operations follow the ARM SMMUv2 architecture specification
  ([IHI 0062](https://documentation-service.arm.com/static/5f900d34f86e16515cdc08fb)). The stage has
  not been run on the phone.
- `adreno=23` repeats steps 21–22, maps a temporary read-only client IB plus a client-writable
  output page, and dispatches one `CP_MEM_WRITE` IB while SID0 uses that client root. The shared
  read-only ring is scrubbed before the switch, and its client-root packet contains no kernel TTBR
  or completion address. After CP and the A6xx RBBM clients are idle, it switches SID0 back to the
  kernel root, scrubs the client ring image, queues a trusted kernel-root fence, and checks both
  the output word and fence. Probe tables and buffers are reclaimed only after the kernel-root idle
  boundary is verified; uncertain state is retained. The stage does not enable the public userclient
  gate, and has not been run on the phone.
- `gpu=1` runs the guarded setup through step 23 and starts the shared HakiosGPU service only after
  the client-root output and kernel-root fence both pass. It is an explicit opt-in path and has not
  been run on the phone. The diagnostic `adreno=N` paths leave the public-client gate closed.
- Step 21 only submits one
  kernel-owned fence; step 22 changes the selected SMMU root while the GPU is idle; step 23 tests an
  IB and writable mapping in a temporary client root. The production submit callback now serializes
  client work across CPU-driven root switches and waits for a full idle boundary before returning to
  the kernel root for its fence. `ready()` requires a separate client-enable latch that bring-up
  opens only after the explicit `gpu=1` path passes step 23. Orderly stop now checks whether client
  spaces are still open: it rejects new client spaces while allowing existing clients to finish,
  then closes the gate and attempts shutdown after the last client has been destroyed. Orderly stop
  is still not validated for client workloads. Loader-return quiesce uses a bounded reset/route-fault
  path. These lifecycle paths also need on-phone validation.
- `sdm845_gpu_stop()` first confirms the kernel fence and CP are idle, drains all four Polaris
  VBIF XINs, releases the GPU/boot OOB holds, and requests the A630 legacy-HFI slumber state using
  the DT-selected initial performance and bandwidth indices. It checks GMU slumber, CX busy and
  AHB-fence state, waits for GPUCC to report GX/GFX3D off, then asserts and verifies CM3 reset.
  After PAS shutdown it faults and detaches GMU SID4/SID5 and GPU SID0, synchronizes each ASID,
  clears the context bank and restores its original S2CR before releasing private queue buffers.
  Any failed check retains the possibly referenced memory. This path accepts successful idle
  startup boundaries from step 17 onward, including CP initialization before the fence smoke test;
  failures in a verified SMMU-only state through step 11 can also be detached before GMU startup.
  Later mid-stage cleanup remains conservative. Its register
  sequence is based on Qualcomm's A630 legacy-HFI slumber path and
  no-GBIF bus drain in [`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/b6410c3f75259c0ca2df45a01838d2e46272723e/drivers/gpu/msm/adreno_a6xx_gmu.c) and
  [`a6xx_gpu.c`](https://android.googlesource.com/kernel/common/%2B/63407d3081a6b406d94bd7d96ff2ca38fa7df236/drivers/gpu/drm/msm/adreno/a6xx_gpu.c). This implementation has not been run on hardware.
- The failure-only GMU cleanup now requires CM3 reset readback before touching active SID4/SID5
  routes and only then clears its software `firmware_started` state. It changes each GMU S2CR to
  fault, invalidates a newly allocated SMR, synchronizes the
  ASID, clears and verifies the context bank, then restores the original S2CR. It attempts to detach
  GPU SID0 after the GMU contexts. CP buffers are freed only if every relevant context detached and
  the GPU core remains confirmed off; GMU page tables/HFI backing and the GX/MX boot-rail vote stay
  retained, and the backend remains faulted. A failed reset/detach keeps possibly referenced state.
  The ordering follows
  the SMMUv2 stream-routing rules in Linux [`arm-smmu.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/%2B/77a1c68a96f4e018860033f3827602014a4f1663/drivers/iommu/arm-smmu.c).
- The orderly stop entry dispatches backend `stop()` even while `ready()` is false. If HakiosGPU
  has not created its client lock yet, the stop path dispatches directly because no clients can
  exist. The A630 stop hook accepts successful idle GX/CP startup boundaries from step 17 onward,
  including the initialized secure-mode stages before step 21, and retains all hardware-referenced
  memory if CP idle, VBIF drain, GMU slumber, power-off, reset, PAS shutdown, or SMMU detach cannot
  be confirmed. Panic/loader-return `quiesce` uses bounded MMIO/SMC
  operations; if it cannot prove the DMA routes are isolated, it fail-stops rather than returning
  control while the loader may reuse memory. A pre-GMU-start partial-stop boundary now handles
  SMMU-only startup through step 11: it requires a healthy backend, active CX/GMU clock, CM3 reset,
  and GX/GFX3D off when SID0 is attached, then detaches SID4/SID5/SID0 with ASID synchronization and
  route/context readbacks. If any prerequisite or detach check fails, it retains page tables and
  mappings. Failures after GMU firmware release, ARC/DCVS or boot-rail votes, PAS activity, core
  configuration, or CP startup still use conservative retention unless they reach the verified
  healthy stop boundary. Neither shutdown path has been run on-device.

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
runtime frequency transitions, complete parent-rail vote handling, and device validation remain
missing.
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
firmware/HFI startup, initial ARC/DCVS requests, GX OOB bring-up, A630 core/CP initialization and
the client-root IB path are implemented behind explicit stages. Hardware validation of those
stages, GMU-user/3D SMMU attachment, orderly stop and quiesce is still missing, as are hang
recovery and thermal hooks. The orderly stop path handles the fully initialized kernel-owned fence
stage, but has not been run on a phone.
The A630 backend has `submit`, `done`, `last`, and `report` callbacks. Without APRIV, the client
callback switches SID0 from the CPU only at a full CP/RBBM idle boundary. It scrubs the shared ring,
switches to the client root, submits only client IB addresses, waits for complete GPU idle, then
switches back to the kernel root before queuing the writable completion fence. Client roots map the
SQE/ring pages as unprivileged and read-only; writable completion scratch remains only in the kernel
GPU root. Public calls remain blocked unless the full `gpu=1` path has completed step 23; that path
opens a separate client-enable latch and starts HakiosGPU. The `ready()` gate also requires an active
SID0 context, ready/sealed GMU/HFI, GPU OOB/core setup, authenticated PAS/ZAP, initialized CP in
unsecured mode, and the completed step 21 fence smoke test. Diagnostic `adreno=N` runs leave the
client latch closed. No callback falls through into Redwood's SMMUv500 or register state.

The shared service passes an opaque address-space handle through client create/destroy,
map/commit/unmap and submit. Polaris allocates up to 64 client roots with ASIDs 4–67; client BOs
are mapped only in that client's root, while each root maps SQE and ring pages as unprivileged,
read-only mappings. The writable RPTR/fence scratch is absent from client roots. The kernel-root
fence builder retains the upstream A6xx `CP_SMMU_TABLE_UPDATE` sequence. Client submissions use a
CPU-driven SMMUv2 root switch instead, so the client-visible ring never carries a trusted TTBR or
kernel fence address. The implementation waits synchronously for client IBs to reach full GPU idle
before it returns to the kernel root; the subsequent kernel fence gives the shared service its
normal completion token. This is a first functional path, not yet an asynchronous context scheduler.
Polaris A630v2 does not advertise `ADRENO_APRIV`. Matching KGSL allocates its CP ring and SQE
microcode as GPU-read-only without the privileged-memory flag, so ordinary read-only CP-fetch
mappings match that driver's A630v2 memory policy
([Polaris `adreno-gpulist.h`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno-gpulist.h),
[`adreno_ringbuffer.c`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno_ringbuffer.c),
[`adreno_a6xx.c`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno_a6xx.c)).
The reference A6xx context-switch sequence is documented in
([`a6xx_gpu.c`](https://android.googlesource.com/kernel/common/%2B/a0497251f2b055a137d62ed065286ba999647b3c/drivers/gpu/drm/msm/adreno/a6xx_gpu.c),
[`adreno_iommu.c`](https://android.googlesource.com/kernel/msm/%2B/33ba973747392d08905b14b9d47a1bfb9ffb20d0/drivers/gpu/msm/adreno_iommu.c)).
The `adreno=23` probe exercises the client-root submission path with a single memory-write IB and
private output page. The destroy hook reclaims a client root only after a kernel-root fence and
complete unmap; otherwise it retains
the mappings and root. The protected-mode bracket is paired with an inspection of the local A630 SQE image
(`fw-gpu/a630_sqe.fw`, version `0x016ee19a`) using Mesa 25.0.7's `afuc-disasm`. Its IB dispatcher
tracks nesting depth in `$12`: the ring starts at level 0, `CP_INDIRECT_BUFFER` increments the level
before entering an IB, and `IN_IB_END` decrements it. The `CP_SET_PROTECTED_MODE` handler at SQE word
`0x376` and `CP_SMMU_TABLE_UPDATE` handler at word `0x92c` both branch to the `CP_NOP` packet-skip
path when `$12 != 0`. This confirms that client IBs cannot use those two packets to change CP
register-protection mode or SID0's translation table in this firmware image. The inspection is
source-level evidence only: retain the public-client gate until no-APRIV CP-page isolation, the SQE
guard, and the switch/teardown path have been validated on Polaris hardware.

GPU VA allocation and release now dispatch through the selected backend too. Polaris has a
software-only, locked 32-bit IOVA allocator. Backend SQE/ring/RPTR buffers use the private range
`[0x00010000, 0x40000000)`; the shared service receives client allocations only from
`[0x40000000, 0x100000000)`. Both allocators round to 64-KiB extents and accept exact frees. This
address partition complements per-client stage-1 roots: each root contains read-only CP fetch pages
and only one client's BOs; writable completion scratch is kernel-root-only. The implementation
builds on the 4K LPAE tables and active SID0 context. User submissions remain disabled until the
step 23 client-root IB, ring confidentiality, completion after return to the kernel root, and the
orderly stop/quiesce path are validated on the device.

The pinned Mesa 25.0.7 Turnip source selects its userspace-I/O-virtual-address path when both
`MSM_PARAM_VA_START` and `MSM_PARAM_VA_SIZE` succeed. That path allocates addresses from the
reported aperture and calls `MSM_INFO_SET_IOVA` for each GEM buffer. HakiosGPU instead assigns and
maps each buffer's address in the selected backend, exposes it through `MSM_INFO_GET_IOVA`, and
currently rejects `MSM_INFO_SET_IOVA`. Keep the two VA parameters unsupported until the service can
safely implement reassignment; Turnip then takes its kernel-assigned-address path and obtains each
buffer address through `GET_IOVA`. This matches Polaris's existing client aperture and per-client
mapping model. This is source inspection only; Turnip allocation and rendering have not been
exercised on the phone.

Turnip also treats `MSM_PARAM_TIMESTAMP` as the A6xx always-on GPU counter, with a fixed 19.2-MHz
period. The shared Redwood profile reports `mach_absolute_time()`, which is not that counter. The
Polaris backend now reads the GMU always-on counter with the A6xx high/low/high sequence, and only
answers while the GMU firmware, CX domain and GMU clock are confirmed active. This source-level
compatibility fix still needs runtime validation on the phone.

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
at most 128 IBs and only client IOVAs in `[0x40000000, 0x100000000)`. The local A630 SQE image rejects
client-IB `CP_SMMU_TABLE_UPDATE` and `CP_SET_PROTECTED_MODE` packets based on IB nesting depth; this
does not establish that all other privileged commands or private-page accesses are blocked. The
public `HakiosGPU` service must only start after its selected backend has completed CP bring-up,
command isolation review, and bounded hardware submission checks.

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
   of these write stages has been run. Step 22 probes CPU-driven CB0 root switching at a full
   A6xx idle boundary; step 23 dispatches one client-root IB and verifies its output plus the
   kernel-root fence. Neither has been run on the phone. Verify the
   Apps RSC transaction, sustained bandwidth/DCVS
   behavior, GX sequencing, core readbacks, CP idle/error status, fence visibility and PAS cleanup
   against Polaris before proceeding. Keep step 7 limited to PAS staging and shutdown. The client
   submit path now switches roots from the CPU only after a full CP/RBBM idle boundary, clears the
   shared read-only ring before each client job, submits no trusted TTBR or kernel-fence address
   while the client root is active, then returns to the kernel root before issuing the fence packet.
   The path waits synchronously at the client-root boundary; it is not yet a multi-context async
   scheduler. Step 23 exercises one CP memory-write IB through this path. Validate register
   protection, root switching, ring scrubbing, fence visibility,
   orderly stop and loader-return quiesce before accepting submissions. The `gpu=1` startup path
   now requests bounded orderly stop after a failed guarded bring-up. The stop path can roll back a
   verified SMMU-only state through step 11; add and validate cleanup for later partial-start
   boundaries on the phone. It does not yet remove all inherited CX/GMU parent-domain and clock
   votes.
   **Gate:** GMU firmware reaches its documented ready state; SMMU and secure firmware report no
   faults; client workloads and orderly stop/loader-return quiesce are validated on the phone.
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
   verify steps 19–23's bounded SQE/CP bootstrap, ZAP authentication, secure-mode transition,
   kernel fence write, CPU-driven CB0 root switching, and client-root IB output. Validate the
   no-APRIV client path at a full A6xx idle boundary, confirm that ring scrubbing prevents stale
   trusted packets from reaching one client, and verify the completion fence only after returning
   to the kernel root. Then validate production register protection, ring/fence mappings, mapper behavior and
   the normal-stop path. Qualcomm's
   A630 KGSL path sets the SQE base and authenticates ZAP through PIL before releasing SQE halt,
   submits `CP_ME_INIT`, then uses `CP_SET_SECURE_MODE(0)` to leave secure mode
   ([`adreno_a6xx.c`](https://android.googlesource.com/kernel/msm/%2B/eb78ba4b034e826f66279bd30408d96227a67210/drivers/gpu/msm/adreno_a6xx.c),
   [`adreno.c`](https://android.googlesource.com/kernel/msm/%2B/604f9dc59bcb86b3967048ebe36c48aff59b2592/drivers/gpu/msm/adreno.c),
   [`adreno_ringbuffer.c`](https://android.googlesource.com/kernel/msm/%2B/3a9ea3d5335330e66dc313253c055442c9db7b8e/drivers/gpu/msm/adreno_ringbuffer.c)). Keep the backend disabled
   until those pieces are verified.
   **Gate:** invalid IOVAs fault; client IBs cannot write SQE/ring or access the kernel fence page;
   commands from one client cannot inspect another client's ring contents; bounded test commands
   complete and write back expected data.
6. **End-to-end rendering.** Expose the A630 profile through HakiosGPU; exercise Mesa/freedreno
   buffer allocation, mappings, command submission, synchronization, surfaces and display; then
   exercise both the `metal2vulkan` route and `hakios-metal`'s A630 profile on the same service. The
   A630-specific `hakios-metal` build profile is now present: `Gpu::chipId()` must match Polaris's
   `0x06030001`, and the profile uses its 1 MiB GMEM plus Mesa's A630 compiler and device data. This
   is source/build evidence only; it does not validate the command stream or compositor on the
   phone. Keep the chip-ID gate and do not enable the Metal path until the A630 profile has been
   exercised on hardware. The shared service now gates new
   submissions after a long completion timeout and retains in-flight resources; add and validate
   an A630 hardware recovery/restart policy. Initial software resource ceilings are now in place:
   1 GiB of mapped GPU VA per client, 2 GiB globally, 1 GiB of active IOSurface allocations, and
   four cached IOSurface allocations of at most 32 MiB each. These limits include concurrent
   reservations and retained GPU mappings, but their fit for real Polaris workloads still needs
   device validation and tuning.
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
- Cache coherency, GPU interrupt delivery/acknowledgement, A630 hardware hang recovery/restart,
  DVFS/thermal policy, and full client-memory isolation on the actual phone. The Recovery-tree
  KGSL tuple and XNU-facing loader GIC description are known; the A630 backend still uses polled
  completion waits until the interrupt path is integrated and exercised on hardware.
- `hakios-metal` has an A630 build profile (`GPU_PROFILE=a630`) covering the Polaris chip ID, 1 MiB GMEM, Mesa A630
  compiler/device data, and the A630 CP_BLIT register switch. It has compile/source evidence only: no MIX 2S command stream,
  Metal compositor, window-server session, hang recovery or sustained workload has been validated. Keep the MIX 2S UI on the
  non-Metal rendering path until those hardware gates pass.
- Do not assume CP APRIV can separate kernel and client mappings on this device. The Polaris
  SDM845 kernel's `a630v2` catalog entries omit `ADRENO_APRIV`, and its A6xx ring startup path does
  not program `CP_APRIV_CNTL`. Matching KGSL allocates its CP ring and SQE microcode as
  GPU-read-only without the privileged-memory flag
  ([Polaris `adreno-gpulist.h`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno-gpulist.h),
  [Polaris `adreno_ringbuffer.c`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno_ringbuffer.c),
  [Polaris `adreno_a6xx.c`](https://github.com/LineageOS/android_kernel_xiaomi_sdm845/blob/lineage-22.0/drivers/gpu/msm/adreno_a6xx.c)).
  This supports ordinary read-only mappings for CP fetches; it does not provide client isolation
  because a client IB can still read the shared SQE/ring pages. The kernel-owned SQE and ring now
  use unprivileged read-only mappings; writable RPTR/fence scratch is mapped only in the kernel GPU
  root and omitted from client roots. This also does not provide safe asynchronous completion by
  itself. A CPU-driven CB0 context-switch helper, explicit `adreno=22` register probe, and
  `adreno=23` client-root IB probe now exist. Step 23 runs one memory-write IB against a private
  client-writable page, returns to the kernel root, and verifies a kernel-root fence. Both stages
  retain their tables if the root return cannot be confirmed. They have not run on-device. The
  production callback uses the same root-switch and ring-scrub sequence, but the userclient submit
  gate remains closed until hardware validation. The local SQE guards `CP_SMMU_TABLE_UPDATE` and
  `CP_SET_PROTECTED_MODE` inside IBs, but broader command isolation and safe context-switch/teardown
  behavior still require hardware validation.
- A630 stop after SQE startup accepts successful idle CP boundaries through the kernel-owned fence
  stage and is still unverified on a phone. It requires CP idle and a completed VBIF drain before requesting
  legacy-HFI slumber; it then checks GMU/CX/GX state, asserts CM3 reset, shuts down PAS, detaches
  SID4/SID5/SID0 and releases private CP buffers. `gmu_domain_ready` remains only an initial
  availability observation. A failed check leaves the affected resources attached. The normal
  stop path does not perform full CX/GMU rail, RPMh, clock or parent-domain power removal, and it
  does not recover a hung GMU. Partial orderly-start cleanup remains open; the panic/loader-return
  `quiesce` implementation still needs device validation. Qualcomm's A630 legacy-HFI request and
  no-GBIF XIN drain are referenced in
  [`adreno_a6xx_gmu.c`](https://android.googlesource.com/kernel/msm/%2B/b6410c3f75259c0ca2df45a01838d2e46272723e/drivers/gpu/msm/adreno_a6xx_gmu.c) and
  [`a6xx_gpu.c`](https://android.googlesource.com/kernel/common/%2B/63407d3081a6b406d94bd7d96ff2ca38fa7df236/drivers/gpu/drm/msm/adreno/a6xx_gpu.c); the Polaris implementation is not hardware-validated.

These are gates to resolve with source evidence and controlled device runs, not values to infer
from Redwood or fill with guessed constants. The Dipper repository is only a comparison reference:
its Adreno source is substantially the same Redwood backend and does not supply an A630/SDM845
implementation.
