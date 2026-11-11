/*
 * Bring-up and firmware loading for the Adreno 630 of the Xiaomi MIX 2S (Polaris, SDM845).
 * This is deliberately a separate backend from the Redwood/SC7280 Adreno 642L driver: the
 * GPUCC and KGSL SMMU are different blocks, and the latter is SMMUv2 rather than SMMUv500.
 *
 * adreno=1..6 performs read-only probes. Explicit adreno=7 stages and releases PAS metadata,
 * adreno=10 loads GMU ITCM while reset stays asserted, adreno=11 installs separate GMU-kernel and
 * GMU-user SMMUv2 contexts and SID routes, and adreno=12 takes the default DDR vote through Apps RSC
 * before booting the GMU and checking its HFI firmware version. adreno=13 sends the A630 HFI performance table;
 * adreno=14 sends that table and the reconstructed DDR/CNOC bandwidth table, closes the legacy
 * HFI message phase with HFI_TEST, requests initial DCVS, then releases the temporary host DDR vote.
 * Explicit adreno=15 only builds the A630 CP_ME_INIT and CP_SET_SECURE_MODE bootstrap packets in
 * local memory for inspection. adreno=16 prepares the SQE, CP ring, and RPTR scratch in the
 * detached GPU SID0 page table; adreno=17 maps those buffers, starts GMU/HFI, then requests GX
 * power through the A630 GPU OOB and verifies the GFX3D clock. Step 18 repeats that guarded startup
 * then programs A630 HWCG, VBIF, UCHE, CP defaults, and protected-register ranges. Step 19
 * authenticates ZAP before SQE/CP startup; step 20 submits CP_SET_SECURE_MODE(0) and waits for idle.
 * Step 21 is a kernel-owned fence smoke test only. A production ring/fence callback path follows
 * it but remains gated; client submissions, runtime GPU DCVS, and rendering remain later stages.
 * Write stages are guarded by runtime preflight/readbacks and are not to be invoked on hardware
 * during this bring-up. `ready()` remains false.
 *
 * Addresses and sizes are from the checked-in Polaris Android DTB:
 *   qcom,gpucc@5090000 (qcom,gpucc-sdm845-v2)
 *   qcom,gmu (qcom,gpu-gmu)
 *   qcom,kgsl-iommu (qcom,kgsl-smmu-v2)
 *   qcom,kgsl-3d0@5000000 (qcom,chipid = 0x06030001)
 * SDM845 GPUCC offsets are cross-checked against Linux drivers/clk/qcom/gpucc-sdm845.c. The
 * separate GFXCC GFX3D branch is cross-checked against Google's downstream kernel/msm source;
 * the AOSP sdm845-v2 tree gives both controllers the same resource base.
 * GMU/A6xx register offsets are from Linux drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml.
 */
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "hakios.gpu_backend.h"
#include "hakios.adreno.hfi.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern lck_mtx_t hakios_rsc_lock;

#define SDM845_SOC_BASE                 0x00100000ul
#define POLARIS_GPUCC_PHYS              0x05090000ul
#define POLARIS_GPUCC_SIZE              0x00009000ul
#define POLARIS_GMU_PHYS                0x0506a000ul
#define POLARIS_GMU_SIZE                0x00030000ul
#define POLARIS_SMMU_PHYS               0x05040000ul
#define POLARIS_SMMU_SIZE               0x00010000ul
#define POLARIS_GPU_PHYS                0x05000000ul
#define POLARIS_GPU_SIZE                0x00040000ul

#define POLARIS_GPU_DT_PATH             "/soc/qcom,kgsl-3d0@5000000"
#define POLARIS_GMU_DT_PATH             "/soc/qcom,gmu"
#define POLARIS_GPUCC_DT_PATH           "/soc/qcom,gpucc@5090000"
#define POLARIS_GFXCC_DT_PATH           "/soc/qcom,gfxcc@5090000"
#define POLARIS_GPU_CX_GDSC_DT_PATH     "/soc/qcom,gdsc@0x509106c"
#define POLARIS_GPU_GX_GDSC_DT_PATH     "/soc/qcom,gdsc@0x509100c"
#define POLARIS_GCC_DT_PATH             "/soc/qcom,gcc@100000"
#define POLARIS_CPUCC_DT_PATH           "/soc/qcom,cpucc@0x17d41000"
#define POLARIS_RPMH_DRV_DT_PATH        "/soc/mailbox@179e0000"
#define POLARIS_CMD_DB_DT_PATH          "/soc/qcom,cmd-db@861e0000"
#define POLARIS_CMD_DB_RESERVATION_PATH "/reserved-memory/removed_region@85fc0000"
#define POLARIS_GPU_PWRLEVELS_DT_PATH   "/soc/qcom,kgsl-3d0@5000000/qcom,gpu-pwrlevels"
#define POLARIS_GMU_PWRLEVELS_DT_PATH   "/soc/qcom,gmu/qcom,gmu-pwrlevels"
#define POLARIS_SMMU_DT_PATH            "/soc/arm,smmu-kgsl@5040000"
#define POLARIS_PIL_GPU_DT_PATH         "/soc/qcom,kgsl-hyp"
#define POLARIS_GPU_MEMORY_DT_PATH      "/reserved-memory/gpu_region@0x8c415000"
#define POLARIS_GPU_CHIP_ID             0x06030001u
#define A630_MDT_MAX_SEGMENTS           16u
#define A630_MDT_MAX_BYTES              0x10000u
#define A630_MDT_HASH(flags)            ((((flags) >> 24) & 7u) == 2u)
#define A630_MDT_RELOCATABLE            (1u << 27)
#define POLARIS_PAS_GPU                 13u

/* A630's legacy KGSL A6xx CP_INIT packet and the secure-mode switch packet. */
#define A630_CP_ME_INIT                 0x48u
#define A630_CP_SET_SECURE_MODE         0x66u
#define A630_CP_MEM_WRITE               0x3du
#define A630_CP_WAIT_MEM_WRITES         0x12u
#define A630_CP_INDIRECT_BUFFER_PFE    0x3fu
#define A630_CP_EVENT_WRITE             0x46u
#define A630_CP_CCU_INVALIDATE_DEPTH    0x18u
#define A630_CP_CCU_INVALIDATE_COLOR    0x19u
#define A630_CP_CACHE_FLUSH_TS          0x04u
#define A630_CP_INIT_ORDINALS           11u
#define A630_CP_INIT_MAX_CONTEXT        (1u << 0)
#define A630_CP_INIT_ERROR_DETECTION    (1u << 1)
#define A630_CP_INIT_HEADER_DUMP        (1u << 2)
#define A630_CP_INIT_DEFAULT_RESET      (1u << 3)
#define A630_CP_INIT_UCODE_WORKAROUNDS  (1u << 5)
#define A630_CP_INIT_OPERATION_MODE     (1u << 6)
#define A630_CP_INIT_REG_INIT_LIST      (1u << 7)
#define A630_CP_INIT_MASK               (A630_CP_INIT_MAX_CONTEXT | A630_CP_INIT_ERROR_DETECTION | \
	A630_CP_INIT_HEADER_DUMP | A630_CP_INIT_DEFAULT_RESET | A630_CP_INIT_UCODE_WORKAROUNDS | \
	A630_CP_INIT_OPERATION_MODE | A630_CP_INIT_REG_INIT_LIST)
#define A630_CP_INIT_PACKET_DWORDS      (1u + A630_CP_INIT_ORDINALS)
#define A630_CP_SECURE_MODE_DWORDS      2u
#define A630_CP_FENCE_TEST_DWORDS       5u
#define A630_CP_FENCE_TEST_VALUE        0xa6302014u
#define A630_CP_FENCE_TEST_OFFSET       64u
#define A630_CP_RING_BYTES              (32u * 1024u)
#define A630_CP_RING_DWORDS             (A630_CP_RING_BYTES / sizeof(uint32_t))
#define A630_CP_SUBMIT_MAX_IBS           128u
#define A630_CP_SUBMIT_FIXED_DWORDS      9u
#define A630_CP_SUBMIT_MAX_DWORDS        (A630_CP_SUBMIT_FIXED_DWORDS + 4u * A630_CP_SUBMIT_MAX_IBS)
#define A630_CP_RB_CNTL_DEFAULT         0x0000020cu
#define A630_CP_INIT_TIMEOUT_US         2000000u
#define A630_CP_INIT_POLL_US            100u
#define A630_CP_INTERRUPT_ERROR_MASK    0x000000f7u
#define A630_RBBM_CP_BUSY_MASK          0x00000007u
#define A630_SQE_FILE_PREFIX_BYTES      4u

/*
 * SDM845 GCC GPU clock branches share the GPU register window's prerequisites:
 * gcc-sdm845.c puts cfg_ahb at base+0x71004 and memnoc_gfx at base+0x7100c.
 */
#define POLARIS_GCC_GPU_CLOCKS_PHYS      0x00171000ul
#define POLARIS_GCC_GPU_CLOCKS_SIZE      0x00001000ul
#define GCC_GPU_CFG_AHB_CBCR             0x0004u
#define GCC_GPU_MEMNOC_GFX_CBCR          0x000cu

/* SDM845's AOP command database, reserved by the SoC DT. */
#define POLARIS_CMD_DB_PHYS               0x85fe0000ul
#define POLARIS_CMD_DB_SIZE               0x00020000ul
#define POLARIS_CMD_DB_HEADER_SIZE        0x90u
#define POLARIS_CMD_DB_SLAVE_COUNT        8u
#define POLARIS_CMD_DB_SLAVE_SIZE         16u
#define POLARIS_CMD_DB_ENTRY_SIZE         24u
#define POLARIS_CMD_DB_MAX_ENTRIES        512u
#define POLARIS_CMD_DB_MAX_LEVELS         16u
#define POLARIS_MAX_GPU_PWRLEVELS         16u
#define POLARIS_MAX_GMU_PWRLEVELS         16u
#define POLARIS_MAX_GPU_BUS_LEVELS        16u
#define POLARIS_EXPECTED_GPU_BUS_LEVELS   13u
#define POLARIS_EXPECTED_GMU_BUS_LEVELS   2u
#define POLARIS_MAX_CLOCK_RATE_HZ         1000000000u

/* SDM845 Apps RSC DRV2; resource windows and TCS counts come from the live DT. */
#define POLARIS_RPMH_DRV_ID                0x0000u
#define POLARIS_RPMH_DRV_SOLVER_CONFIG    0x0004u
#define POLARIS_RPMH_DRV_CONFIG           0x000cu
#define POLARIS_RPMH_ID_MAJOR_SHIFT       16u
#define POLARIS_RPMH_ID_MINOR_SHIFT       8u
#define POLARIS_RPMH_ID_VERSION_MASK      0xffu
#define POLARIS_RPMH_TCS_IRQ_STATUS       0x0004u
#define POLARIS_RPMH_TCS_IRQ_CLEAR        0x0008u
#define POLARIS_RPMH_TCS_WAIT_FOR_DONE    0x0010u
#define POLARIS_RPMH_TCS_STATUS_V27       0x0018u
#define POLARIS_RPMH_TCS_CONTROL_V27      0x0014u
#define POLARIS_RPMH_TCS_CMD_ENABLE_V27   0x001cu
#define POLARIS_RPMH_TCS_STATUS_V30       0x0028u
#define POLARIS_RPMH_TCS_CONTROL_V30      0x0024u
#define POLARIS_RPMH_TCS_CMD_ENABLE_V30   0x002cu
#define POLARIS_RPMH_TCS_STRIDE           0x02a0u
#define POLARIS_RPMH_TCS_ACTIVE_TYPE      2u
#define POLARIS_RPMH_TCS_SLEEP_TYPE       0u
#define POLARIS_RPMH_TCS_WAKE_TYPE        1u
#define POLARIS_RPMH_TCS_CONTROL_TYPE     3u
#define POLARIS_RPMH_TCS_COMMAND_BASE     0x0030u
#define POLARIS_RPMH_TCS_COMMAND_STRIDE   0x0014u
#define POLARIS_RPMH_TCS_MSGID_WRITE_WAITED 0x00010108u
#define POLARIS_RPMH_TCS_ACTIVE_MODE      (1u << 16)
#define POLARIS_RPMH_TCS_TRIGGER          (1u << 24)
#define POLARIS_RPMH_TCS_POLL_LIMIT       2000u
#define POLARIS_RPMH_TCS_POLL_DELAY_US    50u
#define POLARIS_RPMH_DRV_NUM_TCS_SHIFT    6u
#define POLARIS_RPMH_DRV_NUM_TCS_MASK     0x3fu
#define POLARIS_RPMH_DRV_NCPT_SHIFT       27u
#define POLARIS_RPMH_DRV_NCPT_MASK        0x1fu

/* SDM845 GPUCC offsets, from gpucc-sdm845.c. */
#define GPUCC_GX_GDSCR                  0x100c
#define GPUCC_GX_GFX3D_CBCR             0x1054
#define GPUCC_CX_GDSCR                  0x106c
#define GPUCC_CX_GMU_CBCR               0x1098
#define GPUCC_CXO_CBCR                  0x109c
#define GPUCC_GMU_RCGR                  0x1120
#define GPUCC_GMU_RCGR_CFG              0x1124
#define GPUCC_PLL1_MODE                 0x0100
#define GDSC_POWER_ON                   (1u << 31)
#define CBCR_ENABLE                     (1u << 0)
#define CBCR_OFF                        (1u << 31)

/* ARM SMMUv2 identification registers. The Qualcomm v2 binding uses the same global offsets. */
#define SMMU_IDR0                       0x0020
#define SMMU_IDR1                       0x0024
#define SMMU_IDR2                       0x0028
#define SMMU_IDR7                       0x003c
#define SMMU_IDR0_S1TS                  (1u << 30)
#define SMMU_IDR0_S2TS                  (1u << 29)
#define SMMU_IDR0_SMS                   (1u << 27)
#define SMMU_IDR0_CTTW                  (1u << 14)
#define SMMU_IDR0_EXIDS                 (1u << 8)
#define SMMU_IDR0_NUMSIDB_SHIFT         9
#define SMMU_IDR0_NUMSIDB_MASK          0x0fu
#define SMMU_IDR0_NUMSMRG_MASK          0xffu
#define SMMU_IDR1_PAGESIZE_64K          (1u << 31)
#define SMMU_IDR1_NUMPAGENDXB_SHIFT      28
#define SMMU_IDR1_NUMPAGENDXB_MASK       0x07u
#define SMMU_IDR1_NUMS2CB_SHIFT         16
#define SMMU_IDR1_NUMCB_MASK            0xffu
#define SMMU_IDR2_PTFS_4K               (1u << 12)
#define SMMU_IDR2_PTFS_16K              (1u << 13)
#define SMMU_IDR2_PTFS_64K              (1u << 14)
#define SMMU_IDR2_IAS_MASK              0x0fu
#define SMMU_IDR2_OAS_SHIFT             4
#define SMMU_IDR2_UBS_SHIFT             8
#define SMMU_IDR2_SIZE_MASK             0x0fu
#define SMMU_IDR7_MAJOR_SHIFT           4
#define SMMU_IDR7_VERSION_MASK          0x0fu
#define SMMU_SCR0                       0x0000u
#define SMMU_SCR0_CLIENTPD              (1u << 0)
#define SMMU_SMR(n)                     (0x0800u + 4u * (n))
#define SMMU_SMR_VALID                  (1u << 31)
#define SMMU_SMR_ID_MASK                0x7fffu
#define SMMU_SMR_MASK_SHIFT             16u
#define SMMU_SMR_MASK_MASK              0x7fffu
#define SMMU_S2CR(n)                    (0x0c00u + 4u * (n))
#define SMMU_S2CR_TYPE_SHIFT            16
#define SMMU_S2CR_TYPE_MASK              0x3u
#define SMMU_S2CR_TYPE_TRANS             0u
#define SMMU_S2CR_TYPE_BYPASS            1u
#define SMMU_S2CR_TYPE_FAULT             2u
#define SMMU_S2CR_TYPE_FIELD_MASK        (SMMU_S2CR_TYPE_MASK << SMMU_S2CR_TYPE_SHIFT)
#define SMMU_S2CR_CBNDX_MASK            0xffu
#define SMMU_GR1                       0x1000u
#define SMMU_CBAR(n)                    (SMMU_GR1 + 4u * (n))
#define SMMU_CBA2R(n)                   (SMMU_GR1 + 0x0800u + 4u * (n))
#define SMMU_CBAR_TYPE_SHIFT             16u
#define SMMU_CBAR_TYPE_MASK              0x3u
#define SMMU_CBAR_TYPE_S1_S2_BYPASS      1u
#define SMMU_CBA2R_RW64                  (1u << 0)
#define SMMU_CBAR_S1_BPSHCFG_SHIFT       8u
#define SMMU_CBAR_S1_MEMATTR_SHIFT       12u
#define SMMU_CBAR_S1_MEMATTR_WB          0xfu
#define SMMU_CB_SCTLR                   0x0000u
#define SMMU_CB_ACTLR                   0x0004u
#define SMMU_CB_TCR2                    0x0010u
#define SMMU_CB_TTBR0                   0x0020u
#define SMMU_CB_TTBR1                   0x0028u
#define SMMU_CB_TCR                     0x0030u
#define SMMU_CB_CONTEXTIDR              0x0034u
#define SMMU_CB_MAIR0                   0x0038u
#define SMMU_CB_MAIR1                   0x003cu
#define SMMU_CB_FSR                     0x0058u
#define SMMU_CB_TCR_T0SZ_MASK            0x3fu
#define SMMU_CB_TCR_IRGN0_SHIFT           8u
#define SMMU_CB_TCR_ORGN0_SHIFT          10u
#define SMMU_CB_TCR_SH0_SHIFT            12u
#define SMMU_CB_TCR_EPD1                 (1u << 23)
#define SMMU_CB_TCR2_PASIZE_MASK          0x0fu
#define SMMU_CB_TCR2_AS                  (1u << 4)
#define SMMU_CB_TCR2_SEP_SHIFT           15u
#define SMMU_CB_TCR2_SEP_UPSTREAM         7u
#define SMMU_CB_SCTLR_TRE                (1u << 1)
#define SMMU_CB_SCTLR_AFE                (1u << 2)
#define SMMU_CB_SCTLR_CFRE               (1u << 5)
#define SMMU_CB_SCTLR_S1_ASIDPNE          (1u << 12)
#define SMMU_CB_MAIR0_LPAE               0xf404ff44u
#define SMMU_CB_TLBIASID                0x0610u
#define SMMU_CB_TLBSYNC                 0x07f0u
#define SMMU_CB_TLBSTATUS               0x07f4u
#define SMMU_SCTLR_M                    (1u << 0)

/* A6xx GMU register XML uses global word offsets; the Polaris GMU window begins at word 0x1a800. */
#define GMU_WORD(n)                     (4u * (n))
#define GMU_ALWAYS_ON_COUNTER_L         GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H         GMU_WORD(0x5089)
#define GMU_CM3_FW_INIT_RESULT          GMU_WORD(0x501c)
#define GMU_CM3_SYSRESET                GMU_WORD(0x5000)
#define GMU_CM3_BOOT_CONFIG             GMU_WORD(0x5001)
#define GMU_GENERAL_7                   GMU_WORD(0x5007)
#define GMU_CM3_ITCM_START              GMU_WORD(0x0c00)
#define GMU_SYS_BUS_CONFIG              GMU_WORD(0x4c0f)
#define GMU_ICACHE_CONFIG               GMU_WORD(0x4c00)
#define GMU_DCACHE_CONFIG               GMU_WORD(0x4c01)
#define GMU_PWR_COL_INTER_FRAME_CTRL    GMU_WORD(0x50c0)
#define GMU_PWR_COL_CP_MSG              GMU_WORD(0x5100)
#define GMU_PWR_COL_CP_RESP             GMU_WORD(0x5101)
#define GMU_HFI_VERSION_INFO            GMU_WORD(0x5181)
#define GMU_HFI_SFR_ADDR                 GMU_WORD(0x5182)
#define GMU_HFI_QTBL_INFO                GMU_WORD(0x5184)
#define GMU_HFI_QTBL_ADDR                GMU_WORD(0x5185)
#define GMU_HFI_CTRL_INIT                GMU_WORD(0x5186)
#define GMU_HFI_CTRL_STATUS              GMU_WORD(0x5187)
#define GMU_GMU2HOST_INTR_CLR            GMU_WORD(0x5191)
#define GMU_GMU2HOST_INTR_INFO           GMU_WORD(0x5192)
#define GMU_HOST2GMU_INTR_SET            GMU_WORD(0x5194)
#define GMU_REGISTER_WINDOW_BASE_WORD     0x1a800u
#define GMU_REGISTER_GLOBAL_WORD(n)      GMU_WORD((n) - GMU_REGISTER_WINDOW_BASE_WORD)
#define GMU_BOOT_SLUMBER_OPTION          GMU_REGISTER_GLOBAL_WORD(0x1cbf8u)
#define GMU_GX_VOTE_IDX                  GMU_REGISTER_GLOBAL_WORD(0x1cbf9u)
#define GMU_MX_VOTE_IDX                  GMU_REGISTER_GLOBAL_WORD(0x1cbfau)
#define GMU_DCVS_ACK_OPTION              GMU_REGISTER_GLOBAL_WORD(0x1cbfcu)
#define GMU_DCVS_PERF_SETTING            GMU_REGISTER_GLOBAL_WORD(0x1cbfdu)
#define GMU_DCVS_BW_SETTING              GMU_REGISTER_GLOBAL_WORD(0x1cbfeu)
#define GMU_DCVS_RETURN                  GMU_REGISTER_GLOBAL_WORD(0x1cbffu)
#define GMU_AHB_FENCE_RANGE_0            GMU_WORD(0x9311)
#define A630_GMU_FW_MAX_BYTES           0x8000u
#define A630_GMU_FW_READY                0xbabefaceu
#define A630_GMU_BOOT_TCM                2u
#define A630_GMU_BOOT_COLD               1u
#define A630_GMU_HFI_MESSAGE_INTERRUPT   (1u << 0)
#define A630_GMU_HFI_QUEUE_START_POLLS   10000u
#define A630_GMU_HFI_ACK_POLLS           10000u
#define A630_GMU_HFI_POLL_DELAY_US       100u
#define A630_GMU_OOB_BOOT_SET_MASK       (1u << 22)
#define A630_GMU_OOB_BOOT_ACK_MASK       (1u << 30)
#define A630_GMU_OOB_DCVS_SET_MASK       (1u << 23)
#define A630_GMU_OOB_DCVS_ACK_MASK       (1u << 31)
#define A630_GMU_OOB_GPU_SET_MASK        (1u << 16)
#define A630_GMU_OOB_GPU_ACK_MASK        (1u << 24)
#define A630_GMU_DCVS_ACK_NONBLOCK       0u
#define A630_GMU_DCVS_CLKSET_AT_LEAST    3u
#define A630_GMU_HFI_DUMP_BYTES          0x4000u
#define A630_GMU_LOG_BYTES               0x1000u
#define A630_GMU_DUMP_VA                 0x60100000u
#define A630_GMU_LOG_VA                  0x60200000u
#define A630_GMU_HFI_FENCE_RANGE         ((1u << 31) | ((0xau << 2) << 18) | 0x8a0u)
#define A630_GMU_HFI_SFR_CHIP_ID         0x06030100u
#define A630_GMU_SHARED_BYTES            (POLARIS_A630_HFI_ALLOCATION_BYTES + \
	A630_GMU_HFI_DUMP_BYTES + A630_GMU_LOG_BYTES)

/* A6xx GPU offsets are in 32-bit words in the register XML. */
#define GPU_WORD(n)                     (4u * (n))
#define GPU_RBBM_INT_0_STATUS           GPU_WORD(0x0201)
#define GPU_RBBM_STATUS                 GPU_WORD(0x0210)
#define GPU_CP_HW_FAULT                 GPU_WORD(0x0821)
#define GPU_CP_INTERRUPT_STATUS         GPU_WORD(0x0823)
#define GPU_CP_PROTECT_STATUS           GPU_WORD(0x0824)
#define GPU_CP_RB_BASE                  GPU_WORD(0x0800)
#define GPU_CP_RB_BASE_HI               GPU_WORD(0x0801)
#define GPU_CP_RB_CNTL                  GPU_WORD(0x0802)
#define GPU_CP_RB_RPTR_ADDR_LO          GPU_WORD(0x0804)
#define GPU_CP_RB_RPTR_ADDR_HI          GPU_WORD(0x0805)
#define GPU_CP_RB_RPTR                  GPU_WORD(0x0806)
#define GPU_CP_RB_WPTR                  GPU_WORD(0x0807)
#define GPU_CP_SQE_CNTL                 GPU_WORD(0x0808)
#define GPU_CP_SQE_INSTR_BASE_LO        GPU_WORD(0x0830)
#define GPU_CP_SQE_INSTR_BASE_HI        GPU_WORD(0x0831)
#define GPU_RBBM_CLOCK_CNTL             GPU_WORD(0x00ae)
#define GPU_RBBM_PERFCTR_GPU_BUSY_MASKED GPU_WORD(0x050b)
#define GPU_CP_ROQ_THRESHOLDS_1         GPU_WORD(0x08c1)
#define GPU_CP_ROQ_THRESHOLDS_2         GPU_WORD(0x08c2)
#define GPU_CP_MEM_POOL_SIZE            GPU_WORD(0x08c3)
#define GPU_CP_PROTECT_CNTL             GPU_WORD(0x084f)
#define GPU_CP_PROTECT_REG              GPU_WORD(0x0850)
#define GPU_CP_AHB_CNTL                 GPU_WORD(0x098d)
#define GPU_RBBM_PERFCTR_CNTL           GPU_WORD(0x0500)
#define GPU_RBBM_INTERFACE_HANG_INT_CNTL GPU_WORD(0x001f)
#define GPU_PC_DBG_ECO_CNTL             GPU_WORD(0x9e00)
#define GPU_UCHE_MODE_CNTL              GPU_WORD(0x0e01)
#define GPU_UCHE_WRITE_RANGE_MAX_LO     GPU_WORD(0x0e05)
#define GPU_UCHE_WRITE_RANGE_MAX_HI     GPU_WORD(0x0e06)
#define GPU_UCHE_WRITE_THRU_BASE_LO     GPU_WORD(0x0e07)
#define GPU_UCHE_WRITE_THRU_BASE_HI     GPU_WORD(0x0e08)
#define GPU_UCHE_TRAP_BASE_LO           GPU_WORD(0x0e09)
#define GPU_UCHE_TRAP_BASE_HI           GPU_WORD(0x0e0a)
#define GPU_UCHE_GMEM_RANGE_MIN_LO      GPU_WORD(0x0e0b)
#define GPU_UCHE_GMEM_RANGE_MIN_HI      GPU_WORD(0x0e0c)
#define GPU_UCHE_GMEM_RANGE_MAX_LO      GPU_WORD(0x0e0d)
#define GPU_UCHE_GMEM_RANGE_MAX_HI      GPU_WORD(0x0e0e)
#define GPU_UCHE_CACHE_WAYS             GPU_WORD(0x0e17)
#define GPU_UCHE_FILTER_CNTL            GPU_WORD(0x0e18)
#define GPU_UCHE_CLIENT_PF              GPU_WORD(0x0e19)
#define GPU_RB_NC_MODE_CNTL             GPU_WORD(0x8e08)
#define GPU_TPL1_NC_MODE_CNTL           GPU_WORD(0xb604)
#define GPU_SP_NC_MODE_CNTL             GPU_WORD(0xae02)
#define GPU_RBBM_VBIF_CLIENT_QOS_CNTL   GPU_WORD(0x0010)
#define GPU_VBIF_GATE_OFF_WRREQ_EN      GPU_WORD(0x302a)
#define A630_GMEM_BASE                  0x00100000u
#define A630_GMEM_SIZE                  0x00100000u
#define A630_HIGHEST_BANK_BIT           15u
#define A630_MAX_PROTECTED_REGS         32u
#define A630_FIXED_PROTECTED_REGS       27u
#define A630_SMMU_PROTECT_BASE_WORD     0x00010000u
#define A630_SMMU_PROTECT_RANGE_WORDS   0x00002000u
#define A630_HWC_GMU_SPTPRAC_WORD       0x0001a880u
#define A630_GMU_AO_CGC_MODE_WORD       0x00023b09u
#define A630_GMU_AO_CGC_DELAY_WORD      0x00023b0au
#define A630_GMU_AO_CGC_HYST_WORD       0x00023b0bu
#define A630_RBBM_CLOCK_CNTL_ON         0x8aa8aa02u

struct polaris_a630_protected_reg {
	uint32_t base;
	uint32_t count;
	uint32_t read_protect;
};

static const struct polaris_a630_protected_reg polaris_a630_protected_regs[A630_FIXED_PROTECTED_REGS] = {
	{ 0x600, 0x51, 0 }, { 0xae50, 0x2, 1 }, { 0x9624, 0x13, 1 }, { 0x8630, 0x8, 1 },
	{ 0x9e70, 0x1, 1 }, { 0x9e78, 0x187, 1 }, { 0xf000, 0x810, 1 }, { 0xfc00, 0x3, 0 },
	{ 0x50e, 0x0, 1 }, { 0x50f, 0x0, 0 }, { 0x510, 0x0, 1 }, { 0x0, 0x4f9, 0 },
	{ 0x501, 0xa, 0 }, { 0x511, 0x44, 0 }, { 0xe00, 0xe, 1 }, { 0x8e00, 0x0, 1 },
	{ 0x8e50, 0xf, 1 }, { 0xbe02, 0x0, 1 }, { 0xbe20, 0x11f3, 1 }, { 0x800, 0x82, 1 },
	{ 0x8a0, 0x8, 1 }, { 0x8ab, 0x19, 1 }, { 0x900, 0x4d, 1 }, { 0x98d, 0x76, 1 },
	{ 0x8d0, 0x23, 0 }, { 0x980, 0x4, 0 }, { 0xa630, 0x0, 1 },
};

/* A630 uses a 32-bit GPU VA aperture split between private backend buffers and client BOs. */
#define POLARIS_GPU_PRIVATE_VA_START     0x00010000ull
#define POLARIS_GPU_PRIVATE_VA_END       0x40000000ull
#define POLARIS_GPU_PRIVATE_VA_MAX_ALLOCS 16u
#define POLARIS_GPU_VA_START             0x40000000ull
#define POLARIS_GPU_VA_END               0x100000000ull
#define POLARIS_GPU_VA_ALIGN             0x10000ull
#define POLARIS_GPU_VA_GRANULES          ((POLARIS_GPU_VA_END - POLARIS_GPU_VA_START) / POLARIS_GPU_VA_ALIGN)
#define POLARIS_GPU_VA_MAX_RANGES        ((POLARIS_GPU_VA_GRANULES + 1) / 2)
#define POLARIS_GPU_VA_MAX_ALLOCS        POLARIS_GPU_VA_GRANULES
#define POLARIS_SMMU_MAX_STREAM_IDS      8u
#define POLARIS_SMMU_MAX_CONTEXT_BANKS   256u
#define POLARIS_SMMU_TABLE_BYTES         4096u
#define POLARIS_SMMU_ADDRESS_SPACE_COUNT 3u
#define POLARIS_SMMU_TABLES_PER_SPACE    2053u /* one L1, four L2, 2048 L3 tables cover 4 GiB */
#define POLARIS_SMMU_MAX_TABLES          (POLARIS_SMMU_ADDRESS_SPACE_COUNT * POLARIS_SMMU_TABLES_PER_SPACE)
#define POLARIS_SMMU_CACHE_INDEX_NONE    UINT32_MAX
#define POLARIS_SMMU_PTE_TABLE           3ull
#define POLARIS_SMMU_PTE_PAGE            3ull
#define POLARIS_SMMU_PTE_UNPRIVILEGED    (1ull << 6)
#define POLARIS_SMMU_PTE_READ_ONLY       (1ull << 7)
#define POLARIS_SMMU_PTE_SHARED          (2ull << 8)
#define POLARIS_SMMU_PTE_ACCESSED        (1ull << 10)
#define POLARIS_SMMU_PTE_NOT_GLOBAL      (1ull << 11)
#define POLARIS_SMMU_PTE_ADDRESS         0x0000fffffffff000ull
#define POLARIS_SMMU_PTE_BASE            (POLARIS_SMMU_PTE_SHARED | POLARIS_SMMU_PTE_ACCESSED | \
					      POLARIS_SMMU_PTE_NOT_GLOBAL)
#define POLARIS_SMMU_CACHE_LINE           64u

struct gpu_va_range {
	uint64_t start;
	uint64_t size;
};

static LCK_GRP_DECLARE(polaris_gpu_lck_grp, "polaris-gpu-va");
static LCK_MTX_DECLARE(polaris_gpu_va_lock, &polaris_gpu_lck_grp);
static LCK_GRP_DECLARE(polaris_smmu_lck_grp, "polaris-smmu-pt");
static LCK_MTX_DECLARE(polaris_smmu_pt_lock, &polaris_smmu_lck_grp);
static struct gpu_va_range polaris_gpu_va_free[POLARIS_GPU_VA_MAX_RANGES];
static struct gpu_va_range polaris_gpu_va_allocated[POLARIS_GPU_VA_MAX_ALLOCS];
static struct gpu_va_range polaris_gpu_private_va_allocated[POLARIS_GPU_PRIVATE_VA_MAX_ALLOCS];
static uint32_t polaris_gpu_va_free_count;
static uint32_t polaris_gpu_va_allocated_count;
static uint32_t polaris_gpu_private_va_allocated_count;
static uint64_t polaris_gpu_private_va_next = POLARIS_GPU_PRIVATE_VA_START;
static bool polaris_gpu_va_initialized;

struct polaris_smmu_table {
	uint64_t *entries;
	uint64_t phys;
	bool dirty;
};

struct polaris_gpu_dma_buffer {
	vm_offset_t virtual;
	vm_size_t allocation_size;
	uint64_t requested_size;
	uint64_t physical;
	uint64_t gpu_address;
	bool gpu_read_only;
};

enum polaris_smmu_address_space_id {
	POLARIS_SMMU_SPACE_GPU,
	POLARIS_SMMU_SPACE_GMU_KERNEL,
	POLARIS_SMMU_SPACE_GMU_USER,
};

struct polaris_smmu_address_space {
	const char *name;
	uint64_t pte_attributes;
	struct polaris_smmu_table *root;
	struct polaris_smmu_table *cached_l2;
	struct polaris_smmu_table *cached_l3;
	uint32_t cached_l1_index;
	uint32_t cached_l2_index;
	uint32_t context_bank;
	uint32_t asid;
	bool context_active;
};

struct polaris_smmu_context_image {
	uint32_t cbar;
	uint32_t cba2r;
	uint32_t tcr;
	uint32_t tcr2;
	uint32_t mair0;
	uint32_t mair1;
	uint32_t sctlr;
	uint64_t ttbr0;
	uint64_t ttbr1;
};

struct polaris_smmu_route_candidate {
	bool valid;
	bool stream_matching;
	bool existing_smr;
	uint32_t stream_id;
	uint32_t slot;
	uint32_t context_bank;
	uint32_t expected_smr;
	uint32_t expected_s2cr;
};

static struct polaris_smmu_table polaris_smmu_tables[POLARIS_SMMU_MAX_TABLES];
static uint32_t polaris_smmu_table_count;
static vm_offset_t polaris_smmu_table_chunk;
static uint32_t polaris_smmu_chunk_slots;
static struct polaris_smmu_address_space polaris_smmu_spaces[POLARIS_SMMU_ADDRESS_SPACE_COUNT] = {
	[POLARIS_SMMU_SPACE_GPU] = {
		.name = "3D core",
		.pte_attributes = POLARIS_SMMU_PTE_BASE | POLARIS_SMMU_PTE_UNPRIVILEGED,
		.cached_l1_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.cached_l2_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.context_bank = UINT32_MAX,
		.asid = 1,
	},
	[POLARIS_SMMU_SPACE_GMU_KERNEL] = {
		.name = "GMU kernel",
		.pte_attributes = POLARIS_SMMU_PTE_BASE,
		.cached_l1_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.cached_l2_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.context_bank = UINT32_MAX,
		.asid = 2,
	},
	[POLARIS_SMMU_SPACE_GMU_USER] = {
		.name = "GMU user",
		.pte_attributes = POLARIS_SMMU_PTE_BASE | POLARIS_SMMU_PTE_UNPRIVILEGED,
		.cached_l1_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.cached_l2_index = POLARIS_SMMU_CACHE_INDEX_NONE,
		.context_bank = UINT32_MAX,
		.asid = 3,
	},
};
static uint32_t polaris_smmu_oas_bits;
static uint32_t polaris_smmu_oas_encoding;
static bool polaris_smmu_supports_4k;
static bool polaris_smmu_coherent_walk;
static bool polaris_smmu_client_enabled;
static uint32_t polaris_smmu_context_page_size;
static uint32_t polaris_smmu_context_base;
static uint32_t polaris_smmu_context_bank_count;
static bool polaris_smmu_context_snapshot_valid;
static bool polaris_smmu_route_scan_complete;
static bool polaris_smmu_dynamic_routes_safe;
static bool polaris_smmu_context_blank[POLARIS_SMMU_MAX_CONTEXT_BANKS];
static bool polaris_smmu_context_routed[POLARIS_SMMU_MAX_CONTEXT_BANKS];
static uint32_t polaris_smmu_free_smr_slots[POLARIS_SMMU_MAX_CONTEXT_BANKS];
static uint32_t polaris_smmu_free_smr_count;
static struct polaris_smmu_route_candidate polaris_smmu_gpu_route;
static struct polaris_smmu_route_candidate polaris_smmu_gmu_kernel_route;
static struct polaris_smmu_route_candidate polaris_smmu_gmu_user_route;
static uint32_t polaris_smmu_stream_ids[POLARIS_SMMU_MAX_STREAM_IDS];
static uint32_t polaris_smmu_stream_count;
struct polaris_smmu_stream_route {
	uint32_t stream_id;
	uint32_t slot;
	uint32_t smr;
	uint32_t s2cr;
	uint32_t matches;
};
static struct polaris_smmu_stream_route polaris_smmu_stream_routes[POLARIS_SMMU_MAX_STREAM_IDS];
struct polaris_zap_segment {
	uint32_t paddr;
	uint32_t file_size;
	uint32_t memory_size;
	uint32_t flags;
	const uint8_t *data;
};
struct polaris_cmd_db_resource {
	uint32_t address;
	uint16_t levels[POLARIS_CMD_DB_MAX_LEVELS];
	uint32_t level_count;
};
struct polaris_cmd_db_entry {
	uint32_t address;
	const uint8_t *auxiliary;
	uint32_t auxiliary_length;
};
struct polaris_cmd_db_bcm {
	uint32_t address;
	uint32_t unit_size;
	uint16_t width;
	uint8_t clock_domain;
};
struct polaris_gpu_operating_point {
	uint32_t frequency_hz;
	uint32_t bus_frequency;
	uint32_t bus_minimum;
	uint32_t bus_maximum;
};
struct polaris_msm_bus_vector {
	uint32_t source;
	uint32_t destination;
	uint32_t average_kbps;
	uint32_t instantaneous_kbps;
};
static struct polaris_zap_segment polaris_zap_segments[A630_MDT_MAX_SEGMENTS];
static uint32_t polaris_zap_segment_count;
static const uint8_t *polaris_zap_mdt;
static const uint8_t *polaris_zap_hash;
static uint64_t polaris_zap_mdt_size;
static uint32_t polaris_zap_header_size;
static uint32_t polaris_zap_hash_size;
static uint32_t polaris_zap_min_address;
static uint32_t polaris_zap_memory_setup_size;
static uint64_t polaris_zap_memory_phys;
static uint64_t polaris_zap_memory_size;
static vm_offset_t polaris_zap_memory_virtual;
static vm_offset_t polaris_pas_metadata_virtual;
static vm_size_t polaris_pas_metadata_allocation;
static uint64_t polaris_pas_metadata_physical;
static bool polaris_zap_manifest_valid;
static bool polaris_pas_calls_ready;
static bool polaris_pas_gpu_supported;
static bool polaris_pas_image_active;
static bool polaris_zap_authenticated;
static uint64_t polaris_pas_scm_width;
static uint32_t polaris_rpmh_drv_id;
static uint32_t polaris_rpmh_drv_base;
static uint32_t polaris_rpmh_drv_size;
static uint32_t polaris_rpmh_tcs_base;
static uint32_t polaris_rpmh_tcs_size;
static uint32_t polaris_rpmh_tcs_offset;
static uint32_t polaris_rpmh_tcs_counts[4];
static uint32_t polaris_rpmh_total_tcs;
static const uint8_t *polaris_cmd_db;
static struct polaris_gpu_operating_point polaris_gpu_pwrlevels[POLARIS_MAX_GPU_PWRLEVELS];
static uint32_t polaris_gpu_pwrlevel_count;
static uint32_t polaris_gpu_initial_pwrlevel;
static struct polaris_msm_bus_vector polaris_gpu_bus_levels[POLARIS_MAX_GPU_BUS_LEVELS];
static uint32_t polaris_gpu_bus_level_count;
static struct polaris_msm_bus_vector polaris_gmu_cnoc_levels[POLARIS_EXPECTED_GMU_BUS_LEVELS];
static uint32_t polaris_gmu_cnoc_level_count;
static uint32_t polaris_gmu_pwrlevels[POLARIS_MAX_GMU_PWRLEVELS];
static uint32_t polaris_gmu_pwrlevel_count;
static struct polaris_a630_hfi_state polaris_hfi_state;
static vm_offset_t polaris_hfi_allocation;
static vm_size_t polaris_hfi_allocation_size;
static vm_offset_t polaris_hfi_uncached;
static uint64_t polaris_hfi_physical;
static bool polaris_hfi_space_mapped;
static bool polaris_gmu_firmware_loaded;
static bool polaris_gmu_firmware_started;
static bool polaris_gmu_hfi_ready;
static bool polaris_gmu_hfi_sealed;
static bool polaris_gmu_hfi_perf_table_ready;
static bool polaris_gmu_hfi_bw_table_ready;
static bool polaris_gmu_temp_bus_vote_active;
static bool polaris_gmu_boot_rail_active;
static bool polaris_gpu_oob_active;
static bool polaris_gmu_domain_ready;
static bool polaris_gpu_core_ready;
static bool polaris_gpu_core_configured;
static bool polaris_gpu_cp_started;
static bool polaris_gpu_cp_initialized;
static bool polaris_gpu_unsecured_mode;
static bool polaris_gpu_fence_smoke_test_done;
static bool polaris_gpu_ring_submit_ready;
static uint32_t polaris_gpu_ring_write_pointer;
static uint32_t polaris_gpu_last_fence;
static bool polaris_smmu_backend_enabled;
static bool polaris_smmu_backend_faulted;
static bool polaris_sqe_safe;
static const uint8_t *polaris_sqe_image;
static uint64_t polaris_sqe_image_size;
static struct polaris_gpu_dma_buffer polaris_gpu_sqe_buffer;
static struct polaris_gpu_dma_buffer polaris_gpu_ring_buffer;
static struct polaris_gpu_dma_buffer polaris_gpu_rptr_buffer;

static uint32_t
polaris_pm4_odd_parity(uint32_t value)
{
	uint32_t folded = value ^ (value >> 4) ^ (value >> 8) ^ (value >> 12) ^
	    (value >> 16) ^ (value >> 20) ^ (value >> 24) ^ (value >> 28);

	return (0x9669u >> (folded & 0xfu)) & 1u;
}

static uint32_t
polaris_a630_type7_packet(uint32_t opcode, uint32_t count)
{
	if (opcode > 0x7fu || count > 0x3fffu) {
		return 0;
	}
	return 0x70000000u | count | (polaris_pm4_odd_parity(count) << 15) |
	    (opcode << 16) | (polaris_pm4_odd_parity(opcode) << 23);
}

static bool
polaris_a630_cp_bootstrap_packets(uint32_t *cp_init, uint32_t cp_init_capacity,
	uint32_t *secure_mode, uint32_t secure_mode_capacity)
{
	uint32_t offset = 0;

	if (cp_init == NULL || cp_init_capacity < A630_CP_INIT_PACKET_DWORDS ||
	    secure_mode == NULL || secure_mode_capacity < A630_CP_SECURE_MODE_DWORDS) {
		return false;
	}
	cp_init[offset++] = polaris_a630_type7_packet(A630_CP_ME_INIT, A630_CP_INIT_ORDINALS);
	cp_init[offset++] = A630_CP_INIT_MASK;
	cp_init[offset++] = 0x00000003u; /* Allow the 3D and 2D hardware contexts. */
	cp_init[offset++] = 0x20000000u; /* Enable CP register protection/error detection. */
	cp_init[offset++] = 0u; /* Reserved header-dump address. */
	cp_init[offset++] = 0u; /* Header dump disabled. */
	cp_init[offset++] = 0u; /* No SQE workaround mask. */
	cp_init[offset++] = 0x00000002u; /* A6xx default operation-mode ordinal. */
	cp_init[offset++] = 0u; /* No power-up register list. */
	cp_init[offset++] = 0u;
	cp_init[offset++] = 0u; /* Register-list size, in dwords. */
	cp_init[offset++] = 0u; /* Pad to A630's fixed 11-ordinal packet. */
	if (offset != A630_CP_INIT_PACKET_DWORDS || cp_init[0] == 0) {
		return false;
	}
	secure_mode[0] = polaris_a630_type7_packet(A630_CP_SET_SECURE_MODE, 1u);
	secure_mode[1] = 0u; /* Leave secure mode after PAS authenticates ZAP. */
	return secure_mode[0] != 0;
}

static bool
prepare_a630_cp_bootstrap_packets(void)
{
	uint32_t cp_init[A630_CP_INIT_PACKET_DWORDS];
	uint32_t secure_mode[A630_CP_SECURE_MODE_DWORDS];

	printf("adreno-sdm845: step 15: build A630 CP_ME_INIT and CP_SET_SECURE_MODE payloads in local memory only\n");
	if (!polaris_sqe_safe || !polaris_a630_cp_bootstrap_packets(cp_init,
	    sizeof(cp_init) / sizeof(cp_init[0]), secure_mode,
	    sizeof(secure_mode) / sizeof(secure_mode[0]))) {
		printf("adreno-sdm845: step 15 requires the validated A630 SQE image and valid PM4 packet buffers\n");
		return false;
	}
	printf("adreno-sdm845: step 15 CP_ME_INIT[%u dwords]:", A630_CP_INIT_PACKET_DWORDS);
	for (uint32_t i = 0; i < A630_CP_INIT_PACKET_DWORDS; i++) {
		printf(" %08x", cp_init[i]);
	}
	printf("; CP_SET_SECURE_MODE[%u dwords]: %08x %08x; no GPU/SMMU/PAS register or firmware memory was written\n",
	    A630_CP_SECURE_MODE_DWORDS, secure_mode[0], secure_mode[1]);
	return true;
}

static void
polaris_gpu_va_init(void)
{
	lck_mtx_lock(&polaris_gpu_va_lock);
	if (!polaris_gpu_va_initialized) {
		polaris_gpu_va_free[0].start = POLARIS_GPU_VA_START;
		polaris_gpu_va_free[0].size = POLARIS_GPU_VA_END - POLARIS_GPU_VA_START;
		polaris_gpu_va_free_count = 1;
		polaris_gpu_va_initialized = true;
	}
	lck_mtx_unlock(&polaris_gpu_va_lock);
}

static bool
polaris_gpu_va_round(uint64_t size, uint64_t *rounded)
{
	if (size == 0 || size > UINT64_MAX - (POLARIS_GPU_VA_ALIGN - 1)) {
		return false;
	}
	size = (size + POLARIS_GPU_VA_ALIGN - 1) & ~(POLARIS_GPU_VA_ALIGN - 1);
	if (size == 0 || size > POLARIS_GPU_VA_END - POLARIS_GPU_VA_START) {
		return false;
	}
	*rounded = size;
	return true;
}

static uint64_t
sdm845_gpu_address(uint64_t size)
{
	uint64_t rounded;
	uint64_t address = 0;

	if (!polaris_gpu_va_round(size, &rounded)) {
		return 0;
	}
	lck_mtx_lock(&polaris_gpu_va_lock);
	if (!polaris_gpu_va_initialized || polaris_gpu_va_allocated_count >= POLARIS_GPU_VA_MAX_ALLOCS) {
		goto out;
	}
	for (uint32_t i = 0; i < polaris_gpu_va_free_count; i++) {
		if (polaris_gpu_va_free[i].size < rounded) {
			continue;
		}
		address = polaris_gpu_va_free[i].start;
		polaris_gpu_va_free[i].start += rounded;
		polaris_gpu_va_free[i].size -= rounded;
		if (polaris_gpu_va_free[i].size == 0) {
			polaris_gpu_va_free_count--;
			memmove(&polaris_gpu_va_free[i], &polaris_gpu_va_free[i + 1],
			    (polaris_gpu_va_free_count - i) * sizeof(polaris_gpu_va_free[0]));
		}
		polaris_gpu_va_allocated[polaris_gpu_va_allocated_count].start = address;
		polaris_gpu_va_allocated[polaris_gpu_va_allocated_count].size = rounded;
		polaris_gpu_va_allocated_count++;
		break;
	}
out:
	lck_mtx_unlock(&polaris_gpu_va_lock);
	return address;
}

static void
sdm845_gpu_address_free(uint64_t address, uint64_t size)
{
	uint64_t rounded, end;
	uint32_t allocation;
	uint32_t at;
	bool joins_left, joins_right;

	if (!polaris_gpu_va_round(size, &rounded) || (address & (POLARIS_GPU_VA_ALIGN - 1)) != 0 ||
	    address < POLARIS_GPU_VA_START || address >= POLARIS_GPU_VA_END || rounded > POLARIS_GPU_VA_END - address) {
		return;
	}
	end = address + rounded;
	lck_mtx_lock(&polaris_gpu_va_lock);
	allocation = polaris_gpu_va_allocated_count;
	for (uint32_t i = 0; i < polaris_gpu_va_allocated_count; i++) {
		if (polaris_gpu_va_allocated[i].start == address && polaris_gpu_va_allocated[i].size == rounded) {
			allocation = i;
			break;
		}
	}
	if (allocation == polaris_gpu_va_allocated_count) {
		goto out;
	}
	for (at = 0; at < polaris_gpu_va_free_count && polaris_gpu_va_free[at].start < address; at++) {
	}
	if ((at > 0 && polaris_gpu_va_free[at - 1].start + polaris_gpu_va_free[at - 1].size > address) ||
	    (at < polaris_gpu_va_free_count && end > polaris_gpu_va_free[at].start)) {
		goto out;
	}
	joins_left = at > 0 && polaris_gpu_va_free[at - 1].start + polaris_gpu_va_free[at - 1].size == address;
	joins_right = at < polaris_gpu_va_free_count && end == polaris_gpu_va_free[at].start;
	if (!joins_left && !joins_right && polaris_gpu_va_free_count == POLARIS_GPU_VA_MAX_RANGES) {
		goto out;
	}
	if (joins_left) {
		polaris_gpu_va_free[at - 1].size += rounded;
		if (joins_right) {
			polaris_gpu_va_free[at - 1].size += polaris_gpu_va_free[at].size;
			polaris_gpu_va_free_count--;
			memmove(&polaris_gpu_va_free[at], &polaris_gpu_va_free[at + 1],
			    (polaris_gpu_va_free_count - at) * sizeof(polaris_gpu_va_free[0]));
		}
	} else if (joins_right) {
		polaris_gpu_va_free[at].start = address;
		polaris_gpu_va_free[at].size += rounded;
	} else {
		memmove(&polaris_gpu_va_free[at + 1], &polaris_gpu_va_free[at],
		    (polaris_gpu_va_free_count - at) * sizeof(polaris_gpu_va_free[0]));
		polaris_gpu_va_free[at].start = address;
		polaris_gpu_va_free[at].size = rounded;
		polaris_gpu_va_free_count++;
	}
	polaris_gpu_va_allocated_count--;
	memmove(&polaris_gpu_va_allocated[allocation], &polaris_gpu_va_allocated[allocation + 1],
	    (polaris_gpu_va_allocated_count - allocation) * sizeof(polaris_gpu_va_allocated[0]));
out:
	lck_mtx_unlock(&polaris_gpu_va_lock);
}

/* Backend-only firmware/ring IOVAs live below the client aperture and are freed in reverse order. */
static uint64_t
polaris_gpu_private_address_allocate(uint64_t size)
{
	uint64_t rounded, address = 0;

	if (!polaris_gpu_va_round(size, &rounded)) {
		return 0;
	}
	lck_mtx_lock(&polaris_gpu_va_lock);
	if (!polaris_gpu_va_initialized ||
	    polaris_gpu_private_va_allocated_count >= POLARIS_GPU_PRIVATE_VA_MAX_ALLOCS ||
	    polaris_gpu_private_va_next < POLARIS_GPU_PRIVATE_VA_START ||
	    polaris_gpu_private_va_next > POLARIS_GPU_PRIVATE_VA_END ||
	    rounded > POLARIS_GPU_PRIVATE_VA_END - polaris_gpu_private_va_next) {
		goto out;
	}
	address = polaris_gpu_private_va_next;
	polaris_gpu_private_va_allocated[polaris_gpu_private_va_allocated_count].start = address;
	polaris_gpu_private_va_allocated[polaris_gpu_private_va_allocated_count].size = rounded;
	polaris_gpu_private_va_allocated_count++;
	polaris_gpu_private_va_next += rounded;
out:
	lck_mtx_unlock(&polaris_gpu_va_lock);
	return address;
}

static void
polaris_gpu_private_address_free(uint64_t address, uint64_t size)
{
	uint64_t rounded;
	uint32_t allocation;

	if (!polaris_gpu_va_round(size, &rounded) || (address & (POLARIS_GPU_VA_ALIGN - 1)) != 0 ||
	    address < POLARIS_GPU_PRIVATE_VA_START || address >= POLARIS_GPU_PRIVATE_VA_END ||
	    rounded > POLARIS_GPU_PRIVATE_VA_END - address) {
		return;
	}
	lck_mtx_lock(&polaris_gpu_va_lock);
	allocation = polaris_gpu_private_va_allocated_count;
	for (uint32_t i = 0; i < polaris_gpu_private_va_allocated_count; i++) {
		if (polaris_gpu_private_va_allocated[i].start == address &&
		    polaris_gpu_private_va_allocated[i].size == rounded) {
			allocation = i;
			break;
		}
	}
	if (allocation == polaris_gpu_private_va_allocated_count) {
		goto out;
	}
	polaris_gpu_private_va_allocated[allocation].size = 0;
	while (polaris_gpu_private_va_allocated_count != 0 &&
	    polaris_gpu_private_va_allocated[polaris_gpu_private_va_allocated_count - 1].size == 0) {
		polaris_gpu_private_va_next = polaris_gpu_private_va_allocated[--polaris_gpu_private_va_allocated_count].start;
	}
out:
	lck_mtx_unlock(&polaris_gpu_va_lock);
}

struct probe_block {
	const char *name;
	vm_offset_t physical;
	vm_size_t size;
	vm_offset_t virtual;
};

static struct probe_block gpucc = { "gpucc", POLARIS_GPUCC_PHYS, POLARIS_GPUCC_SIZE, 0 };
static struct probe_block gmu = { "gmu", POLARIS_GMU_PHYS, POLARIS_GMU_SIZE, 0 };
static struct probe_block smmu = { "kgsl-smmu-v2", POLARIS_SMMU_PHYS, POLARIS_SMMU_SIZE, 0 };
static struct probe_block gpu = { "3d-core", POLARIS_GPU_PHYS, POLARIS_GPU_SIZE, 0 };
static struct probe_block gcc_gpu_clocks = { "gcc-gpu-clocks", POLARIS_GCC_GPU_CLOCKS_PHYS,
	POLARIS_GCC_GPU_CLOCKS_SIZE, 0 };
static struct probe_block polaris_rpmh_drv = { "apps-rpmh-drv2", 0, 0, 0 };

static bool sdm845_gpu_map(uint64_t address, uint64_t phys, bool read_only);
static void sdm845_gpu_mapped(void);
static void sdm845_gpu_unmap(uint64_t address, uint64_t size);
static void sdm845_gpu_to_memory(void *start, uint64_t size);
static uint32_t sdm845_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
static bool sdm845_gpu_done(uint32_t fence);
static uint32_t sdm845_gpu_last(void);
static void sdm845_gpu_report(const char *why);
static void sdm845_gpu_stop(void);
static uint32_t smmu_id_size_to_bits(uint32_t encoding);

static bool
sdm845_gpu_ready(void)
{
	return polaris_smmu_backend_enabled && !polaris_smmu_backend_faulted && polaris_sqe_safe &&
	    polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active && polaris_gmu_hfi_ready &&
	    polaris_gmu_hfi_sealed && polaris_gpu_oob_active && polaris_gpu_core_ready &&
	    polaris_gpu_core_configured && polaris_zap_authenticated && polaris_pas_image_active &&
	    polaris_gpu_cp_started && polaris_gpu_cp_initialized && polaris_gpu_unsecured_mode &&
	    polaris_gpu_fence_smoke_test_done && polaris_gpu_ring_submit_ready;
}

const struct hakios_gpu_backend_ops hakios_gpu_sdm845_backend_ops = {
	.ready = sdm845_gpu_ready,
	.parameter = hakios_gpu_profile_parameter,
	.address = sdm845_gpu_address,
	.address_free = sdm845_gpu_address_free,
	.map = sdm845_gpu_map,
	.mapped = sdm845_gpu_mapped,
	.unmap = sdm845_gpu_unmap,
	.to_memory = sdm845_gpu_to_memory,
	.submit = sdm845_gpu_submit,
	.done = sdm845_gpu_done,
	.last = sdm845_gpu_last,
	.report = sdm845_gpu_report,
	.stop = sdm845_gpu_stop,
	/* SMMU translations are software-only until a verified context and stream are attached. */
};

static bool
map_block(struct probe_block *block)
{
	if (block->virtual == 0) {
		block->virtual = ml_io_map(block->physical, block->size);
	}
	if (block->virtual == 0) {
		printf("adreno-sdm845: cannot map %s at %#llx (%#llx bytes)\n", block->name,
		    (uint64_t)block->physical, (uint64_t)block->size);
		return false;
	}
	return true;
}

/* Flattened device-tree cells are big-endian, including on the little-endian SDM845 CPU. */
static uint32_t
polaris_dt_be32(const uint8_t *data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static bool
polaris_dt_has_compatible(DTEntry node, const char *expected)
{
	const uint8_t *strings;
	unsigned int size = 0, offset = 0;
	size_t expected_size = strlen(expected);

	if (SecureDTGetProperty(node, "compatible", (const void **)&strings, &size) != kSuccess || strings == NULL) {
		return false;
	}
	while (offset < size) {
		const uint8_t *terminator = memchr(strings + offset, '\0', size - offset);
		unsigned int string_size;

		if (terminator == NULL) {
			return false;
		}
		string_size = (unsigned int)(terminator - (strings + offset));
		if (string_size == expected_size && memcmp(strings + offset, expected, expected_size) == 0) {
			return true;
		}
		offset += string_size + 1;
	}
	return false;
}

static bool
polaris_dt_has_reg(DTEntry node, uint32_t address, uint32_t length)
{
	const uint8_t *reg;
	unsigned int size = 0;

	if (SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess || reg == NULL || size == 0 ||
	    (size & 7u) != 0) {
		return false;
	}
	for (unsigned int offset = 0; offset < size; offset += 8) {
		if (polaris_dt_be32(reg + offset) == address && polaris_dt_be32(reg + offset + 4) == length) {
			return true;
		}
	}
	return false;
}

static bool
polaris_dt_get_u32(DTEntry node, const char *name, uint32_t *value)
{
	const uint8_t *property;
	unsigned int size = 0;

	if (SecureDTGetProperty(node, name, (const void **)&property, &size) != kSuccess || property == NULL || size != 4) {
		return false;
	}
	*value = polaris_dt_be32(property);
	return true;
}

static bool
polaris_dt_string_equals(DTEntry node, const char *name, const char *expected)
{
	const uint8_t *property;
	unsigned int size = 0;
	size_t expected_size = strlen(expected) + 1;

	return SecureDTGetProperty(node, name, (const void **)&property, &size) == kSuccess && property != NULL &&
	    size == expected_size && memcmp(property, expected, expected_size) == 0;
}

static bool
polaris_dt_string_list_equals(DTEntry node, const char *name, const char *const *expected, uint32_t expected_count)
{
	const uint8_t *property;
	unsigned int size = 0, offset = 0;

	if (SecureDTGetProperty(node, name, (const void **)&property, &size) != kSuccess || property == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < expected_count; i++) {
		size_t length = strlen(expected[i]) + 1;

		if (offset > size || length > size - offset || memcmp(property + offset, expected[i], length - 1) != 0 ||
		    property[offset + length - 1] != '\0') {
			return false;
		}
		offset += (unsigned int)length;
	}
	return offset == size;
}

struct polaris_dt_clock_spec {
	const char *provider_path;
	uint32_t clock_id;
};

static bool
polaris_dt_clocks_match(DTEntry node, const char *name, const struct polaris_dt_clock_spec *expected,
	uint32_t expected_count)
{
	const uint8_t *property;
	unsigned int size = 0;

	if (SecureDTGetProperty(node, name, (const void **)&property, &size) != kSuccess || property == NULL ||
	    size != expected_count * 8u) {
		return false;
	}
	for (uint32_t i = 0; i < expected_count; i++) {
		DTEntry provider = NULL;
		uint32_t phandle;
		uint32_t actual_phandle = polaris_dt_be32(property + i * 8u);
		uint32_t actual_id = polaris_dt_be32(property + i * 8u + 4u);

		if (SecureDTLookupEntry(NULL, expected[i].provider_path, &provider) != kSuccess || provider == NULL ||
		    !polaris_dt_get_u32(provider, "phandle", &phandle) || actual_phandle != phandle ||
		    actual_id != expected[i].clock_id) {
			printf("adreno-sdm845: runtime %s clock %u is provider=%#x id=%u; expected %s/%u\n", name, i,
			    actual_phandle, actual_id, expected[i].provider_path, expected[i].clock_id);
			return false;
		}
	}
	return true;
}

static bool
polaris_runtime_pwrlevels_validate(void)
{
	DTEntry gpu_node = NULL, gpu_table = NULL, gmu_table = NULL;
	char path[160];
	uint32_t gpu_count = 0, gmu_count = 0, initial_gpu_pwrlevel, previous_gpu_frequency = UINT32_MAX;
	bool gpu_idle_seen = false, gmu_nonzero_seen = false;

	if (SecureDTLookupEntry(NULL, POLARIS_GPU_DT_PATH, &gpu_node) != kSuccess || gpu_node == NULL ||
	    !polaris_dt_get_u32(gpu_node, "qcom,initial-pwrlevel", &initial_gpu_pwrlevel) ||
	    SecureDTLookupEntry(NULL, POLARIS_GPU_PWRLEVELS_DT_PATH, &gpu_table) != kSuccess || gpu_table == NULL ||
	    !polaris_dt_has_compatible(gpu_table, "qcom,gpu-pwrlevels") ||
	    SecureDTLookupEntry(NULL, POLARIS_GMU_PWRLEVELS_DT_PATH, &gmu_table) != kSuccess || gmu_table == NULL ||
	    !polaris_dt_has_compatible(gmu_table, "qcom,gmu-pwrlevels")) {
		printf("adreno-sdm845: runtime GPU default powerlevel or GPU/GMU operating-point tables are missing or invalid\n");
		return false;
	}

	for (uint32_t i = 0; i < POLARIS_MAX_GPU_PWRLEVELS; i++) {
		DTEntry level = NULL;
		uint32_t index, frequency, bus, bus_min, bus_max;

		snprintf(path, sizeof(path), "%s/qcom,gpu-pwrlevel@%u", POLARIS_GPU_PWRLEVELS_DT_PATH, i);
		if (SecureDTLookupEntry(NULL, path, &level) != kSuccess || level == NULL) {
			break;
		}
		if (!polaris_dt_get_u32(level, "reg", &index) || index != i ||
		    !polaris_dt_get_u32(level, "qcom,gpu-freq", &frequency) || frequency > POLARIS_MAX_CLOCK_RATE_HZ ||
		    !polaris_dt_get_u32(level, "qcom,bus-freq", &bus) ||
		    !polaris_dt_get_u32(level, "qcom,bus-min", &bus_min) ||
		    !polaris_dt_get_u32(level, "qcom,bus-max", &bus_max)) {
			printf("adreno-sdm845: Polaris GPU operating point %u is incomplete or outside supported bounds\n", i);
			return false;
		}
		if (frequency == 0) {
			if (gpu_count == 0 || gpu_idle_seen || bus != 0 || bus_min != 0 || bus_max != 0) {
				printf("adreno-sdm845: Polaris GPU idle operating point %u is malformed\n", i);
				return false;
			}
			gpu_idle_seen = true;
		} else if (gpu_idle_seen || frequency >= previous_gpu_frequency || bus == 0 || bus_max > 31 ||
		    bus_min > bus || bus > bus_max) {
			printf("adreno-sdm845: Polaris GPU operating point %u has invalid frequency or bus ordering\n", i);
			return false;
		}
		polaris_gpu_pwrlevels[i] = (struct polaris_gpu_operating_point){ frequency, bus, bus_min, bus_max };
		if (frequency != 0) {
			previous_gpu_frequency = frequency;
		}
		gpu_count++;
	}
	if (gpu_count < 2 || !gpu_idle_seen || gpu_count == POLARIS_MAX_GPU_PWRLEVELS ||
	    polaris_gpu_pwrlevels[gpu_count - 1].frequency_hz != 0) {
		printf("adreno-sdm845: Polaris GPU operating-point table lacks a bounded descending range and terminal idle entry\n");
		return false;
	}
	if (initial_gpu_pwrlevel >= gpu_count - 1u) {
		printf("adreno-sdm845: Polaris initial GPU powerlevel %u is outside the %u non-idle operating points\n",
		    initial_gpu_pwrlevel, gpu_count - 1u);
		return false;
	}
	for (uint32_t i = gpu_count; i < POLARIS_MAX_GPU_PWRLEVELS; i++) {
		DTEntry level = NULL;

		snprintf(path, sizeof(path), "%s/qcom,gpu-pwrlevel@%u", POLARIS_GPU_PWRLEVELS_DT_PATH, i);
		if (SecureDTLookupEntry(NULL, path, &level) == kSuccess && level != NULL) {
			printf("adreno-sdm845: Polaris GPU operating-point table has a gap before entry %u\n", i);
			return false;
		}
	}
	polaris_gpu_pwrlevel_count = gpu_count;
	polaris_gpu_initial_pwrlevel = initial_gpu_pwrlevel;

	for (uint32_t i = 0; i < POLARIS_MAX_GMU_PWRLEVELS; i++) {
		DTEntry level = NULL;
		uint32_t index, frequency;

		snprintf(path, sizeof(path), "%s/qcom,gmu-pwrlevel@%u", POLARIS_GMU_PWRLEVELS_DT_PATH, i);
		if (SecureDTLookupEntry(NULL, path, &level) != kSuccess || level == NULL) {
			break;
		}
		if (!polaris_dt_get_u32(level, "reg", &index) || index != i ||
		    !polaris_dt_get_u32(level, "qcom,gmu-freq", &frequency) || frequency > POLARIS_MAX_CLOCK_RATE_HZ) {
			printf("adreno-sdm845: Polaris GMU operating point %u is incomplete or outside supported bounds\n", i);
			return false;
		}
		for (uint32_t prior = 0; prior < gmu_count; prior++) {
			if (frequency != 0 && polaris_gmu_pwrlevels[prior] == frequency) {
				printf("adreno-sdm845: Polaris GMU operating-point frequency %u is duplicated\n", frequency);
				return false;
			}
		}
		polaris_gmu_pwrlevels[i] = frequency;
		gmu_nonzero_seen |= frequency != 0;
		gmu_count++;
	}
	if (gmu_count < 2 || !gmu_nonzero_seen || gmu_count == POLARIS_MAX_GMU_PWRLEVELS) {
		printf("adreno-sdm845: Polaris GMU operating-point table is incomplete or unbounded\n");
		return false;
	}
	for (uint32_t i = gmu_count; i < POLARIS_MAX_GMU_PWRLEVELS; i++) {
		DTEntry level = NULL;

		snprintf(path, sizeof(path), "%s/qcom,gmu-pwrlevel@%u", POLARIS_GMU_PWRLEVELS_DT_PATH, i);
		if (SecureDTLookupEntry(NULL, path, &level) == kSuccess && level != NULL) {
			printf("adreno-sdm845: Polaris GMU operating-point table has a gap before entry %u\n", i);
			return false;
		}
	}
	polaris_gmu_pwrlevel_count = gmu_count;
	return true;
}

static bool
polaris_dt_bus_vectors_read(DTEntry node, const char *expected_name, uint32_t expected_cases,
	struct polaris_msm_bus_vector *vectors)
{
	const uint8_t *property;
	unsigned int size = 0;
	uint32_t cases, paths;

	if (node == NULL || expected_name == NULL || vectors == NULL || expected_cases == 0 ||
	    !polaris_dt_string_equals(node, "qcom,msm-bus,name", expected_name) ||
	    !polaris_dt_get_u32(node, "qcom,msm-bus,num-cases", &cases) || cases != expected_cases ||
	    !polaris_dt_get_u32(node, "qcom,msm-bus,num-paths", &paths) || paths != 1 ||
	    SecureDTGetProperty(node, "qcom,msm-bus,vectors-KBps", (const void **)&property, &size) != kSuccess ||
	    property == NULL || size != expected_cases * 4u * sizeof(uint32_t)) {
		printf("adreno-sdm845: runtime %s bus table is missing, malformed, or has an unexpected usecase layout\n",
		    expected_name != NULL ? expected_name : "unnamed");
		return false;
	}

	for (uint32_t i = 0; i < expected_cases; i++) {
		struct polaris_msm_bus_vector *vector = &vectors[i];
		const uint8_t *cells = property + i * 4u * sizeof(uint32_t);

		vector->source = polaris_dt_be32(cells);
		vector->destination = polaris_dt_be32(cells + sizeof(uint32_t));
		vector->average_kbps = polaris_dt_be32(cells + 2u * sizeof(uint32_t));
		vector->instantaneous_kbps = polaris_dt_be32(cells + 3u * sizeof(uint32_t));
		if (vector->source == 0 || vector->destination == 0 ||
		    (i == 0 && (vector->average_kbps != 0 || vector->instantaneous_kbps != 0)) ||
		    (i != 0 && (vector->source != vectors[0].source || vector->destination != vectors[0].destination ||
		    vector->average_kbps < vectors[i - 1].average_kbps ||
		    vector->instantaneous_kbps < vectors[i - 1].instantaneous_kbps))) {
			printf("adreno-sdm845: runtime %s bus usecase %u has invalid path IDs or bandwidth ordering\n",
			    expected_name, i);
			return false;
		}
	}
	return true;
}

static bool
polaris_runtime_bus_tables_validate(void)
{
	DTEntry gpu_node = NULL, gmu_node = NULL;

	if (SecureDTLookupEntry(NULL, POLARIS_GPU_DT_PATH, &gpu_node) != kSuccess || gpu_node == NULL ||
	    SecureDTLookupEntry(NULL, POLARIS_GMU_DT_PATH, &gmu_node) != kSuccess || gmu_node == NULL ||
	    !polaris_dt_bus_vectors_read(gpu_node, "grp3d", POLARIS_EXPECTED_GPU_BUS_LEVELS,
	    polaris_gpu_bus_levels) ||
	    !polaris_dt_bus_vectors_read(gmu_node, "cnoc", POLARIS_EXPECTED_GMU_BUS_LEVELS,
	    polaris_gmu_cnoc_levels)) {
		return false;
	}
	polaris_gpu_bus_level_count = POLARIS_EXPECTED_GPU_BUS_LEVELS;
	polaris_gmu_cnoc_level_count = POLARIS_EXPECTED_GMU_BUS_LEVELS;

	for (uint32_t i = 0; i < polaris_gpu_pwrlevel_count; i++) {
		const struct polaris_gpu_operating_point *level = &polaris_gpu_pwrlevels[i];

		if (level->bus_frequency >= polaris_gpu_bus_level_count || level->bus_minimum >= polaris_gpu_bus_level_count ||
		    level->bus_maximum >= polaris_gpu_bus_level_count) {
			printf("adreno-sdm845: GPU powerlevel %u references bus usecase outside the %u-entry grp3d table\n",
			    i, polaris_gpu_bus_level_count);
			return false;
		}
	}
	return true;
}

static bool
polaris_dt_has_reg64(DTEntry node, uint64_t address, uint64_t length)
{
	const uint8_t *reg;
	unsigned int size = 0;

	/* The Polaris reserved-memory parent uses two address cells and two size cells. */
	if (SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess || reg == NULL || size == 0 ||
	    (size & 15u) != 0) {
		return false;
	}
	for (unsigned int offset = 0; offset < size; offset += 16) {
		uint64_t start = (uint64_t)polaris_dt_be32(reg + offset) << 32 | polaris_dt_be32(reg + offset + 4);
		uint64_t bytes = (uint64_t)polaris_dt_be32(reg + offset + 8) << 32 | polaris_dt_be32(reg + offset + 12);

		if (start == address && bytes == length) {
			return true;
		}
	}
	return false;
}

static bool
polaris_dt_reg64_contains(DTEntry node, uint64_t address, uint64_t length)
{
	const uint8_t *reg;
	unsigned int size = 0;
	uint64_t end;

	if (length == 0 || address > UINT64_MAX - length ||
	    SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess || reg == NULL || size == 0 ||
	    (size & 15u) != 0) {
		return false;
	}
	end = address + length;
	for (unsigned int offset = 0; offset < size; offset += 16) {
		uint64_t start = (uint64_t)polaris_dt_be32(reg + offset) << 32 | polaris_dt_be32(reg + offset + 4);
		uint64_t bytes = (uint64_t)polaris_dt_be32(reg + offset + 8) << 32 | polaris_dt_be32(reg + offset + 12);

		if (bytes != 0 && start <= UINT64_MAX - bytes && address >= start && end <= start + bytes) {
			return true;
		}
	}
	return false;
}

static bool
polaris_dt_get_reg32(DTEntry node, uint32_t index, uint32_t *address, uint32_t *length)
{
	const uint8_t *reg;
	unsigned int size = 0;

	if (address == NULL || length == NULL ||
	    SecureDTGetProperty(node, "reg", (const void **)&reg, &size) != kSuccess || reg == NULL || size == 0 ||
	    (size & 7u) != 0 || index >= size / 8u) {
		return false;
	}
	*address = polaris_dt_be32(reg + index * 8u);
	*length = polaris_dt_be32(reg + index * 8u + 4u);
	return *length != 0;
}

static bool
polaris_dt_mboxes_match(const char *path, uint32_t provider_phandle)
{
	DTEntry node = NULL;
	const uint8_t *mboxes;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, path, &node) != kSuccess || node == NULL ||
	    SecureDTGetProperty(node, "mboxes", (const void **)&mboxes, &size) != kSuccess || mboxes == NULL ||
	    size != 8u) {
		return false;
	}
	return polaris_dt_be32(mboxes) == provider_phandle && polaris_dt_be32(mboxes + 4) == 0;
}

static bool
polaris_dt_has_empty_property(DTEntry node, const char *name)
{
	const void *property = NULL;
	unsigned int size = 0;

	return SecureDTGetProperty(node, name, &property, &size) == kSuccess && size == 0;
}

static bool
polaris_dt_has_property(DTEntry node, const char *name)
{
	const void *property = NULL;
	unsigned int size = 0;

	return SecureDTGetProperty(node, name, &property, &size) == kSuccess;
}

static bool
polaris_runtime_rpmh_validate(void)
{
	static const uint32_t expected_types[] = {
		POLARIS_RPMH_TCS_ACTIVE_TYPE, POLARIS_RPMH_TCS_SLEEP_TYPE,
		POLARIS_RPMH_TCS_WAKE_TYPE, POLARIS_RPMH_TCS_CONTROL_TYPE,
	};
	static const uint32_t expected_counts[] = { 2, 3, 3, 1 };
	DTEntry drv_node = NULL, cmd_db_node = NULL, reservation_node = NULL;
	DTEntry cx_rpmh_node = NULL, gfx_rpmh_node = NULL;
	const uint8_t *tcs_config;
	unsigned int tcs_config_size = 0;
	uint32_t drv_id, drv_phandle, drv_base, drv_size, tcs_base, tcs_size, total_tcs = 0;
	uint64_t tcs_end;

	if (SecureDTLookupEntry(NULL, POLARIS_RPMH_DRV_DT_PATH, &drv_node) != kSuccess || drv_node == NULL ||
	    !polaris_dt_has_compatible(drv_node, "qcom,tcs-drv") ||
	    !polaris_dt_string_equals(drv_node, "label", "apps_rsc") ||
	    !polaris_dt_get_u32(drv_node, "qcom,drv-id", &drv_id) || drv_id != 2 ||
	    !polaris_dt_get_u32(drv_node, "phandle", &drv_phandle) ||
	    !polaris_dt_get_reg32(drv_node, 0, &drv_base, &drv_size) ||
	    !polaris_dt_get_reg32(drv_node, 1, &tcs_base, &tcs_size) ||
	    SecureDTLookupEntry(NULL, "/soc/rpmh-regulator-cxlvl", &cx_rpmh_node) != kSuccess || cx_rpmh_node == NULL ||
	    SecureDTLookupEntry(NULL, "/soc/rpmh-regulator-gfxlvl", &gfx_rpmh_node) != kSuccess || gfx_rpmh_node == NULL ||
	    !polaris_dt_string_equals(cx_rpmh_node, "qcom,resource-name", "cx.lvl") ||
	    !polaris_dt_string_equals(gfx_rpmh_node, "qcom,resource-name", "gfx.lvl") ||
	    drv_base != 0x179e0000u || drv_size != 0x100u || tcs_base != 0x179e0d00u || tcs_size != 0x3000u ||
	    tcs_base <= drv_base || !polaris_dt_mboxes_match("/soc/rpmh-regulator-cxlvl", drv_phandle) ||
	    !polaris_dt_mboxes_match("/soc/rpmh-regulator-gfxlvl", drv_phandle) ||
	    SecureDTGetProperty(drv_node, "qcom,tcs-config", (const void **)&tcs_config, &tcs_config_size) != kSuccess ||
	    tcs_config == NULL || tcs_config_size != 4u * 8u ||
	    SecureDTLookupEntry(NULL, POLARIS_CMD_DB_DT_PATH, &cmd_db_node) != kSuccess || cmd_db_node == NULL ||
	    !polaris_dt_has_compatible(cmd_db_node, "qcom,cmd-db") ||
	    SecureDTLookupEntry(NULL, POLARIS_CMD_DB_RESERVATION_PATH, &reservation_node) != kSuccess ||
	    reservation_node == NULL || !polaris_dt_has_empty_property(reservation_node, "no-map") ||
	    !polaris_dt_reg64_contains(reservation_node, POLARIS_CMD_DB_PHYS, POLARIS_CMD_DB_SIZE)) {
		printf("adreno-sdm845: runtime RPMh DRV2 or AOP command-database reservation differs from Polaris; probe refused\n");
		return false;
	}

	for (uint32_t i = 0; i < 4; i++) {
		uint32_t type = polaris_dt_be32(tcs_config + i * 8u);
		uint32_t count = polaris_dt_be32(tcs_config + i * 8u + 4u);

		if (type != expected_types[i] || count != expected_counts[i]) {
			printf("adreno-sdm845: runtime RPMh TCS group %u is type=%u count=%u; expected type=%u count=%u\n",
			    i, type, count, expected_types[i], expected_counts[i]);
			return false;
		}
		polaris_rpmh_tcs_counts[type] = count;
		if (type != POLARIS_RPMH_TCS_CONTROL_TYPE) {
			total_tcs += count;
		}
	}
	tcs_end = (uint64_t)tcs_base + tcs_size;
	if (total_tcs == 0 || total_tcs > (tcs_size - 0x10u) / POLARIS_RPMH_TCS_STRIDE ||
	    tcs_end <= drv_base || tcs_end - drv_base > UINT32_MAX) {
		printf("adreno-sdm845: runtime RPMh TCS window is too small for its declared groups\n");
		return false;
	}

	polaris_rpmh_drv_id = drv_id;
	polaris_rpmh_drv_base = drv_base;
	polaris_rpmh_drv_size = drv_size;
	polaris_rpmh_tcs_base = tcs_base;
	polaris_rpmh_tcs_size = tcs_size;
	polaris_rpmh_tcs_offset = tcs_base - drv_base;
	polaris_rpmh_total_tcs = total_tcs;
	polaris_rpmh_drv.physical = drv_base;
	polaris_rpmh_drv.size = (vm_size_t)(tcs_end - drv_base);
	printf("adreno-sdm845: runtime DT identifies Apps RPMh DRV%u at %#x/%#x, TCS at %#x/%#x "
	    "(active/sleep/wake/control=%u/%u/%u/%u); AOP CMD_DB %#llx/%#llx is inside no-map reservation\n",
	    drv_id, drv_base, drv_size, tcs_base, tcs_size, polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_ACTIVE_TYPE],
	    polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_SLEEP_TYPE], polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_WAKE_TYPE],
	    polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_CONTROL_TYPE], (uint64_t)POLARIS_CMD_DB_PHYS,
	    (uint64_t)POLARIS_CMD_DB_SIZE);
	return true;
}

static bool
polaris_dt_iommu_streams_match(const char *path, uint32_t smmu_phandle, const uint32_t *expected_ids,
	uint32_t expected_count)
{
	DTEntry node = NULL;
	const uint8_t *iommus;
	unsigned int size = 0;

	if (SecureDTLookupEntry(NULL, path, &node) != kSuccess || node == NULL ||
	    SecureDTGetProperty(node, "iommus", (const void **)&iommus, &size) != kSuccess || iommus == NULL ||
	    size != expected_count * 8u) {
		printf("adreno-sdm845: runtime device tree has invalid iommus on %s\n", path);
		return false;
	}
	for (uint32_t i = 0; i < expected_count; i++) {
		uint32_t phandle = polaris_dt_be32(iommus + i * 8u);
		uint32_t stream_id = polaris_dt_be32(iommus + i * 8u + 4u);
		bool seen = false;

		if (phandle != smmu_phandle || stream_id != expected_ids[i]) {
			printf("adreno-sdm845: runtime iommus on %s entry %u is phandle=%#x SID=%u; expected %#x/%u\n",
			    path, i, phandle, stream_id, smmu_phandle, expected_ids[i]);
			return false;
		}
		for (uint32_t j = 0; j < polaris_smmu_stream_count; j++) {
			if (polaris_smmu_stream_ids[j] == stream_id) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			if (polaris_smmu_stream_count == POLARIS_SMMU_MAX_STREAM_IDS) {
				return false;
			}
			polaris_smmu_stream_ids[polaris_smmu_stream_count++] = stream_id;
		}
	}
	return true;
}

static bool
polaris_runtime_iommu_validate(DTEntry smmu_node, uint32_t smmu_phandle)
{
	static const uint32_t gfx3d_user_ids[] = { 0 };
	static const uint32_t gfx3d_secure_ids[] = { 2, 1 };
	static const uint32_t gmu_kernel_ids[] = { 5 };
	static const uint32_t gmu_user_ids[] = { 4 };
	DTEntry kgsl_iommu_node = NULL, gfx3d_user_node = NULL;
	const uint8_t *protect;
	unsigned int protect_size = 0;
	uint32_t gpu_offset;

	polaris_smmu_dynamic_routes_safe = false;
	if (polaris_dt_has_property(smmu_node, "qcom,enable-static-cb")) {
		printf("adreno-sdm845: runtime DT enables TrustZone-owned static SMMU routes; route planning and writes are refused\n");
		return false;
	}
	if (!polaris_dt_has_empty_property(smmu_node, "qcom,dynamic") ||
	    !polaris_dt_has_empty_property(smmu_node, "qcom,use-3-lvl-tables") ||
	    !polaris_dt_has_empty_property(smmu_node, "qcom,disable-atos") ||
	    SecureDTLookupEntry(NULL, "/soc/qcom,kgsl-iommu", &kgsl_iommu_node) != kSuccess || kgsl_iommu_node == NULL ||
	    !polaris_dt_has_compatible(kgsl_iommu_node, "qcom,kgsl-smmu-v2") ||
	    SecureDTGetProperty(kgsl_iommu_node, "qcom,protect", (const void **)&protect, &protect_size) != kSuccess ||
	    protect == NULL || protect_size != 8u || polaris_dt_be32(protect) != 0x40000u ||
	    polaris_dt_be32(protect + 4u) != 0xc000u ||
	    !polaris_dt_has_empty_property(kgsl_iommu_node, "qcom,hyp_secure_alloc") ||
	    SecureDTLookupEntry(NULL, "/soc/qcom,kgsl-iommu/gfx3d_user", &gfx3d_user_node) != kSuccess ||
	    gfx3d_user_node == NULL || !polaris_dt_get_u32(gfx3d_user_node, "qcom,gpu-offset", &gpu_offset) ||
	    gpu_offset != 0x48000u ||
	    !polaris_dt_iommu_streams_match("/soc/qcom,kgsl-iommu/gfx3d_user", smmu_phandle,
	    gfx3d_user_ids, sizeof(gfx3d_user_ids) / sizeof(gfx3d_user_ids[0])) ||
	    !polaris_dt_iommu_streams_match("/soc/qcom,kgsl-iommu/gfx3d_secure", smmu_phandle,
	    gfx3d_secure_ids, sizeof(gfx3d_secure_ids) / sizeof(gfx3d_secure_ids[0])) ||
	    !polaris_dt_iommu_streams_match("/soc/qcom,gmu/gmu_kernel", smmu_phandle,
	    gmu_kernel_ids, sizeof(gmu_kernel_ids) / sizeof(gmu_kernel_ids[0])) ||
	    !polaris_dt_iommu_streams_match("/soc/qcom,gmu/gmu_user", smmu_phandle,
	    gmu_user_ids, sizeof(gmu_user_ids) / sizeof(gmu_user_ids[0]))) {
		printf("adreno-sdm845: runtime SMMUv2 stream/context description is incomplete or differs from Polaris; probe refused\n");
		return false;
	}
	polaris_smmu_dynamic_routes_safe = true;
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		for (uint32_t j = i + 1; j < polaris_smmu_stream_count; j++) {
			if (polaris_smmu_stream_ids[j] < polaris_smmu_stream_ids[i]) {
				uint32_t temporary = polaris_smmu_stream_ids[i];
				polaris_smmu_stream_ids[i] = polaris_smmu_stream_ids[j];
				polaris_smmu_stream_ids[j] = temporary;
			}
		}
	}
	printf("adreno-sdm845: runtime DT KGSL/GMU stream IDs:");
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		printf(" %u", polaris_smmu_stream_ids[i]);
	}
	printf("; gfx3d_user GPU offset=%#x; three-level SMMU tables required; protect=<0x40000 0xc000>, HYP secure allocation and ATOS-disabled policy present, static-CB policy absent\n",
	    gpu_offset);
	return true;
}

static bool
polaris_runtime_pas_validate(void)
{
	DTEntry pil_node = NULL, memory_node = NULL;
	uint32_t pas_id, region_phandle, memory_phandle;
	const uint8_t *memory_region;
	unsigned int memory_region_size = 0;
	uint64_t memory_phys = 0x8c415000ull, memory_size = 0x2000ull;

	if (SecureDTLookupEntry(NULL, POLARIS_PIL_GPU_DT_PATH, &pil_node) != kSuccess || pil_node == NULL ||
	    !polaris_dt_has_compatible(pil_node, "qcom,pil-tz-generic") ||
	    !polaris_dt_get_u32(pil_node, "qcom,pas-id", &pas_id) || pas_id != POLARIS_PAS_GPU ||
	    !polaris_dt_string_equals(pil_node, "qcom,firmware-name", "a630_zap") ||
	    SecureDTGetProperty(pil_node, "memory-region", (const void **)&memory_region, &memory_region_size) != kSuccess ||
	    memory_region == NULL || memory_region_size != 4) {
		printf("adreno-sdm845: runtime DT has no valid A630 PAS 13 firmware/memory-region description\n");
		return false;
	}
	region_phandle = polaris_dt_be32(memory_region);
	if (SecureDTLookupEntry(NULL, POLARIS_GPU_MEMORY_DT_PATH, &memory_node) != kSuccess || memory_node == NULL ||
	    !polaris_dt_get_u32(memory_node, "phandle", &memory_phandle) || memory_phandle != region_phandle ||
	    !polaris_dt_has_empty_property(memory_node, "no-map") ||
	    !polaris_dt_has_reg64(memory_node, memory_phys, memory_size)) {
		printf("adreno-sdm845: runtime GPU PAS memory-region does not resolve to the expected no-map 8-KiB range\n");
		return false;
	}
	polaris_zap_memory_phys = memory_phys;
	polaris_zap_memory_size = memory_size;
	printf("adreno-sdm845: runtime DT PAS %u firmware a630_zap maps reserved GPU memory %#llx/%#llx bytes\n",
	    pas_id, memory_phys, memory_size);
	return true;
}

static bool
polaris_dt_node_matches(const char *path, const char *compatible, uint32_t address, uint32_t length)
{
	DTEntry node = NULL;

	if (SecureDTLookupEntry(NULL, path, &node) != kSuccess || node == NULL) {
		printf("adreno-sdm845: runtime device tree is missing %s\n", path);
		return false;
	}
	if (!polaris_dt_has_compatible(node, compatible)) {
		printf("adreno-sdm845: runtime device tree %s lacks compatible %s\n", path, compatible);
		return false;
	}
	if (!polaris_dt_has_reg(node, address, length)) {
		printf("adreno-sdm845: runtime device tree %s has no reg entry for %#x/%#x\n", path, address, length);
		return false;
	}
	return true;
}

static bool
polaris_runtime_power_validate(DTEntry gpu_node, DTEntry gmu_node)
{
	static const char *const gpu_clock_names[] = {
		"core_clk", "rbbmtimer_clk", "mem_clk", "mem_iface_clk", "gmu_clk", "l3_vote",
	};
	static const struct polaris_dt_clock_spec gpu_clocks[] = {
		{ POLARIS_GFXCC_DT_PATH, 3 },  /* GPU_CC_GX_GFX3D_CLK */
		{ POLARIS_GPUCC_DT_PATH, 10 }, /* GPU_CC_CXO_CLK */
		{ POLARIS_GCC_DT_PATH, 26 },   /* GCC_DDRSS_GPU_AXI_CLK */
		{ POLARIS_GCC_DT_PATH, 41 },   /* GCC_GPU_MEMNOC_GFX_CLK */
		{ POLARIS_GPUCC_DT_PATH, 4 },  /* GPU_CC_CX_GMU_CLK */
		{ POLARIS_CPUCC_DT_PATH, 16 }, /* L3_GPU_VOTE_CLK */
	};
	static const char *const gmu_clock_names[] = {
		"gmu_clk", "cxo_clk", "axi_clk", "memnoc_clk",
	};
	static const struct polaris_dt_clock_spec gmu_clocks[] = {
		{ POLARIS_GPUCC_DT_PATH, 4 },  /* GPU_CC_CX_GMU_CLK */
		{ POLARIS_GPUCC_DT_PATH, 10 }, /* GPU_CC_CXO_CLK */
		{ POLARIS_GCC_DT_PATH, 26 },   /* GCC_DDRSS_GPU_AXI_CLK */
		{ POLARIS_GCC_DT_PATH, 41 },   /* GCC_GPU_MEMNOC_GFX_CLK */
	};
	static const char *const regulator_names[] = { "vddcx", "vdd" };
	DTEntry gpucc_node = NULL, gfxcc_node = NULL;
	DTEntry cx_gdsc_node = NULL, gx_gdsc_node = NULL;
	uint32_t gpu_vddcx, gpu_vdd, gmu_vddcx, gmu_vdd;
	uint32_t gpucc_vddcx, gpucc_vddmx, gfxcc_vddgfx, cx_parent, gx_parent;
	uint32_t cx_gdsc_phandle, gx_gdsc_phandle, cx_clk_dis_wait;

	if (!polaris_dt_node_matches(POLARIS_GFXCC_DT_PATH, "qcom,gfxcc-sdm845-v2", POLARIS_GPUCC_PHYS,
	    POLARIS_GPUCC_SIZE) ||
	    !polaris_dt_node_matches(POLARIS_GPU_CX_GDSC_DT_PATH, "qcom,gdsc", 0x0509106cu, 4) ||
	    !polaris_dt_node_matches(POLARIS_GPU_GX_GDSC_DT_PATH, "qcom,gdsc", 0x0509100cu, 4) ||
	    SecureDTLookupEntry(NULL, POLARIS_GPUCC_DT_PATH, &gpucc_node) != kSuccess || gpucc_node == NULL ||
	    SecureDTLookupEntry(NULL, POLARIS_GFXCC_DT_PATH, &gfxcc_node) != kSuccess || gfxcc_node == NULL ||
	    SecureDTLookupEntry(NULL, POLARIS_GPU_CX_GDSC_DT_PATH, &cx_gdsc_node) != kSuccess || cx_gdsc_node == NULL ||
	    SecureDTLookupEntry(NULL, POLARIS_GPU_GX_GDSC_DT_PATH, &gx_gdsc_node) != kSuccess || gx_gdsc_node == NULL ||
	    !polaris_dt_string_list_equals(gpu_node, "clock-names", gpu_clock_names,
	    sizeof(gpu_clock_names) / sizeof(gpu_clock_names[0])) ||
	    !polaris_dt_clocks_match(gpu_node, "clocks", gpu_clocks, sizeof(gpu_clocks) / sizeof(gpu_clocks[0])) ||
	    !polaris_dt_string_list_equals(gmu_node, "clock-names", gmu_clock_names,
	    sizeof(gmu_clock_names) / sizeof(gmu_clock_names[0])) ||
	    !polaris_dt_clocks_match(gmu_node, "clocks", gmu_clocks, sizeof(gmu_clocks) / sizeof(gmu_clocks[0])) ||
	    !polaris_dt_string_list_equals(gpu_node, "regulator-names", regulator_names,
	    sizeof(regulator_names) / sizeof(regulator_names[0])) ||
	    !polaris_dt_string_list_equals(gmu_node, "regulator-names", regulator_names,
	    sizeof(regulator_names) / sizeof(regulator_names[0])) ||
	    !polaris_dt_get_u32(gpu_node, "vddcx-supply", &gpu_vddcx) ||
	    !polaris_dt_get_u32(gpu_node, "vdd-supply", &gpu_vdd) ||
	    !polaris_dt_get_u32(gmu_node, "vddcx-supply", &gmu_vddcx) ||
	    !polaris_dt_get_u32(gmu_node, "vdd-supply", &gmu_vdd) ||
	    !polaris_dt_get_u32(gpucc_node, "vdd_cx-supply", &gpucc_vddcx) ||
	    !polaris_dt_get_u32(gpucc_node, "vdd_mx-supply", &gpucc_vddmx) ||
	    !polaris_dt_get_u32(gfxcc_node, "vdd_gfx-supply", &gfxcc_vddgfx) || gpu_vddcx == 0 || gpu_vdd == 0 ||
	    !polaris_dt_get_u32(cx_gdsc_node, "phandle", &cx_gdsc_phandle) ||
	    !polaris_dt_get_u32(gx_gdsc_node, "phandle", &gx_gdsc_phandle) ||
	    !polaris_dt_get_u32(cx_gdsc_node, "parent-supply", &cx_parent) ||
	    !polaris_dt_get_u32(gx_gdsc_node, "parent-supply", &gx_parent) ||
	    !polaris_dt_get_u32(cx_gdsc_node, "qcom,clk-dis-wait-val", &cx_clk_dis_wait) ||
	    !polaris_dt_string_equals(cx_gdsc_node, "regulator-name", "gpu_cx_gdsc") ||
	    !polaris_dt_string_equals(gx_gdsc_node, "regulator-name", "gpu_gx_gdsc") ||
	    gpu_vddcx != cx_gdsc_phandle || gpu_vdd != gx_gdsc_phandle ||
	    gmu_vddcx != gpu_vddcx || gmu_vdd != gpu_vdd || gpu_vddcx == gpu_vdd || gpucc_vddcx == 0 ||
	    gpucc_vddmx == 0 || gfxcc_vddgfx == 0 || cx_parent != gpucc_vddcx || gx_parent != gfxcc_vddgfx ||
	    gpucc_vddcx == gpucc_vddmx || cx_clk_dis_wait != 8 ||
	    gfxcc_vddgfx == gpucc_vddcx || gfxcc_vddgfx == gpucc_vddmx) {
		printf("adreno-sdm845: runtime GPU power/clock dependencies are incomplete or differ from Polaris; probe refused\n");
		return false;
	}
	if (!polaris_runtime_rpmh_validate()) {
		return false;
	}
	if (!polaris_runtime_pwrlevels_validate() || !polaris_runtime_bus_tables_validate()) {
		return false;
	}
	printf("adreno-sdm845: runtime DT matches the SDM845 GPU/GFXCC clock tree, GPU/GMU supply domains, CX/MX/GFX rails, and GPU/CNOC bus tables\n");
	return true;
}

static bool
polaris_runtime_dt_validate(void)
{
	DTEntry gpu_node = NULL, gmu_node = NULL, smmu_node = NULL;
	const uint8_t *chip_id;
	unsigned int chip_id_size = 0;
	uint32_t actual_chip_id, smmu_phandle;

	if (!polaris_dt_node_matches(POLARIS_GPU_DT_PATH, "qcom,kgsl-3d0", POLARIS_GPU_PHYS, POLARIS_GPU_SIZE) ||
	    !polaris_dt_node_matches(POLARIS_GMU_DT_PATH, "qcom,gpu-gmu", POLARIS_GMU_PHYS, POLARIS_GMU_SIZE) ||
	    !polaris_dt_node_matches(POLARIS_GPUCC_DT_PATH, "qcom,gpucc-sdm845-v2", POLARIS_GPUCC_PHYS,
	    POLARIS_GPUCC_SIZE) ||
	    !polaris_dt_node_matches(POLARIS_GFXCC_DT_PATH, "qcom,gfxcc-sdm845-v2", POLARIS_GPUCC_PHYS,
	    POLARIS_GPUCC_SIZE) ||
	    !polaris_dt_node_matches(POLARIS_SMMU_DT_PATH, "qcom,smmu-v2", POLARIS_SMMU_PHYS, POLARIS_SMMU_SIZE) ||
	    SecureDTLookupEntry(NULL, POLARIS_GPU_DT_PATH, &gpu_node) != kSuccess || gpu_node == NULL ||
	    SecureDTGetProperty(gpu_node, "qcom,chipid", (const void **)&chip_id, &chip_id_size) != kSuccess ||
	    chip_id == NULL || chip_id_size != 4 ||
	    SecureDTLookupEntry(NULL, POLARIS_GMU_DT_PATH, &gmu_node) != kSuccess || gmu_node == NULL ||
	    SecureDTLookupEntry(NULL, POLARIS_SMMU_DT_PATH, &smmu_node) != kSuccess || smmu_node == NULL ||
	    !polaris_dt_get_u32(smmu_node, "phandle", &smmu_phandle)) {
		printf("adreno-sdm845: runtime device-tree validation failed; no GPU MMIO was accessed\n");
		return false;
	}
	actual_chip_id = polaris_dt_be32(chip_id);
	if (actual_chip_id != POLARIS_GPU_CHIP_ID) {
		printf("adreno-sdm845: runtime GPU chip ID is %#x, expected Polaris A630 %#x; probe refused\n",
		    actual_chip_id, POLARIS_GPU_CHIP_ID);
		return false;
	}
	if (!polaris_runtime_power_validate(gpu_node, gmu_node)) {
		return false;
	}
	polaris_smmu_stream_count = 0;
	if (!polaris_runtime_iommu_validate(smmu_node, smmu_phandle)) {
		return false;
	}
	printf("adreno-sdm845: runtime device tree identifies Polaris A630 (%#x); GPU, GPUCC, GMU and SMMUv2 windows match\n",
	    actual_chip_id);
	return true;
}

static uint32_t __attribute__((noinline))
probe_read(struct probe_block *block, uint32_t offset)
{
	uint32_t value;

	/* Keep the load form simple; Qualcomm's hypervisor emulates some of these device reads. */
	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (block->virtual + offset) : "memory");
	return value;
}

static void __attribute__((noinline))
probe_write(struct probe_block *block, uint32_t offset, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (block->virtual + offset) : "memory");
}

static void __attribute__((noinline))
probe_write64(struct probe_block *block, uint32_t offset, uint64_t value)
{
	__asm__ volatile ("str %x0, [%1]" : : "r" (value), "r" (block->virtual + offset) : "memory");
}

static bool
polaris_smmu_phys_valid(uint64_t phys, uint64_t size)
{
	uint64_t limit;

	if (!polaris_smmu_supports_4k || polaris_smmu_oas_bits < 32 || polaris_smmu_oas_bits > 48 || size == 0) {
		return false;
	}
	limit = (1ull << polaris_smmu_oas_bits) - 1;
	return phys <= limit && size - 1 <= limit - phys;
}

static struct polaris_smmu_table *
polaris_smmu_table_new(void)
{
	vm_offset_t address;
	uint64_t phys;
	uint32_t slots_per_page;
	struct polaris_smmu_table *table;

	if (polaris_smmu_table_count == POLARIS_SMMU_MAX_TABLES || PAGE_SIZE < POLARIS_SMMU_TABLE_BYTES ||
	    PAGE_SIZE % POLARIS_SMMU_TABLE_BYTES != 0) {
		return NULL;
	}
	slots_per_page = PAGE_SIZE / POLARIS_SMMU_TABLE_BYTES;
	if (polaris_smmu_table_chunk == 0 || polaris_smmu_chunk_slots == slots_per_page) {
		vm_offset_t chunk = 0;

		if (kmem_alloc(kernel_map, &chunk, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return NULL;
		}
		polaris_smmu_table_chunk = chunk;
		polaris_smmu_chunk_slots = 0;
	}
	address = polaris_smmu_table_chunk + (vm_offset_t)polaris_smmu_chunk_slots++ * POLARIS_SMMU_TABLE_BYTES;
	phys = kvtophys(address);
	if ((phys & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 || !polaris_smmu_phys_valid(phys, POLARIS_SMMU_TABLE_BYTES)) {
		/* Do not reuse a virtual table page with a physical address the walker cannot reach. */
		return NULL;
	}
	table = &polaris_smmu_tables[polaris_smmu_table_count++];
	table->entries = (uint64_t *)address;
	table->phys = phys;
	table->dirty = false;
	return table;
}

static struct polaris_smmu_table *
polaris_smmu_table_from_phys(uint64_t phys)
{
	phys &= POLARIS_SMMU_PTE_ADDRESS;
	for (uint32_t i = 0; i < polaris_smmu_table_count; i++) {
		if (polaris_smmu_tables[i].phys == phys) {
			return &polaris_smmu_tables[i];
		}
	}
	return NULL;
}

static struct polaris_smmu_table *
polaris_smmu_table_child(struct polaris_smmu_table *parent, uint32_t index, bool create)
{
	uint64_t descriptor = parent->entries[index];
	struct polaris_smmu_table *child;

	if (descriptor == 0) {
		if (!create || (child = polaris_smmu_table_new()) == NULL) {
			return NULL;
		}
		parent->entries[index] = child->phys | POLARIS_SMMU_PTE_TABLE;
		parent->dirty = true;
		return child;
	}
	if ((descriptor & 3u) != POLARIS_SMMU_PTE_TABLE) {
		return NULL;
	}
	return polaris_smmu_table_from_phys(descriptor);
}

static void
polaris_cache_lines(vm_offset_t start, uint64_t size)
{
	uint64_t end = (uint64_t)start + size;
	uint64_t line;

	if (size == 0 || end < (uint64_t)start) {
		return;
	}
	line = (uint64_t)start & ~(uint64_t)(POLARIS_SMMU_CACHE_LINE - 1);
	while (line < end) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
		if (line > UINT64_MAX - POLARIS_SMMU_CACHE_LINE) {
			break;
		}
		line += POLARIS_SMMU_CACHE_LINE;
	}
}

static void
polaris_smmu_commit_locked(void)
{
	/* Flush child tables before the parent descriptors that make them reachable. */
	for (uint32_t i = polaris_smmu_table_count; i-- > 0;) {
		if (polaris_smmu_tables[i].dirty) {
			polaris_cache_lines((vm_offset_t)polaris_smmu_tables[i].entries, POLARIS_SMMU_TABLE_BYTES);
			polaris_smmu_tables[i].dirty = false;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
polaris_smmu_tlbi(struct polaris_smmu_address_space *space)
{
	uint64_t context_offset;
	uint32_t context;

	if (space == NULL || !space->context_active) {
		return true;
	}
	context_offset = (uint64_t)polaris_smmu_context_base +
	    (uint64_t)space->context_bank * polaris_smmu_context_page_size;
	if (smmu.virtual == 0 || polaris_smmu_context_page_size == 0 ||
	    space->context_bank >= polaris_smmu_context_bank_count ||
	    context_offset + polaris_smmu_context_page_size > smmu.size || context_offset > UINT32_MAX) {
		return false;
	}
	context = (uint32_t)context_offset;
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&smmu, context + SMMU_CB_TLBIASID, space->asid);
	probe_write(&smmu, context + SMMU_CB_TLBSYNC, 1);
	for (uint32_t retry = 0; retry < 10000; retry++) {
		if ((probe_read(&smmu, context + SMMU_CB_TLBSTATUS) & 1u) == 0) {
			__asm__ volatile ("dsb sy" ::: "memory");
			return true;
		}
	}
	printf("adreno-sdm845: SMMU %s context %u did not finish TLB invalidation\n",
	    space->name, space->context_bank);
	return false;
}

static bool
polaris_smmu_map_page_locked(struct polaris_smmu_address_space *space, uint64_t address, uint64_t phys,
	bool read_only)
{
	uint32_t l1_index, l2_index, l3_index;
	struct polaris_smmu_table *l2, *l3;
	uint64_t descriptor;

	if (space == NULL || address >= POLARIS_GPU_VA_END ||
	    (address & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    (phys & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    !polaris_smmu_phys_valid(phys, POLARIS_SMMU_TABLE_BYTES)) {
		return false;
	}
	l1_index = (uint32_t)(address >> 30) & 0x1ffu;
	l2_index = (uint32_t)(address >> 21) & 0x1ffu;
	l3_index = (uint32_t)(address >> 12) & 0x1ffu;

	if (space->root == NULL && (space->root = polaris_smmu_table_new()) == NULL) {
		return false;
	}
	if (space->cached_l1_index == l1_index && space->cached_l2 != NULL &&
	    space->root->entries[l1_index] == (space->cached_l2->phys | POLARIS_SMMU_PTE_TABLE)) {
		l2 = space->cached_l2;
	} else {
		l2 = polaris_smmu_table_child(space->root, l1_index, true);
		space->cached_l1_index = l1_index;
		space->cached_l2_index = POLARIS_SMMU_CACHE_INDEX_NONE;
		space->cached_l2 = l2;
		space->cached_l3 = NULL;
	}
	if (l2 == NULL) {
		return false;
	}
	if (space->cached_l1_index == l1_index && space->cached_l2_index == l2_index &&
	    space->cached_l3 != NULL &&
	    l2->entries[l2_index] == (space->cached_l3->phys | POLARIS_SMMU_PTE_TABLE)) {
		l3 = space->cached_l3;
	} else {
		l3 = polaris_smmu_table_child(l2, l2_index, true);
		space->cached_l2_index = l2_index;
		space->cached_l3 = l3;
	}
	if (l3 == NULL || l3->entries[l3_index] != 0) {
		return false;
	}
	descriptor = phys | space->pte_attributes | POLARIS_SMMU_PTE_PAGE;
	if (read_only) {
		descriptor |= POLARIS_SMMU_PTE_READ_ONLY;
	}
	l3->entries[l3_index] = descriptor;
	l3->dirty = true;
	return true;
}

static bool
polaris_smmu_space_map_page(struct polaris_smmu_address_space *space, uint64_t address, uint64_t phys,
	bool read_only)
{
	bool ok;

	lck_mtx_lock(&polaris_smmu_pt_lock);
	ok = polaris_smmu_map_page_locked(space, address, phys, read_only);
	lck_mtx_unlock(&polaris_smmu_pt_lock);
	return ok;
}

/* Build the AArch64 stage-1 register image; this helper never touches the SMMU. */
static bool
polaris_smmu_context_image_build(const struct polaris_smmu_address_space *space,
	struct polaris_smmu_context_image *image)
{
	uint32_t tcr;

	if (space == NULL || image == NULL || space->root == NULL || space->asid == 0 || space->asid > 0xffffu ||
	    !polaris_smmu_supports_4k || polaris_smmu_oas_bits < 32 || polaris_smmu_oas_bits > 48 ||
	    polaris_smmu_oas_encoding > 5u ||
	    polaris_smmu_oas_bits != smmu_id_size_to_bits(polaris_smmu_oas_encoding) ||
	    !polaris_smmu_phys_valid(space->root->phys, POLARIS_SMMU_TABLE_BYTES) ||
	    (space->root->phys & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0) {
		return false;
	}
	memset(image, 0, sizeof(*image));
	image->cbar = (SMMU_CBAR_TYPE_S1_S2_BYPASS << SMMU_CBAR_TYPE_SHIFT) |
	    (3u << SMMU_CBAR_S1_BPSHCFG_SHIFT) | (SMMU_CBAR_S1_MEMATTR_WB << SMMU_CBAR_S1_MEMATTR_SHIFT);
	image->cba2r = SMMU_CBA2R_RW64;
	tcr = (64u - 39u) & SMMU_CB_TCR_T0SZ_MASK;
	tcr |= SMMU_CB_TCR_EPD1;
	if (polaris_smmu_coherent_walk) {
		tcr |= (1u << SMMU_CB_TCR_IRGN0_SHIFT) | (1u << SMMU_CB_TCR_ORGN0_SHIFT) |
		    (3u << SMMU_CB_TCR_SH0_SHIFT);
	} else {
		/* Noncoherent walks use outer-shareable, noncacheable table accesses; PTEs are cleaned. */
		tcr |= (2u << SMMU_CB_TCR_SH0_SHIFT);
	}
	image->tcr = tcr;
	image->tcr2 = (polaris_smmu_oas_encoding & SMMU_CB_TCR2_PASIZE_MASK) | SMMU_CB_TCR2_AS |
	    (SMMU_CB_TCR2_SEP_UPSTREAM << SMMU_CB_TCR2_SEP_SHIFT);
	image->mair0 = SMMU_CB_MAIR0_LPAE;
	image->sctlr = SMMU_SCTLR_M | SMMU_CB_SCTLR_TRE | SMMU_CB_SCTLR_AFE |
	    SMMU_CB_SCTLR_CFRE | SMMU_CB_SCTLR_S1_ASIDPNE;
	image->ttbr0 = space->root->phys | ((uint64_t)space->asid << 48);
	image->ttbr1 = (uint64_t)space->asid << 48;
	return true;
}

static bool
polaris_smmu_context_is_blank(uint32_t bank)
{
	uint64_t offset;
	uint32_t ttbr0_lo, ttbr0_hi, ttbr1_lo, ttbr1_hi;

	if (smmu.virtual == 0 || polaris_smmu_context_page_size == 0 ||
	    bank >= polaris_smmu_context_bank_count) {
		return false;
	}
	offset = (uint64_t)polaris_smmu_context_base + (uint64_t)bank * polaris_smmu_context_page_size;
	if (offset + SMMU_CB_FSR + sizeof(uint32_t) > smmu.size || offset > UINT32_MAX) {
		return false;
	}
	ttbr0_lo = probe_read(&smmu, (uint32_t)offset + SMMU_CB_TTBR0);
	ttbr0_hi = probe_read(&smmu, (uint32_t)offset + SMMU_CB_TTBR0 + 4u);
	ttbr1_lo = probe_read(&smmu, (uint32_t)offset + SMMU_CB_TTBR1);
	ttbr1_hi = probe_read(&smmu, (uint32_t)offset + SMMU_CB_TTBR1 + 4u);
	return probe_read(&smmu, SMMU_CBAR(bank)) == 0 && probe_read(&smmu, SMMU_CBA2R(bank)) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_SCTLR) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_ACTLR) == 0 && ttbr0_lo == 0 && ttbr0_hi == 0 &&
	    ttbr1_lo == 0 && ttbr1_hi == 0 && probe_read(&smmu, (uint32_t)offset + SMMU_CB_TCR) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_TCR2) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_CONTEXTIDR) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_MAIR0) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_MAIR1) == 0 &&
	    probe_read(&smmu, (uint32_t)offset + SMMU_CB_FSR) == 0;
}

static bool
polaris_smmu_context_attach(struct polaris_smmu_address_space *space,
	const struct polaris_smmu_route_candidate *route)
{
	struct polaris_smmu_context_image image;
	const char *name;
	uint32_t expected_stream_id;
	uint32_t old_smr = 0, old_s2cr, new_s2cr, new_smr = 0;
	uint32_t context, reset, init_result;
	uint64_t context_offset;
	bool context_touched = false, s2cr_touched = false, smr_touched = false;
	bool restored;

	if (space == NULL || route == NULL) {
		printf("adreno-sdm845: SMMU attach received a null address space or route\n");
		return false;
	}
	if (space == &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_KERNEL]) {
		name = "GMU-kernel";
		expected_stream_id = 5u;
	} else if (space == &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_USER]) {
		name = "GMU-user";
		expected_stream_id = 4u;
	} else if (space == &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU]) {
		name = "3D-user";
		expected_stream_id = 0u;
	} else {
		printf("adreno-sdm845: SMMU attach received an unsupported address space\n");
		return false;
	}
	if (!route->valid || !polaris_smmu_dynamic_routes_safe ||
	    !polaris_smmu_client_enabled || !polaris_smmu_context_snapshot_valid || !polaris_smmu_route_scan_complete ||
	    route->stream_id != expected_stream_id || route->slot >= POLARIS_SMMU_MAX_CONTEXT_BANKS ||
	    (expected_stream_id == 0u ? route->context_bank != 0u :
	    (route->context_bank < 1u || route->context_bank > 4u)) ||
	    route->context_bank >= polaris_smmu_context_bank_count || polaris_smmu_context_routed[route->context_bank] ||
	    !polaris_smmu_context_blank[route->context_bank] || polaris_smmu_backend_faulted ||
	    polaris_gpu_core_ready || !polaris_gmu_domain_ready || gmu.virtual == 0 || space->context_active ||
	    space->root == NULL || !polaris_smmu_context_image_build(space, &image)) {
		printf("adreno-sdm845: %s SMMU attach preconditions are incomplete; no attach writes were made\n", name);
		return false;
	}
	for (uint32_t i = 0; i < POLARIS_SMMU_ADDRESS_SPACE_COUNT; i++) {
		if (&polaris_smmu_spaces[i] != space && polaris_smmu_spaces[i].context_active &&
		    polaris_smmu_spaces[i].context_bank == route->context_bank) {
			printf("adreno-sdm845: %s CB%u is already assigned to %s; attach refused\n", name,
			    route->context_bank, polaris_smmu_spaces[i].name);
			return false;
		}
	}
	reset = probe_read(&gmu, GMU_CM3_SYSRESET);
	init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
	if ((reset & 1u) == 0 || init_result == A630_GMU_FW_READY) {
		printf("adreno-sdm845: %s attach requires CM3 held in reset (reset=%#x firmware-init=%#x)\n",
		    name, reset, init_result);
		return false;
	}
	if ((probe_read(&smmu, SMMU_SCR0) & SMMU_SCR0_CLIENTPD) != 0 ||
	    !polaris_smmu_context_is_blank(route->context_bank)) {
		printf("adreno-sdm845: %s context or SMMU client state changed since the read-only scan; attach refused\n", name);
		return false;
	}
	context_offset = (uint64_t)polaris_smmu_context_base +
	    (uint64_t)route->context_bank * polaris_smmu_context_page_size;
	if (context_offset + polaris_smmu_context_page_size > smmu.size || context_offset > UINT32_MAX ||
	    SMMU_CBA2R(route->context_bank) + sizeof(uint32_t) > polaris_smmu_context_base ||
	    SMMU_S2CR(route->slot) + sizeof(uint32_t) > smmu.size ||
	    (route->stream_matching && SMMU_SMR(route->slot) + sizeof(uint32_t) > polaris_smmu_context_base)) {
		printf("adreno-sdm845: %s context or route register window is invalid; attach refused\n", name);
		return false;
	}
	context = (uint32_t)context_offset;
	old_s2cr = probe_read(&smmu, SMMU_S2CR(route->slot));
	if (old_s2cr != route->expected_s2cr) {
		printf("adreno-sdm845: %s S2CR[%u] changed since planning; attach refused\n", name, route->slot);
		return false;
	}
	if (route->stream_matching) {
		old_smr = probe_read(&smmu, SMMU_SMR(route->slot));
		if (old_smr != route->expected_smr || (!!(old_smr & SMMU_SMR_VALID) != route->existing_smr)) {
			printf("adreno-sdm845: %s SMR[%u] changed since planning; attach refused\n", name, route->slot);
			return false;
		}
	}
	if (((old_s2cr >> SMMU_S2CR_TYPE_SHIFT) & SMMU_S2CR_TYPE_MASK) == SMMU_S2CR_TYPE_TRANS) {
		printf("adreno-sdm845: %s S2CR[%u] already translates; attach refused\n", name, route->slot);
		return false;
	}

	printf("adreno-sdm845: explicit SMMUv2 stage: configure %s ASID %u at CB%u for SID%u; this stage writes context and route registers\n",
	    name, space->asid, route->context_bank, route->stream_id);
	context_touched = true;
	probe_write(&smmu, context + SMMU_CB_SCTLR, 0);
	probe_write(&smmu, SMMU_CBA2R(route->context_bank), image.cba2r);
	probe_write(&smmu, SMMU_CBAR(route->context_bank), image.cbar);
	probe_write(&smmu, context + SMMU_CB_TCR2, image.tcr2);
	probe_write(&smmu, context + SMMU_CB_TCR, image.tcr);
	probe_write64(&smmu, context + SMMU_CB_TTBR0, image.ttbr0);
	probe_write64(&smmu, context + SMMU_CB_TTBR1, image.ttbr1);
	probe_write(&smmu, context + SMMU_CB_CONTEXTIDR, 0);
	probe_write(&smmu, context + SMMU_CB_MAIR0, image.mair0);
	probe_write(&smmu, context + SMMU_CB_MAIR1, image.mair1);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&smmu, SMMU_CBA2R(route->context_bank)) != image.cba2r ||
	    probe_read(&smmu, SMMU_CBAR(route->context_bank)) != image.cbar ||
	    probe_read(&smmu, context + SMMU_CB_TCR2) != image.tcr2 ||
	    probe_read(&smmu, context + SMMU_CB_TCR) != image.tcr ||
	    probe_read(&smmu, context + SMMU_CB_TTBR0) != (uint32_t)image.ttbr0 ||
	    probe_read(&smmu, context + SMMU_CB_TTBR0 + 4u) != (uint32_t)(image.ttbr0 >> 32) ||
	    probe_read(&smmu, context + SMMU_CB_TTBR1) != (uint32_t)image.ttbr1 ||
	    probe_read(&smmu, context + SMMU_CB_TTBR1 + 4u) != (uint32_t)(image.ttbr1 >> 32) ||
	    probe_read(&smmu, context + SMMU_CB_CONTEXTIDR) != 0 ||
	    probe_read(&smmu, context + SMMU_CB_MAIR0) != image.mair0 ||
	    probe_read(&smmu, context + SMMU_CB_MAIR1) != image.mair1) {
		printf("adreno-sdm845: %s context register readback failed before SCTLR enable\n", name);
		goto rollback;
	}
	probe_write(&smmu, context + SMMU_CB_SCTLR, image.sctlr);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&smmu, context + SMMU_CB_SCTLR) != image.sctlr) {
		printf("adreno-sdm845: %s SCTLR readback failed\n", name);
		goto rollback;
	}
	space->context_bank = route->context_bank;
	space->context_active = true;
	if (!polaris_smmu_tlbi(space)) {
		polaris_smmu_backend_faulted = true;
		goto rollback;
	}

	new_s2cr = old_s2cr & ~(SMMU_S2CR_TYPE_FIELD_MASK | SMMU_S2CR_CBNDX_MASK);
	new_s2cr |= (SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | route->context_bank;
	s2cr_touched = true;
	probe_write(&smmu, SMMU_S2CR(route->slot), new_s2cr);
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((probe_read(&smmu, SMMU_S2CR(route->slot)) &
	    (SMMU_S2CR_TYPE_FIELD_MASK | SMMU_S2CR_CBNDX_MASK)) !=
	    (new_s2cr & (SMMU_S2CR_TYPE_FIELD_MASK | SMMU_S2CR_CBNDX_MASK))) {
		printf("adreno-sdm845: %s S2CR readback failed\n", name);
		goto rollback;
	}
	if (route->stream_matching && !route->existing_smr) {
		new_smr = SMMU_SMR_VALID | (route->stream_id & SMMU_SMR_ID_MASK);
		smr_touched = true;
		probe_write(&smmu, SMMU_SMR(route->slot), new_smr);
		__asm__ volatile ("dsb sy" ::: "memory");
		if ((probe_read(&smmu, SMMU_SMR(route->slot)) &
		    (SMMU_SMR_VALID | SMMU_SMR_ID_MASK | (SMMU_SMR_MASK_MASK << SMMU_SMR_MASK_SHIFT))) != new_smr) {
			printf("adreno-sdm845: %s SMR readback failed\n", name);
			goto rollback;
		}
	}
	printf("adreno-sdm845: %s SMMU context is active at CB%u for SID%u; firmware/HFI startup remains disabled\n",
	    name, route->context_bank, route->stream_id);
	return true;

rollback:
	if (s2cr_touched) {
		probe_write(&smmu, SMMU_S2CR(route->slot), old_s2cr);
	}
	if (smr_touched) {
		probe_write(&smmu, SMMU_SMR(route->slot), old_smr);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	space->context_active = false;
	space->context_bank = UINT32_MAX;
	if (context_touched) {
		probe_write(&smmu, context + SMMU_CB_SCTLR, 0);
		probe_write64(&smmu, context + SMMU_CB_TTBR0, 0);
		probe_write64(&smmu, context + SMMU_CB_TTBR1, 0);
		probe_write(&smmu, context + SMMU_CB_TCR, 0);
		probe_write(&smmu, context + SMMU_CB_TCR2, 0);
		probe_write(&smmu, context + SMMU_CB_CONTEXTIDR, 0);
		probe_write(&smmu, context + SMMU_CB_MAIR0, 0);
		probe_write(&smmu, context + SMMU_CB_MAIR1, 0);
		probe_write(&smmu, SMMU_CBAR(route->context_bank), 0);
		probe_write(&smmu, SMMU_CBA2R(route->context_bank), 0);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	restored = (!context_touched || polaris_smmu_context_is_blank(route->context_bank)) &&
	    (!s2cr_touched || probe_read(&smmu, SMMU_S2CR(route->slot)) == old_s2cr) &&
	    (!smr_touched || probe_read(&smmu, SMMU_SMR(route->slot)) == old_smr);
	if (!restored) {
		polaris_smmu_backend_faulted = true;
		printf("adreno-sdm845: %s SMMU rollback could not be verified; backend is faulted and remains disabled\n", name);
	} else if (polaris_smmu_backend_faulted) {
		printf("adreno-sdm845: %s SMMU state was restored, but the backend remains faulted after TLB synchronization failure\n",
		    name);
	} else {
		printf("adreno-sdm845: %s SMMU attach failed; original route and blank context were restored\n", name);
	}
	return false;
}

static bool
polaris_smmu_gpu_attach(void)
{
	return polaris_smmu_context_attach(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU],
	    &polaris_smmu_gpu_route);
}

static bool
polaris_smmu_gmu_kernel_attach(void)
{
	return polaris_smmu_context_attach(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_KERNEL],
	    &polaris_smmu_gmu_kernel_route);
}

static bool
polaris_smmu_gmu_user_attach(void)
{
	return polaris_smmu_context_attach(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_USER],
	    &polaris_smmu_gmu_user_route);
}

static void
polaris_smmu_unmap_locked(struct polaris_smmu_address_space *space, uint64_t address, uint64_t size)
{
	for (uint64_t offset = 0; offset < size; offset += POLARIS_SMMU_TABLE_BYTES) {
		uint64_t iova = address + offset;
		uint32_t l1_index = (uint32_t)(iova >> 30) & 0x1ffu;
		uint32_t l2_index = (uint32_t)(iova >> 21) & 0x1ffu;
		uint32_t l3_index = (uint32_t)(iova >> 12) & 0x1ffu;
		struct polaris_smmu_table *l2, *l3;

		if (space->root == NULL) {
			break;
		}
		if (space->cached_l1_index == l1_index && space->cached_l2 != NULL &&
		    space->root->entries[l1_index] == (space->cached_l2->phys | POLARIS_SMMU_PTE_TABLE)) {
			l2 = space->cached_l2;
		} else {
			l2 = polaris_smmu_table_child(space->root, l1_index, false);
			space->cached_l1_index = l1_index;
			space->cached_l2_index = POLARIS_SMMU_CACHE_INDEX_NONE;
			space->cached_l2 = l2;
			space->cached_l3 = NULL;
		}
		if (l2 == NULL) {
			continue;
		}
		if (space->cached_l1_index == l1_index && space->cached_l2_index == l2_index &&
		    space->cached_l3 != NULL &&
		    l2->entries[l2_index] == (space->cached_l3->phys | POLARIS_SMMU_PTE_TABLE)) {
			l3 = space->cached_l3;
		} else {
			l3 = polaris_smmu_table_child(l2, l2_index, false);
			space->cached_l2_index = l2_index;
			space->cached_l3 = l3;
		}
		if (l3 != NULL && l3->entries[l3_index] != 0) {
			l3->entries[l3_index] = 0;
			l3->dirty = true;
		}
	}
}

static void
polaris_smmu_space_unmap(struct polaris_smmu_address_space *space, uint64_t address, uint64_t size)
{
	if (space == NULL || size == 0 || address >= POLARIS_GPU_VA_END ||
	    (address & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    (size & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 || size > POLARIS_GPU_VA_END - address) {
		return;
	}
	lck_mtx_lock(&polaris_smmu_pt_lock);
	polaris_smmu_unmap_locked(space, address, size);
	polaris_smmu_commit_locked();
	if (!polaris_smmu_tlbi(space)) {
		/* A timed-out invalidation prevents safe IOVA reuse; keep the backend disabled. */
		polaris_smmu_backend_faulted = true;
		printf("adreno-sdm845: A630 backend disabled after %s SMMU TLB invalidation failure\n", space->name);
	}
	lck_mtx_unlock(&polaris_smmu_pt_lock);
}

static bool
sdm845_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return polaris_smmu_space_map_page(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU], address, phys, read_only);
}

static void
sdm845_gpu_mapped(void)
{
	lck_mtx_lock(&polaris_smmu_pt_lock);
	polaris_smmu_commit_locked();
	lck_mtx_unlock(&polaris_smmu_pt_lock);
}

static void
sdm845_gpu_unmap(uint64_t address, uint64_t size)
{
	polaris_smmu_space_unmap(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU], address, size);
}

static void
sdm845_gpu_to_memory(void *start, uint64_t size)
{
	polaris_cache_lines((vm_offset_t)start, size);
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
polaris_smmu_space_root_prepare(struct polaris_smmu_address_space *space)
{
	bool ready;

	if (space == NULL) {
		return false;
	}
	lck_mtx_lock(&polaris_smmu_pt_lock);
	if (space->root == NULL) {
		space->root = polaris_smmu_table_new();
		if (space->root != NULL) {
			space->root->dirty = true;
		}
	}
	ready = space->root != NULL;
	if (ready) {
		polaris_smmu_commit_locked();
	}
	lck_mtx_unlock(&polaris_smmu_pt_lock);
	if (!ready) {
		printf("adreno-sdm845: cannot allocate the detached SMMUv2 root for %s\n", space->name);
	}
	return ready;
}

static void
polaris_gpu_dma_buffer_release(struct polaris_gpu_dma_buffer *buffer)
{
	if (buffer == NULL) {
		return;
	}
	if (buffer->gpu_address != 0 && buffer->allocation_size != 0) {
		polaris_smmu_space_unmap(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU], buffer->gpu_address,
		    buffer->allocation_size);
		polaris_gpu_private_address_free(buffer->gpu_address, buffer->allocation_size);
	}
	if (buffer->virtual != 0 && buffer->allocation_size != 0) {
		kmem_free(kernel_map, buffer->virtual, buffer->allocation_size);
	}
	bzero(buffer, sizeof(*buffer));
}

static bool
polaris_gpu_dma_buffer_allocate(struct polaris_gpu_dma_buffer *buffer, uint64_t size,
	bool gpu_read_only)
{
	struct polaris_smmu_address_space *space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU];
	vm_size_t allocation_size;
	vm_offset_t allocation = 0;
	uint64_t physical, gpu_address, mapped_size = 0;

	if (buffer == NULL || size == 0 || size > UINT32_MAX || buffer->virtual != 0 ||
	    buffer->gpu_address != 0 || !polaris_smmu_supports_4k || !polaris_smmu_space_root_prepare(space)) {
		return false;
	}
	allocation_size = (vm_size_t)round_page((vm_size_t)size);
	if (allocation_size < size || allocation_size == 0 ||
	    (allocation_size & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    kmem_alloc_contig(kernel_map, &allocation, allocation_size, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno-sdm845: step 16 could not allocate a contiguous %llu-byte A630 GPU buffer\n", size);
		return false;
	}
	physical = kvtophys(allocation);
	if ((physical & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    !polaris_smmu_phys_valid(physical, allocation_size)) {
		kmem_free(kernel_map, allocation, allocation_size);
		printf("adreno-sdm845: step 16 GPU buffer is outside the SMMU output range or misaligned\n");
		return false;
	}
	for (vm_size_t offset = POLARIS_SMMU_TABLE_BYTES; offset < allocation_size;
	    offset += POLARIS_SMMU_TABLE_BYTES) {
		if (kvtophys(allocation + offset) != physical + offset) {
			kmem_free(kernel_map, allocation, allocation_size);
			printf("adreno-sdm845: step 16 GPU buffer pages are not physically contiguous\n");
			return false;
		}
	}
	gpu_address = polaris_gpu_private_address_allocate(allocation_size);
	if (gpu_address == 0 || (gpu_address & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0) {
		if (gpu_address != 0) {
			polaris_gpu_private_address_free(gpu_address, allocation_size);
		}
		kmem_free(kernel_map, allocation, allocation_size);
		printf("adreno-sdm845: step 16 could not reserve a 64-KiB-aligned A630 GPU virtual range\n");
		return false;
	}
	buffer->virtual = allocation;
	buffer->allocation_size = allocation_size;
	buffer->requested_size = size;
	buffer->physical = physical;
	buffer->gpu_address = gpu_address;
	buffer->gpu_read_only = gpu_read_only;
	for (uint64_t offset = 0; offset < allocation_size; offset += POLARIS_SMMU_TABLE_BYTES) {
		if (!polaris_smmu_space_map_page(space, gpu_address + offset,
		    physical + offset, gpu_read_only)) {
			if (mapped_size != 0) {
				polaris_smmu_space_unmap(space, gpu_address, mapped_size);
			}
			polaris_gpu_dma_buffer_release(buffer);
			printf("adreno-sdm845: step 16 could not add the A630 GPU buffer to the detached SID0 page table\n");
			return false;
		}
		mapped_size += POLARIS_SMMU_TABLE_BYTES;
	}
	return true;
}

static bool
polaris_gpu_cp_software_prepare(void)
{
	uint32_t cp_init[A630_CP_INIT_PACKET_DWORDS], secure_mode[A630_CP_SECURE_MODE_DWORDS];
	uint64_t sqe_payload_size;
	bool all_present;

	printf("adreno-sdm845: step 16: map A630 SQE, CP ring and RPTR scratch into the detached 3D SID0 page table\n");
	if (!polaris_sqe_safe || polaris_sqe_image == NULL ||
	    polaris_sqe_image_size <= A630_SQE_FILE_PREFIX_BYTES || !polaris_smmu_gpu_route.valid ||
	    polaris_gpu_core_ready || polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ||
	    polaris_smmu_backend_faulted || !polaris_smmu_supports_4k ||
	    !polaris_a630_cp_bootstrap_packets(cp_init, sizeof(cp_init) / sizeof(cp_init[0]),
	    secure_mode, sizeof(secure_mode) / sizeof(secure_mode[0]))) {
		printf("adreno-sdm845: step 16 requires a validated SQE, a safe SID0/CB0 candidate, and a detached healthy GPU root\n");
		return false;
	}
	all_present = polaris_gpu_sqe_buffer.virtual != 0 && polaris_gpu_ring_buffer.virtual != 0 &&
	    polaris_gpu_rptr_buffer.virtual != 0;
	if (all_present) {
		printf("adreno-sdm845: step 16 CP buffers are already prepared at SQE=%#llx ring=%#llx RPTR=%#llx\n",
		    polaris_gpu_sqe_buffer.gpu_address, polaris_gpu_ring_buffer.gpu_address,
		    polaris_gpu_rptr_buffer.gpu_address);
		return true;
	}
	if (polaris_gpu_sqe_buffer.virtual != 0 || polaris_gpu_ring_buffer.virtual != 0 ||
	    polaris_gpu_rptr_buffer.virtual != 0 ||
	    !polaris_smmu_space_root_prepare(&polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU])) {
		printf("adreno-sdm845: step 16 found partial CP-buffer state or could not prepare the detached GPU root\n");
		return false;
	}
	sqe_payload_size = polaris_sqe_image_size - A630_SQE_FILE_PREFIX_BYTES;
	if (!polaris_gpu_dma_buffer_allocate(&polaris_gpu_sqe_buffer, sqe_payload_size, true) ||
	    !polaris_gpu_dma_buffer_allocate(&polaris_gpu_ring_buffer, A630_CP_RING_BYTES, true) ||
	    !polaris_gpu_dma_buffer_allocate(&polaris_gpu_rptr_buffer, PAGE_SIZE, false)) {
		polaris_gpu_dma_buffer_release(&polaris_gpu_rptr_buffer);
		polaris_gpu_dma_buffer_release(&polaris_gpu_ring_buffer);
		polaris_gpu_dma_buffer_release(&polaris_gpu_sqe_buffer);
		return false;
	}
	memcpy((void *)polaris_gpu_sqe_buffer.virtual,
	    polaris_sqe_image + A630_SQE_FILE_PREFIX_BYTES, (size_t)sqe_payload_size);
	memcpy((void *)polaris_gpu_ring_buffer.virtual, cp_init, sizeof(cp_init));
	polaris_cache_lines(polaris_gpu_sqe_buffer.virtual, polaris_gpu_sqe_buffer.allocation_size);
	polaris_cache_lines(polaris_gpu_ring_buffer.virtual, polaris_gpu_ring_buffer.allocation_size);
	polaris_cache_lines(polaris_gpu_rptr_buffer.virtual, polaris_gpu_rptr_buffer.allocation_size);
	sdm845_gpu_mapped();
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("adreno-sdm845: step 16 prepared SQE=%llu bytes at IOVA=%#llx, CP ring=%u bytes at IOVA=%#llx, RPTR scratch at IOVA=%#llx; SQE/ring are GPU read-only, RPTR scratch is GPU writable; no SMMU registers or CP were touched\n",
	    sqe_payload_size, polaris_gpu_sqe_buffer.gpu_address, A630_CP_RING_BYTES,
	    polaris_gpu_ring_buffer.gpu_address, polaris_gpu_rptr_buffer.gpu_address);
	return true;
}

/* Prepare the HFI buffers and all detached GPU/GMU roots without enabling the SMMU. */
static bool
polaris_hfi_software_prepare(void)
{
	struct polaris_smmu_address_space *gpu_space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU];
	struct polaris_smmu_address_space *space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_KERNEL];
	struct polaris_smmu_address_space *user_space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_USER];
	struct polaris_smmu_context_image context_image;
	struct polaris_smmu_context_image gpu_context_image;
	struct polaris_smmu_context_image user_context_image;
	vm_size_t allocation_size;
	vm_offset_t allocation = 0, uncached;
	uint64_t phys;
	static const uint32_t gmu_address[] = {
		POLARIS_A630_HFI_GMU_KERNEL_BASE,
		A630_GMU_DUMP_VA,
		A630_GMU_LOG_VA,
	};
	static const uint64_t physical_offset[] = {
		0,
		POLARIS_A630_HFI_ALLOCATION_BYTES,
		POLARIS_A630_HFI_ALLOCATION_BYTES + A630_GMU_HFI_DUMP_BYTES,
	};
	static const uint64_t region_bytes[] = {
		POLARIS_A630_HFI_ALLOCATION_BYTES,
		A630_GMU_HFI_DUMP_BYTES,
		A630_GMU_LOG_BYTES,
	};
	uint64_t mapped[sizeof(gmu_address) / sizeof(gmu_address[0])] = { 0 };

	if (!polaris_smmu_supports_4k || polaris_smmu_oas_bits < 32 || polaris_smmu_oas_bits > 48 ||
	    polaris_smmu_context_page_size == 0 || polaris_smmu_context_bank_count == 0) {
		printf("adreno-sdm845: step 9 requires a validated SMMUv2 4-KiB stage-1 configuration\n");
		return false;
	}
	if (!polaris_smmu_space_root_prepare(gpu_space) || !polaris_smmu_space_root_prepare(user_space)) {
		return false;
	}
	if (polaris_hfi_allocation == 0) {
		allocation_size = (vm_size_t)round_page(A630_GMU_SHARED_BYTES);
		if (allocation_size < A630_GMU_SHARED_BYTES ||
		    kmem_alloc_contig(kernel_map, &allocation, allocation_size, PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("adreno-sdm845: step 9 could not allocate contiguous GMU shared memory (%u bytes)\n",
			    (uint32_t)A630_GMU_SHARED_BYTES);
			return false;
		}
		phys = kvtophys(allocation);
		if ((phys & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
		    !polaris_smmu_phys_valid(phys, allocation_size)) {
			kmem_free(kernel_map, allocation, allocation_size);
			printf("adreno-sdm845: step 9 HFI allocation is outside the SMMU output address range or is misaligned\n");
			return false;
		}
		for (vm_size_t offset = POLARIS_SMMU_TABLE_BYTES; offset < allocation_size;
		    offset += POLARIS_SMMU_TABLE_BYTES) {
			if (kvtophys(allocation + offset) != phys + offset) {
				kmem_free(kernel_map, allocation, allocation_size);
				printf("adreno-sdm845: step 9 HFI pages are not physically contiguous\n");
				return false;
			}
		}
		uncached = ml_io_map_wcomb((vm_offset_t)phys, allocation_size);
		if (uncached == 0) {
			kmem_free(kernel_map, allocation, allocation_size);
			printf("adreno-sdm845: step 9 could not create a write-combined CPU mapping for HFI memory\n");
			return false;
		}
		polaris_hfi_allocation = allocation;
		polaris_hfi_allocation_size = allocation_size;
		polaris_hfi_uncached = uncached;
		polaris_hfi_physical = phys;
	}
	if (polaris_hfi_uncached == 0 || polaris_hfi_allocation_size < A630_GMU_SHARED_BYTES ||
	    !polaris_a630_hfi_init(&polaris_hfi_state, (volatile void *)polaris_hfi_uncached,
	    polaris_hfi_allocation_size, POLARIS_A630_HFI_GMU_KERNEL_BASE)) {
		printf("adreno-sdm845: step 9 A630 HFI table initialization failed\n");
		return false;
	}
	if (!polaris_hfi_space_mapped) {
		for (uint32_t region = 0; region < sizeof(gmu_address) / sizeof(gmu_address[0]); region++) {
			for (uint64_t offset = 0; offset < region_bytes[region]; offset += POLARIS_SMMU_TABLE_BYTES) {
				if (!polaris_smmu_space_map_page(space, gmu_address[region] + offset,
				    polaris_hfi_physical + physical_offset[region] + offset, false)) {
					for (uint32_t rollback = region + 1; rollback > 0; rollback--) {
						uint32_t previous = rollback - 1;
						if (mapped[previous] != 0) {
							polaris_smmu_space_unmap(space, gmu_address[previous], mapped[previous]);
						}
					}
					printf("adreno-sdm845: step 9 could not construct the GMU-kernel HFI/dump/log mappings\n");
					return false;
				}
				mapped[region] += POLARIS_SMMU_TABLE_BYTES;
			}
		}
		sdm845_gpu_mapped();
		polaris_hfi_space_mapped = true;
	}
	if (!polaris_smmu_context_image_build(gpu_space, &gpu_context_image) ||
	    !polaris_smmu_context_image_build(space, &context_image) ||
	    !polaris_smmu_context_image_build(user_space, &user_context_image)) {
		printf("adreno-sdm845: step 9 cannot build AArch64 SMMUv2 context images for the detached GPU/GMU roots\n");
		return false;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("adreno-sdm845: step 9 GMU-kernel software maps: HFI=%#x/%u bytes, dump=%#x/%u bytes, log=%#x/%u bytes; physical=%#llx; detached context CBAR=%#x CBA2R=%#x TCR=%#x TCR2=%#x TTBR0=%#llx TTBR1=%#llx MAIR0=%#x SCTLR=%#x; no SMMU or GMU registers were written\n",
	    POLARIS_A630_HFI_GMU_KERNEL_BASE, (uint32_t)POLARIS_A630_HFI_ALLOCATION_BYTES,
	    A630_GMU_DUMP_VA, A630_GMU_HFI_DUMP_BYTES, A630_GMU_LOG_VA, A630_GMU_LOG_BYTES,
	    polaris_hfi_physical, context_image.cbar, context_image.cba2r,
	    context_image.tcr, context_image.tcr2, context_image.ttbr0, context_image.ttbr1,
	    context_image.mair0, context_image.sctlr);
	printf("adreno-sdm845: step 9 GMU-user detached empty context: ASID=%u CBAR=%#x CBA2R=%#x TCR=%#x TCR2=%#x TTBR0=%#llx; no SMMU register was written\n",
	    user_space->asid, user_context_image.cbar, user_context_image.cba2r, user_context_image.tcr,
	    user_context_image.tcr2, user_context_image.ttbr0);
	printf("adreno-sdm845: step 9 3D-user detached context root: ASID=%u CBAR=%#x CBA2R=%#x TCR=%#x TCR2=%#x TTBR0=%#llx; no SMMU register was written\n",
	    gpu_space->asid, gpu_context_image.cbar, gpu_context_image.cba2r, gpu_context_image.tcr,
	    gpu_context_image.tcr2, gpu_context_image.ttbr0);
	return true;
}

/* GPUCC accesses can stall when its configuration AHB branch is halted. */
static bool
probe_gpucc_bus(const char *step)
{
	uint32_t cbcr;

	if (!map_block(&gcc_gpu_clocks)) {
		return false;
	}
	cbcr = probe_read(&gcc_gpu_clocks, GCC_GPU_CFG_AHB_CBCR);
	if (cbcr & CBCR_OFF) {
		printf("adreno-sdm845: %s: GCC GPU config AHB is halted (CBCR=%#x); GPUCC registers are not read\n",
		    step, cbcr);
		return false;
	}
	return true;
}

static bool
probe_gpucc(void)
{
	uint32_t cx, gx, gmu_clock, cxo_clock, rcgr, rcgr_cfg, pll1;

	if (!probe_gpucc_bus("step 1") || !map_block(&gpucc)) {
		return false;
	}
	printf("adreno-sdm845: step 1: read SDM845 GPUCC state (no writes)\n");
	cx = probe_read(&gpucc, GPUCC_CX_GDSCR);
	gx = probe_read(&gpucc, GPUCC_GX_GDSCR);
	gmu_clock = probe_read(&gpucc, GPUCC_CX_GMU_CBCR);
	cxo_clock = probe_read(&gpucc, GPUCC_CXO_CBCR);
	rcgr = probe_read(&gpucc, GPUCC_GMU_RCGR);
	rcgr_cfg = probe_read(&gpucc, GPUCC_GMU_RCGR_CFG);
	pll1 = probe_read(&gpucc, GPUCC_PLL1_MODE);
	printf("adreno-sdm845: CX GDSCR=%#x, GX GDSCR=%#x\n", cx, gx);
	printf("adreno-sdm845: CX GMU CBCR=%#x, CXO CBCR=%#x, GMU RCGR=%#x/%#x, PLL1 MODE=%#x\n",
	    gmu_clock, cxo_clock, rcgr, rcgr_cfg, pll1);
	return true;
}

static const char *
polaris_smmu_stream_name(uint32_t stream_id)
{
	switch (stream_id) {
	case 0: return "3D user";
	case 1: return "3D secure A";
	case 2: return "3D secure B";
	case 4: return "GMU user";
	case 5: return "GMU kernel";
	default: return "unlabelled";
	}
}

static const char *
polaris_smmu_s2cr_type_name(uint32_t type)
{
	switch (type) {
	case SMMU_S2CR_TYPE_TRANS: return "translation";
	case SMMU_S2CR_TYPE_BYPASS: return "bypass";
	case SMMU_S2CR_TYPE_FAULT: return "fault";
	default: return "reserved";
	}
}

static const char *
polaris_smmu_cbar_type_name(uint32_t type)
{
	switch (type) {
	case 0: return "stage-2";
	case SMMU_CBAR_TYPE_S1_S2_BYPASS: return "stage-1/S2-bypass";
	case 2: return "stage-1/S2-fault";
	case 3: return "stage-1/S2-translation";
	default: return "reserved";
	}
}

static void
polaris_smmu_routes_reset(void)
{
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		polaris_smmu_stream_routes[i].stream_id = polaris_smmu_stream_ids[i];
		polaris_smmu_stream_routes[i].slot = UINT32_MAX;
		polaris_smmu_stream_routes[i].smr = 0;
		polaris_smmu_stream_routes[i].s2cr = 0;
		polaris_smmu_stream_routes[i].matches = 0;
	}
	memset(polaris_smmu_context_routed, 0, sizeof(polaris_smmu_context_routed));
	memset(polaris_smmu_context_blank, 0, sizeof(polaris_smmu_context_blank));
	polaris_smmu_context_snapshot_valid = false;
	polaris_smmu_route_scan_complete = false;
	polaris_smmu_context_page_size = 0;
	polaris_smmu_context_base = 0;
	polaris_smmu_context_bank_count = 0;
	polaris_smmu_oas_bits = 0;
	polaris_smmu_oas_encoding = 0;
	polaris_smmu_supports_4k = false;
	polaris_smmu_coherent_walk = false;
	polaris_smmu_client_enabled = false;
	polaris_smmu_free_smr_count = 0;
	memset(&polaris_smmu_gmu_kernel_route, 0, sizeof(polaris_smmu_gmu_kernel_route));
}

static void
polaris_smmu_route_record(uint32_t stream_id, uint32_t slot, uint32_t smr, uint32_t s2cr)
{
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		struct polaris_smmu_stream_route *route = &polaris_smmu_stream_routes[i];

		if (route->stream_id != stream_id) {
			continue;
		}
		if (route->matches == 0) {
			route->slot = slot;
			route->smr = smr;
			route->s2cr = s2cr;
		}
		route->matches++;
	}
}

static void
polaris_smmu_note_translation_route(uint32_t s2cr)
{
	uint32_t type = (s2cr >> SMMU_S2CR_TYPE_SHIFT) & SMMU_S2CR_TYPE_MASK;
	uint32_t context_bank = s2cr & SMMU_S2CR_CBNDX_MASK;

	if (type == SMMU_S2CR_TYPE_TRANS && context_bank < POLARIS_SMMU_MAX_CONTEXT_BANKS) {
		polaris_smmu_context_routed[context_bank] = true;
	}
}

static bool
polaris_smmu_route_context(uint32_t context_bank, uint32_t *cbar_out, uint32_t *cba2r_out,
	uint32_t *sctlr_out)
{
	uint64_t context_offset;

	if (smmu.virtual == 0 || polaris_smmu_context_page_size == 0 ||
	    context_bank >= polaris_smmu_context_bank_count) {
		return false;
	}
	context_offset = (uint64_t)polaris_smmu_context_base +
	    (uint64_t)context_bank * polaris_smmu_context_page_size;
	if (context_offset + polaris_smmu_context_page_size > smmu.size || context_offset > UINT32_MAX) {
		return false;
	}
	*cbar_out = probe_read(&smmu, SMMU_CBAR(context_bank));
	*cba2r_out = probe_read(&smmu, SMMU_CBA2R(context_bank));
	*sctlr_out = probe_read(&smmu, (uint32_t)context_offset + SMMU_CB_SCTLR);
	return true;
}

static void
polaris_smmu_routes_report(void)
{
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		const struct polaris_smmu_stream_route *route = &polaris_smmu_stream_routes[i];
		uint32_t type;

		if (route->matches == 0) {
			printf("adreno-sdm845: SMMU SID %u (%s): no active stream route matched\n", route->stream_id,
			    polaris_smmu_stream_name(route->stream_id));
			continue;
		}
		if (route->matches != 1) {
			printf("adreno-sdm845: SMMU SID %u (%s): %u SMR entries match; route is ambiguous\n",
			    route->stream_id, polaris_smmu_stream_name(route->stream_id), route->matches);
			continue;
		}
		type = (route->s2cr >> SMMU_S2CR_TYPE_SHIFT) & SMMU_S2CR_TYPE_MASK;
		printf("adreno-sdm845: SMMU SID %u (%s): slot %u routes to %s",
		    route->stream_id, polaris_smmu_stream_name(route->stream_id), route->slot,
		    polaris_smmu_s2cr_type_name(type));
		if (type == SMMU_S2CR_TYPE_TRANS) {
			uint32_t context_bank = route->s2cr & SMMU_S2CR_CBNDX_MASK;
			uint32_t cbar, cba2r, sctlr;

			if (!polaris_smmu_route_context(context_bank, &cbar, &cba2r, &sctlr)) {
				printf(" via CB%u; context-bank registers are unavailable\n", context_bank);
				continue;
			}
			printf(" via CB%u (CBAR %s VMID=%u, CBA2R=%#x%s, SCTLR=%#x%s)\n", context_bank,
			    polaris_smmu_cbar_type_name((cbar >> SMMU_CBAR_TYPE_SHIFT) & SMMU_CBAR_TYPE_MASK),
			    cbar & 0xffu, cba2r, (cba2r & SMMU_CBA2R_RW64) ? " 64-bit" : " 32-bit", sctlr,
			    (sctlr & SMMU_SCTLR_M) ? " translation-on" : " translation-off");
		} else {
			printf(" (S2CR=%#x)\n", route->s2cr);
		}
	}
}

static void
probe_smmu_contexts(uint32_t idr1)
{
	uint32_t page_shift = (idr1 & SMMU_IDR1_PAGESIZE_64K) ? 16u : 12u;
	uint32_t page_size = 1u << page_shift;
	uint32_t num_page_index_bits = (idr1 >> SMMU_IDR1_NUMPAGENDXB_SHIFT) & SMMU_IDR1_NUMPAGENDXB_MASK;
	uint32_t context_banks = idr1 & SMMU_IDR1_NUMCB_MASK;
	uint64_t global_pages = 1ull << (num_page_index_bits + 1u);
	uint64_t context_base = global_pages * page_size;
	uint64_t context_end = context_base + (uint64_t)page_size * context_banks;
	uint32_t active = 0;

	polaris_smmu_context_page_size = 0;
	polaris_smmu_context_base = 0;
	polaris_smmu_context_bank_count = 0;
	/* IDR1 sizes the split global/context-bank register apertures; never probe beyond DT reg. */
	if (context_banks == 0 || (uint64_t)SMMU_CB_TLBSTATUS + sizeof(uint32_t) > page_size ||
	    context_base > smmu.size || context_end > smmu.size ||
	    (context_banks != 0 &&
	    (SMMU_CBAR(context_banks - 1) + sizeof(uint32_t) > context_base ||
	    SMMU_CBA2R(context_banks - 1) + sizeof(uint32_t) > context_base))) {
		printf("adreno-sdm845: SMMU context-bank window is outside DT reg (%#llx bytes); context registers are not read\n",
		    (uint64_t)smmu.size);
		return;
	}
	polaris_smmu_context_page_size = page_size;
	polaris_smmu_context_base = (uint32_t)context_base;
	polaris_smmu_context_bank_count = context_banks;
	polaris_smmu_context_snapshot_valid = context_banks <= POLARIS_SMMU_MAX_CONTEXT_BANKS;

	for (uint32_t i = 0; i < context_banks; i++) {
		uint32_t cbar = probe_read(&smmu, SMMU_CBAR(i));
		uint32_t cba2r = probe_read(&smmu, SMMU_CBA2R(i));
		uint32_t base = (uint32_t)context_base + page_size * i;
		uint32_t sctlr = probe_read(&smmu, base + SMMU_CB_SCTLR);
		uint32_t actlr = probe_read(&smmu, base + SMMU_CB_ACTLR);
		uint32_t ttbr0_lo = probe_read(&smmu, base + SMMU_CB_TTBR0);
		uint32_t ttbr0_hi = probe_read(&smmu, base + SMMU_CB_TTBR0 + 4u);
		uint32_t ttbr1_lo = probe_read(&smmu, base + SMMU_CB_TTBR1);
		uint32_t ttbr1_hi = probe_read(&smmu, base + SMMU_CB_TTBR1 + 4u);
		uint32_t tcr = probe_read(&smmu, base + SMMU_CB_TCR);
		uint32_t tcr2 = probe_read(&smmu, base + SMMU_CB_TCR2);
		uint32_t contextidr = probe_read(&smmu, base + SMMU_CB_CONTEXTIDR);
		uint32_t mair0 = probe_read(&smmu, base + SMMU_CB_MAIR0);
		uint32_t mair1 = probe_read(&smmu, base + SMMU_CB_MAIR1);
		uint32_t fsr = probe_read(&smmu, base + SMMU_CB_FSR);

		if (cbar == 0 && cba2r == 0 && sctlr == 0 && actlr == 0 && ttbr0_lo == 0 && ttbr0_hi == 0 &&
		    ttbr1_lo == 0 && ttbr1_hi == 0 && tcr == 0 && tcr2 == 0 && contextidr == 0 && mair0 == 0 &&
		    mair1 == 0 && fsr == 0) {
			if (i < POLARIS_SMMU_MAX_CONTEXT_BANKS) {
				polaris_smmu_context_blank[i] = true;
			}
			continue;
		}
		active++;
		printf("adreno-sdm845: SMMU CB%u: CBAR=%#x CBA2R=%#x SCTLR=%#x%s ACTLR=%#x "
		    "TTBR0=%#x%08x TTBR1=%#x%08x TCR=%#x TCR2=%#x CONTEXTIDR=%#x MAIR=%#x/%#x FSR=%#x\n",
		    i, cbar, cba2r, sctlr, (sctlr & SMMU_SCTLR_M) ? " (translation on)" : "", actlr,
		    ttbr0_hi, ttbr0_lo, ttbr1_hi, ttbr1_lo, tcr, tcr2, contextidr, mair0, mair1, fsr);
	}
	printf("adreno-sdm845: SMMU context-bank window: page=%u bytes, base=%#llx, %u banks, %u nonzero\n",
	    page_size, context_base, context_banks, active);
}

static struct polaris_smmu_stream_route *
polaris_smmu_find_route(uint32_t stream_id)
{
	for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
		if (polaris_smmu_stream_routes[i].stream_id == stream_id) {
			return &polaris_smmu_stream_routes[i];
		}
	}
	return NULL;
}

static bool
polaris_smmu_plan_route_slot(uint32_t stream_id, bool stream_matching, uint32_t sid_bits,
	const uint32_t *claimed_slots, uint32_t claimed_count, uint32_t *slot_out)
{
	struct polaris_smmu_stream_route *route = polaris_smmu_find_route(stream_id);
	uint32_t slot = UINT32_MAX;

	if (route == NULL || slot_out == NULL || route->matches > 1) {
		printf("adreno-sdm845: attach plan SID %u: route is unavailable or ambiguous\n", stream_id);
		return false;
	}
	if (route->matches == 1) {
		uint32_t type = (route->s2cr >> SMMU_S2CR_TYPE_SHIFT) & SMMU_S2CR_TYPE_MASK;

		if (type == SMMU_S2CR_TYPE_TRANS) {
			printf("adreno-sdm845: attach plan SID %u: an existing translation route must not be replaced\n",
			    stream_id);
			return false;
		}
		if (stream_matching && (((route->smr >> SMMU_SMR_MASK_SHIFT) & SMMU_SMR_MASK_MASK) != 0 ||
		    (route->smr & SMMU_SMR_ID_MASK) != stream_id)) {
			printf("adreno-sdm845: attach plan SID %u: existing SMR is broader than this stream; slot is not reused\n",
			    stream_id);
			return false;
		}
		slot = route->slot;
	} else if (stream_matching) {
		for (uint32_t i = 0; i < polaris_smmu_free_smr_count; i++) {
			bool claimed = false;

			for (uint32_t j = 0; j < claimed_count; j++) {
				if (claimed_slots[j] == polaris_smmu_free_smr_slots[i]) {
					claimed = true;
					break;
				}
			}
			if (!claimed) {
				slot = polaris_smmu_free_smr_slots[i];
				break;
			}
		}
		if (slot == UINT32_MAX) {
			printf("adreno-sdm845: attach plan SID %u: no invalid SMR slot is available\n", stream_id);
			return false;
		}
	} else {
		uint32_t sid_limit = sid_bits >= 16 ? 0x10000u : (1u << sid_bits);

		if (stream_id >= sid_limit || SMMU_S2CR(stream_id) + sizeof(uint32_t) > smmu.size) {
			printf("adreno-sdm845: attach plan SID %u: SID-indexed S2CR is outside the decoded register range\n",
			    stream_id);
			return false;
		}
		slot = stream_id;
	}
	for (uint32_t i = 0; i < claimed_count; i++) {
		if (claimed_slots[i] == slot) {
			printf("adreno-sdm845: attach plan SID %u: route slot %u is already assigned in this plan\n",
			    stream_id, slot);
			return false;
		}
	}
	*slot_out = slot;
	return true;
}

static void
polaris_smmu_plan_attach(bool stream_matching, uint32_t sid_bits)
{
	static const uint32_t plan_streams[] = { 0, 5, 4 };
	static const char *const plan_names[] = { "3D user", "GMU kernel", "GMU user" };
	uint32_t route_slots[sizeof(plan_streams) / sizeof(plan_streams[0])];
	uint32_t claimed_slots[sizeof(plan_streams) / sizeof(plan_streams[0])];
	uint32_t gmu_banks[2], gmu_bank_count = 0, route_count = 0;
	bool route_ok[sizeof(plan_streams) / sizeof(plan_streams[0])] = { false, false, false };
	bool gpu_cb0_candidate;
	struct polaris_smmu_stream_route *gpu_route, *kernel_route, *user_route;

	printf("adreno-sdm845: read-only SMMUv2 attach planner: GPU SID0 reserves Qualcomm CB0; secure GPU SIDs1/2 are excluded; HYP-protected CB5-7 are excluded\n");
	if (!polaris_smmu_client_enabled) {
		printf("adreno-sdm845: attach plan unavailable: SMMU sCR0 has the client interface disabled\n");
		return;
	}
	if (!polaris_smmu_dynamic_routes_safe) {
		printf("adreno-sdm845: attach plan unavailable: runtime DT has not authorized dynamic, non-static route planning\n");
		return;
	}
	if (!polaris_smmu_context_snapshot_valid || !polaris_smmu_route_scan_complete) {
		printf("adreno-sdm845: attach plan unavailable: context snapshot or complete stream-route scan is missing\n");
		return;
	}
	if (polaris_smmu_context_bank_count <= 1u) {
		printf("adreno-sdm845: attach plan unavailable: SMMU exposes no CB in the permitted GMU range CB1-4\n");
		return;
	}
	gpu_cb0_candidate = polaris_smmu_context_blank[0] && !polaris_smmu_context_routed[0];
	if (gpu_cb0_candidate) {
		printf("adreno-sdm845: attach plan: fixed GPU CB0 is clear in the sampled context registers and has no current translation route\n");
	} else {
		printf("adreno-sdm845: attach plan: fixed GPU CB0 is occupied or already routed; it is reserved and will not be reused by GMU\n");
	}
	for (uint32_t bank = 1; bank <= 4u && bank < polaris_smmu_context_bank_count; bank++) {
		if (polaris_smmu_context_blank[bank] && !polaris_smmu_context_routed[bank]) {
			if (gmu_bank_count < sizeof(gmu_banks) / sizeof(gmu_banks[0])) {
				gmu_banks[gmu_bank_count++] = bank;
			}
		}
	}
	if (gmu_bank_count == 2) {
		printf("adreno-sdm845: attach plan: GMU kernel/user candidate CBs are CB%u/CB%u (both sampled clear and unused by translation routes)\n",
		    gmu_banks[0], gmu_banks[1]);
	} else {
		printf("adreno-sdm845: attach plan: only %u clear, unrouted GMU context bank(s) found in CB1-4; two independent contexts are required\n",
		    gmu_bank_count);
	}
	for (uint32_t i = 0; i < sizeof(plan_streams) / sizeof(plan_streams[0]); i++) {
		uint32_t candidate_slot;

		route_ok[i] = polaris_smmu_plan_route_slot(plan_streams[i], stream_matching, sid_bits,
		    claimed_slots, route_count, &candidate_slot);
		if (route_ok[i]) {
			route_slots[i] = candidate_slot;
			claimed_slots[route_count] = candidate_slot;
			printf("adreno-sdm845: attach plan: %s SID%u may use %s slot %u\n", plan_names[i], plan_streams[i],
			    stream_matching ? "SMR/S2CR" : "SID-indexed S2CR", candidate_slot);
			route_count++;
		}
	}
	gpu_route = polaris_smmu_find_route(plan_streams[0]);
	if (gpu_cb0_candidate && route_ok[0] && gpu_route != NULL) {
		uint32_t expected_smr = 0;
		uint32_t expected_s2cr;
		bool existing_smr = stream_matching && gpu_route->matches == 1;

		if (existing_smr) {
			expected_smr = gpu_route->smr;
			expected_s2cr = gpu_route->s2cr;
		} else if (stream_matching) {
			expected_smr = probe_read(&smmu, SMMU_SMR(route_slots[0]));
			expected_s2cr = probe_read(&smmu, SMMU_S2CR(route_slots[0]));
			if (expected_smr & SMMU_SMR_VALID) {
				printf("adreno-sdm845: 3D-user candidate slot %u became valid during planning\n", route_slots[0]);
				return;
			}
		} else {
			expected_s2cr = gpu_route->s2cr;
		}
		polaris_smmu_gpu_route.valid = true;
		polaris_smmu_gpu_route.stream_matching = stream_matching;
		polaris_smmu_gpu_route.existing_smr = existing_smr;
		polaris_smmu_gpu_route.stream_id = plan_streams[0];
		polaris_smmu_gpu_route.slot = route_slots[0];
		polaris_smmu_gpu_route.context_bank = 0;
		polaris_smmu_gpu_route.expected_smr = expected_smr;
		polaris_smmu_gpu_route.expected_s2cr = expected_s2cr;
		printf("adreno-sdm845: 3D-user stage-1 candidate: SID%u -> fixed CB0 via %s slot %u%s\n",
		    plan_streams[0], stream_matching ? "SMR/S2CR" : "SID-indexed S2CR", route_slots[0],
		    existing_smr ? " (existing exact SMR)" : "");
	}
	kernel_route = polaris_smmu_find_route(plan_streams[1]);
	if (gmu_bank_count != 0 && route_ok[1] && kernel_route != NULL) {
		uint32_t expected_smr = 0;
		uint32_t expected_s2cr;
		bool existing_smr = stream_matching && kernel_route->matches == 1;

		if (existing_smr) {
			expected_smr = kernel_route->smr;
			expected_s2cr = kernel_route->s2cr;
		} else if (stream_matching) {
			expected_smr = probe_read(&smmu, SMMU_SMR(route_slots[1]));
			expected_s2cr = probe_read(&smmu, SMMU_S2CR(route_slots[1]));
			if (expected_smr & SMMU_SMR_VALID) {
				printf("adreno-sdm845: GMU-kernel candidate slot %u became valid during planning\n", route_slots[1]);
				return;
			}
		} else {
			expected_s2cr = kernel_route->s2cr;
		}
		polaris_smmu_gmu_kernel_route.valid = true;
		polaris_smmu_gmu_kernel_route.stream_matching = stream_matching;
		polaris_smmu_gmu_kernel_route.existing_smr = existing_smr;
		polaris_smmu_gmu_kernel_route.stream_id = plan_streams[1];
		polaris_smmu_gmu_kernel_route.slot = route_slots[1];
		polaris_smmu_gmu_kernel_route.context_bank = gmu_banks[0];
		polaris_smmu_gmu_kernel_route.expected_smr = expected_smr;
		polaris_smmu_gmu_kernel_route.expected_s2cr = expected_s2cr;
		printf("adreno-sdm845: GMU-kernel stage-1 candidate: SID%u -> CB%u via %s slot %u%s\n",
		    plan_streams[1], gmu_banks[0], stream_matching ? "SMR/S2CR" : "SID-indexed S2CR",
		    route_slots[1], existing_smr ? " (existing exact SMR)" : "");
	}
	user_route = polaris_smmu_find_route(plan_streams[2]);
	if (gmu_bank_count == 2 && route_ok[2] && user_route != NULL) {
		uint32_t expected_smr = 0;
		uint32_t expected_s2cr;
		bool existing_smr = stream_matching && user_route->matches == 1;

		if (existing_smr) {
			expected_smr = user_route->smr;
			expected_s2cr = user_route->s2cr;
		} else if (stream_matching) {
			expected_smr = probe_read(&smmu, SMMU_SMR(route_slots[2]));
			expected_s2cr = probe_read(&smmu, SMMU_S2CR(route_slots[2]));
			if (expected_smr & SMMU_SMR_VALID) {
				printf("adreno-sdm845: GMU-user candidate slot %u became valid during planning\n", route_slots[2]);
				return;
			}
		} else {
			expected_s2cr = user_route->s2cr;
		}
		polaris_smmu_gmu_user_route.valid = true;
		polaris_smmu_gmu_user_route.stream_matching = stream_matching;
		polaris_smmu_gmu_user_route.existing_smr = existing_smr;
		polaris_smmu_gmu_user_route.stream_id = plan_streams[2];
		polaris_smmu_gmu_user_route.slot = route_slots[2];
		polaris_smmu_gmu_user_route.context_bank = gmu_banks[1];
		polaris_smmu_gmu_user_route.expected_smr = expected_smr;
		polaris_smmu_gmu_user_route.expected_s2cr = expected_s2cr;
		printf("adreno-sdm845: GMU-user stage-1 candidate: SID%u -> CB%u via %s slot %u%s\n",
		    plan_streams[2], gmu_banks[1], stream_matching ? "SMR/S2CR" : "SID-indexed S2CR",
		    route_slots[2], existing_smr ? " (existing exact SMR)" : "");
	}
	if (gpu_cb0_candidate && gmu_bank_count == 2 && route_ok[0] && route_ok[1] && route_ok[2]) {
		printf("adreno-sdm845: read-only full-plan candidate: GPU SID0->CB0; GMU kernel SID5->CB%u; GMU user SID4->CB%u; this planner performs no register writes\n",
		    gmu_banks[0], gmu_banks[1]);
	} else {
		printf("adreno-sdm845: no complete attach plan; no SMMU register was written\n");
	}
}

static uint32_t
smmu_id_size_to_bits(uint32_t encoding)
{
	switch (encoding) {
	case 0: return 32;
	case 1: return 36;
	case 2: return 40;
	case 3: return 42;
	case 4: return 44;
	default: return 48;
	}
}

static bool
probe_smmu(void)
{
	uint32_t cx, memnoc, scr0, idr0, idr1, idr2, idr7;
	uint32_t stream_groups, context_banks, s2_context_banks, sid_bits, sid_mask;
	uint32_t page_shift, major, minor;

	polaris_smmu_routes_reset();
	if (!probe_gpucc_bus("step 2") || !map_block(&gpucc)) {
		return false;
	}
	cx = probe_read(&gpucc, GPUCC_CX_GDSCR);
	if (!(cx & GDSC_POWER_ON)) {
		printf("adreno-sdm845: step 2: CX is off (GDSCR=%#x); SMMU registers are not read\n", cx);
		return true;
	}
	memnoc = probe_read(&gcc_gpu_clocks, GCC_GPU_MEMNOC_GFX_CBCR);
	if (memnoc & CBCR_OFF) {
		printf("adreno-sdm845: step 2: GPU MEMNOC GFX clock is halted (CBCR=%#x); SMMU registers are not read\n",
		    memnoc);
		return true;
	}
	if (!map_block(&smmu)) {
		return false;
	}
	printf("adreno-sdm845: step 2: CX and GPU MEMNOC are on; read SMMUv2 identity at %#llx (no writes)\n",
	    (uint64_t)smmu.physical);
	idr0 = probe_read(&smmu, SMMU_IDR0);
	idr1 = probe_read(&smmu, SMMU_IDR1);
	idr2 = probe_read(&smmu, SMMU_IDR2);
	idr7 = probe_read(&smmu, SMMU_IDR7);
	scr0 = probe_read(&smmu, SMMU_SCR0);
	polaris_smmu_client_enabled = (scr0 & SMMU_SCR0_CLIENTPD) == 0;
	printf("adreno-sdm845: SMMU sCR0=%#x client interface=%s; IDR0=%#x IDR1=%#x IDR2=%#x IDR7=%#x\n",
	    scr0, polaris_smmu_client_enabled ? "enabled" : "disabled", idr0, idr1, idr2, idr7);
	stream_groups = idr0 & SMMU_IDR0_NUMSMRG_MASK;
	context_banks = idr1 & SMMU_IDR1_NUMCB_MASK;
	s2_context_banks = (idr1 >> SMMU_IDR1_NUMS2CB_SHIFT) & SMMU_IDR1_NUMCB_MASK;
	sid_bits = (idr0 & SMMU_IDR0_EXIDS) ? 16 :
	    ((idr0 >> SMMU_IDR0_NUMSIDB_SHIFT) & SMMU_IDR0_NUMSIDB_MASK);
	sid_mask = sid_bits >= 16 ? 0xffffu : ((1u << sid_bits) - 1u);
	page_shift = (idr1 & SMMU_IDR1_PAGESIZE_64K) ? 16 : 12;
	major = (idr7 >> SMMU_IDR7_MAJOR_SHIFT) & SMMU_IDR7_VERSION_MASK;
	minor = idr7 & SMMU_IDR7_VERSION_MASK;
	printf("adreno-sdm845: SMMU v%u.%u: stage1=%u stage2=%u stream matching=%u (%u groups), SID bits=%u, "
	    "context banks=%u (stage2-only=%u), context granule=%uK\n", major, minor,
	    (unsigned)!!(idr0 & SMMU_IDR0_S1TS), (unsigned)!!(idr0 & SMMU_IDR0_S2TS),
	    (unsigned)!!(idr0 & SMMU_IDR0_SMS), stream_groups,
	    sid_bits, context_banks, s2_context_banks, 1u << (page_shift - 10));
	printf("adreno-sdm845: SMMU page-walk: IAS/OAS/UBS=%u/%u/%u bits, encodings=%u/%u/%u, "
	    "4K/16K/64K tables=%u/%u/%u\n",
	    smmu_id_size_to_bits(idr2 & SMMU_IDR2_IAS_MASK),
	    smmu_id_size_to_bits((idr2 >> SMMU_IDR2_OAS_SHIFT) & SMMU_IDR2_SIZE_MASK),
	    smmu_id_size_to_bits((idr2 >> SMMU_IDR2_UBS_SHIFT) & SMMU_IDR2_SIZE_MASK),
	    idr2 & SMMU_IDR2_IAS_MASK, (idr2 >> SMMU_IDR2_OAS_SHIFT) & SMMU_IDR2_SIZE_MASK,
	    (idr2 >> SMMU_IDR2_UBS_SHIFT) & SMMU_IDR2_SIZE_MASK, (unsigned)!!(idr2 & SMMU_IDR2_PTFS_4K),
	    (unsigned)!!(idr2 & SMMU_IDR2_PTFS_16K), (unsigned)!!(idr2 & SMMU_IDR2_PTFS_64K));
	polaris_smmu_oas_bits = smmu_id_size_to_bits((idr2 >> SMMU_IDR2_OAS_SHIFT) & SMMU_IDR2_SIZE_MASK);
	polaris_smmu_oas_encoding = (idr2 >> SMMU_IDR2_OAS_SHIFT) & SMMU_IDR2_SIZE_MASK;
	polaris_smmu_supports_4k = (idr0 & SMMU_IDR0_S1TS) != 0 && (idr2 & SMMU_IDR2_PTFS_4K) != 0;
	polaris_smmu_coherent_walk = (idr0 & SMMU_IDR0_CTTW) != 0;
	printf("adreno-sdm845: SMMU stage-1 table prerequisites: 4K=%s, OAS=%u bits (encoding=%u), coherent table walk=%s\n",
	    polaris_smmu_supports_4k ? "supported" : "unavailable", polaris_smmu_oas_bits,
	    polaris_smmu_oas_encoding, polaris_smmu_coherent_walk ? "yes" : "no");
	if (idr0 & SMMU_IDR0_SMS) {
		if (stream_groups == 0) {
			printf("adreno-sdm845: SMMU advertises stream matching but reports no SMR groups\n");
		} else if (stream_groups > (smmu.size - SMMU_S2CR(0)) / sizeof(uint32_t)) {
			printf("adreno-sdm845: SMMU stream-match count %#x exceeds the mapped register window; entries are not read\n",
			    stream_groups);
		} else {
			for (uint32_t i = 0; i < stream_groups; i++) {
				uint32_t smr = probe_read(&smmu, SMMU_SMR(i));
				uint32_t s2cr, stream_id_field, stream_mask;

				if (!(smr & SMMU_SMR_VALID)) {
					polaris_smmu_free_smr_slots[polaris_smmu_free_smr_count++] = i;
					continue;
				}
				s2cr = probe_read(&smmu, SMMU_S2CR(i));
				polaris_smmu_note_translation_route(s2cr);
				stream_id_field = smr & SMMU_SMR_ID_MASK;
				stream_mask = (smr >> SMMU_SMR_MASK_SHIFT) & SMMU_SMR_MASK_MASK;
				printf("adreno-sdm845: SMMU SMR[%u]=%#x S2CR=%#x type=%u context-bank=%u\n", i, smr, s2cr,
				    (s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u, s2cr & SMMU_S2CR_CBNDX_MASK);

				for (uint32_t j = 0; j < polaris_smmu_stream_count; j++) {
					uint32_t stream_id = polaris_smmu_stream_ids[j];

					if (((stream_id ^ stream_id_field) & sid_mask & ~stream_mask) == 0) {
						polaris_smmu_route_record(stream_id, i, smr, s2cr);
					}
				}
			}
			polaris_smmu_route_scan_complete = true;
			printf("adreno-sdm845: SMMU stream-match scan complete: %u invalid SMR slot(s) available\n",
			    polaris_smmu_free_smr_count);
		}
	} else if (sid_bits <= 16) {
		uint32_t sid_limit = sid_bits >= 16u ? 0x10000u : (1u << sid_bits);
		uint32_t s2cr_slots = (smmu.size - SMMU_S2CR(0)) / sizeof(uint32_t);
		uint32_t scan_count = sid_limit < s2cr_slots ? sid_limit : s2cr_slots;

		if (sid_limit > s2cr_slots) {
			printf("adreno-sdm845: SID-indexed S2CR table has %u entries but IDR0 describes %u SIDs; complete route scan is unavailable\n",
			    s2cr_slots, sid_limit);
		} else {
			polaris_smmu_route_scan_complete = true;
		}
		for (uint32_t sid = 0; sid < scan_count; sid++) {
			uint32_t s2cr = probe_read(&smmu, SMMU_S2CR(sid));

			polaris_smmu_note_translation_route(s2cr);
			for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
				if (polaris_smmu_stream_ids[i] == sid) {
					printf("adreno-sdm845: SMMU SID %u S2CR=%#x type=%u context-bank=%u\n", sid, s2cr,
					    (s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u, s2cr & SMMU_S2CR_CBNDX_MASK);
					polaris_smmu_route_record(sid, sid, 0, s2cr);
				}
			}
		}
	}
	probe_smmu_contexts(idr1);
	polaris_smmu_routes_report();
	polaris_smmu_plan_attach((idr0 & SMMU_IDR0_SMS) != 0, sid_bits);
	return true;
}

static bool
probe_gmu(void)
{
	uint32_t cx, clock;
	uint32_t high1 = 0, high2 = 0, low = 0, init_result, hfi_version;
	bool counter_stable = false;

	polaris_gmu_domain_ready = false;
	if (!polaris_a630_hfi_layout_valid()) {
		printf("adreno-sdm845: step 3: A630 legacy HFI layout is inconsistent; GMU access is refused\n");
		return false;
	}
	printf("adreno-sdm845: step 3: A630 HFI v1 layout: %u-byte allocation, %u-dword queues, GMU VA %#x, legacy IDs cmd/msg/debug/dispatch=0/4/5/1; transport is not started\n",
	    (uint32_t)POLARIS_A630_HFI_ALLOCATION_BYTES, (uint32_t)POLARIS_A630_HFI_QUEUE_DWORDS,
	    POLARIS_A630_HFI_GMU_KERNEL_BASE);
	if (!probe_gpucc_bus("step 3") || !map_block(&gpucc)) {
		return false;
	}
	cx = probe_read(&gpucc, GPUCC_CX_GDSCR);
	if (!(cx & GDSC_POWER_ON)) {
		printf("adreno-sdm845: step 3: CX is off (GDSCR=%#x); GMU registers are not read\n", cx);
		return true;
	}
	clock = probe_read(&gpucc, GPUCC_CX_GMU_CBCR);
	if ((clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno-sdm845: step 3: CX GMU clock is not running (CBCR=%#x); GMU registers are not read\n",
		    clock);
		return true;
	}
	polaris_gmu_domain_ready = true;
	if (!map_block(&gmu)) {
		return false;
	}
	printf("adreno-sdm845: step 3: CX is powered; read A6xx GMU state (no writes)\n");
	for (unsigned attempt = 0; attempt < 4; attempt++) {
		high1 = probe_read(&gmu, GMU_ALWAYS_ON_COUNTER_H);
		low = probe_read(&gmu, GMU_ALWAYS_ON_COUNTER_L);
		high2 = probe_read(&gmu, GMU_ALWAYS_ON_COUNTER_H);
		if (high1 == high2) {
			counter_stable = true;
			break;
		}
	}
	init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
	hfi_version = probe_read(&gmu, GMU_HFI_VERSION_INFO);
	if (counter_stable) {
		printf("adreno-sdm845: GMU counter=%#llx, firmware-init=%#x, HFI version=%#x\n",
		    ((uint64_t)high2 << 32) | low, init_result, hfi_version);
	} else {
		printf("adreno-sdm845: GMU counter did not stabilize; firmware-init=%#x, HFI version=%#x\n",
		    init_result, hfi_version);
	}
	return true;
}

static bool
probe_core(void)
{
	uint32_t gx, clock;
	uint32_t status, interrupts, fault;

	polaris_gpu_core_ready = false;
	if (!probe_gpucc_bus("step 4") || !map_block(&gpucc)) {
		return false;
	}
	gx = probe_read(&gpucc, GPUCC_GX_GDSCR);
	if (!(gx & GDSC_POWER_ON)) {
		printf("adreno-sdm845: step 4: GX is off (GDSCR=%#x); 3D registers are not read\n", gx);
		return true;
	}
	clock = probe_read(&gpucc, GPUCC_GX_GFX3D_CBCR);
	if ((clock & (CBCR_ENABLE | CBCR_OFF)) != CBCR_ENABLE) {
		printf("adreno-sdm845: step 4: GFX3D clock is not running (CBCR=%#x); 3D registers are not read\n",
		    clock);
		return true;
	}
	polaris_gpu_core_ready = true;
	if (!map_block(&gpu)) {
		return false;
	}
	printf("adreno-sdm845: step 4: GX is powered; read A6xx core status (no writes)\n");
	status = probe_read(&gpu, GPU_RBBM_STATUS);
	interrupts = probe_read(&gpu, GPU_RBBM_INT_0_STATUS);
	fault = probe_read(&gpu, GPU_CP_HW_FAULT);
	printf("adreno-sdm845: RBBM_STATUS=%#x RBBM_INT_0_STATUS=%#x CP_HW_FAULT=%#x\n", status,
	    interrupts, fault);
	return true;
}

#define POLARIS_SCM_FAST              (1ull << 31)
#define POLARIS_SCM_64                (1ull << 30)
#define POLARIS_SCM_SIP               (2ull << 24)
#define POLARIS_SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define POLARIS_SCM_IS_CALL_AVAILABLE POLARIS_SCM_CALL(6, 1)
#define POLARIS_SCM_PAS_INIT_IMAGE    POLARIS_SCM_CALL(2, 1)
#define POLARIS_SCM_PAS_MEM_SETUP     POLARIS_SCM_CALL(2, 2)
#define POLARIS_SCM_PAS_AUTH_RESET    POLARIS_SCM_CALL(2, 5)
#define POLARIS_SCM_PAS_SHUTDOWN      POLARIS_SCM_CALL(2, 6)
#define POLARIS_SCM_PAS_IS_SUPPORTED  POLARIS_SCM_CALL(2, 7)
#define POLARIS_SCM_ONE_VALUE         1u
#define POLARIS_SCM_VALUE_AND_BUFFER  (2u | 2u << 6)
#define POLARIS_SCM_THREE_VALUES      3u
#define POLARIS_SCM_INTERRUPTED       1u

static uint16_t
read_le16(const uint8_t *data)
{
	return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t
read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static bool
polaris_cmd_db_entry_lookup(const char *name, struct polaris_cmd_db_entry *resource)
{
	char resource_id[8] = { 0 };
	static const uint8_t magic[] = { 0xdb, 0x30, 0x03, 0x0c };

	if (name == NULL || resource == NULL || strlen(name) > sizeof(resource_id)) {
		return false;
	}
	if (polaris_cmd_db == NULL) {
		vm_offset_t mapping = ml_io_map_wcomb(POLARIS_CMD_DB_PHYS, POLARIS_CMD_DB_SIZE);

		if (mapping == 0) {
			printf("adreno-sdm845: cannot map the validated AOP CMD_DB window\n");
			return false;
		}
		polaris_cmd_db = (const uint8_t *)mapping;
	}
	if (memcmp(polaris_cmd_db + 4, magic, sizeof(magic)) != 0) {
		printf("adreno-sdm845: AOP CMD_DB magic is invalid\n");
		return false;
	}
	memcpy(resource_id, name, strlen(name));
	memset(resource, 0, sizeof(*resource));

	for (uint32_t slave_index = 0; slave_index < POLARIS_CMD_DB_SLAVE_COUNT; slave_index++) {
		const uint8_t *slave = polaris_cmd_db + 8u + slave_index * POLARIS_CMD_DB_SLAVE_SIZE;
		uint32_t slave_id = read_le16(slave);
		uint32_t header_offset = read_le16(slave + 2);
		uint32_t data_offset = read_le16(slave + 4);
		uint32_t entry_count = read_le16(slave + 6);
		uint64_t entry_bytes = (uint64_t)entry_count * POLARIS_CMD_DB_ENTRY_SIZE;

		if (slave_id == 0) {
			continue;
		}
		if (entry_count > POLARIS_CMD_DB_MAX_ENTRIES || header_offset > POLARIS_CMD_DB_SIZE - POLARIS_CMD_DB_HEADER_SIZE ||
		    entry_bytes > POLARIS_CMD_DB_SIZE - POLARIS_CMD_DB_HEADER_SIZE - header_offset ||
		    data_offset > POLARIS_CMD_DB_SIZE - POLARIS_CMD_DB_HEADER_SIZE) {
			printf("adreno-sdm845: AOP CMD_DB slave %u has out-of-range entry/data offsets\n", slave_index);
			return false;
		}
		for (uint32_t entry_index = 0; entry_index < entry_count; entry_index++) {
			const uint8_t *entry = polaris_cmd_db + POLARIS_CMD_DB_HEADER_SIZE + header_offset +
			    entry_index * POLARIS_CMD_DB_ENTRY_SIZE;
			uint32_t aux_length, aux_offset;
			uint64_t aux_start;

			if (memcmp(entry, resource_id, sizeof(resource_id)) != 0) {
				continue;
			}
			aux_length = read_le16(entry + 20);
			aux_offset = read_le16(entry + 22);
			aux_start = (uint64_t)POLARIS_CMD_DB_HEADER_SIZE + data_offset + aux_offset;
			if (aux_length == 0 || aux_start > POLARIS_CMD_DB_SIZE ||
			    aux_length > POLARIS_CMD_DB_SIZE - aux_start) {
				printf("adreno-sdm845: CMD_DB resource %s has invalid auxiliary metadata\n", name);
				return false;
			}
			resource->address = read_le32(entry + 16);
			resource->auxiliary = polaris_cmd_db + aux_start;
			resource->auxiliary_length = aux_length;
			return resource->address != 0;
		}
	}
	return false;
}

static bool
polaris_cmd_db_resource_lookup(const char *name, struct polaris_cmd_db_resource *resource)
{
	struct polaris_cmd_db_entry entry;
	uint32_t level_count;

	if (resource == NULL || !polaris_cmd_db_entry_lookup(name, &entry) || entry.auxiliary_length == 0 ||
	    (entry.auxiliary_length & 1u) != 0 || entry.auxiliary_length > POLARIS_CMD_DB_MAX_LEVELS * sizeof(uint16_t)) {
		if (name != NULL) {
			printf("adreno-sdm845: CMD_DB ARC resource %s has invalid level metadata\n", name);
		}
		return false;
	}
	memset(resource, 0, sizeof(*resource));
	resource->address = entry.address;
	level_count = entry.auxiliary_length / sizeof(uint16_t);
	for (uint32_t level_index = 0; level_index < level_count; level_index++) {
		uint16_t level = read_le16(entry.auxiliary + level_index * sizeof(uint16_t));

		if (level_index != 0 && level == 0) {
			break;
		}
		if (level == 0 || (level_index != 0 && level <= resource->levels[level_index - 1u])) {
			printf("adreno-sdm845: CMD_DB resource %s levels are zero or not strictly ascending\n", name);
			return false;
		}
		resource->levels[level_index] = level;
		resource->level_count++;
	}
	return resource->address != 0 && resource->level_count != 0 && resource->levels[0] != 0;
}

static bool
polaris_cmd_db_bcm_lookup(const char *name, struct polaris_cmd_db_bcm *bcm)
{
	struct polaris_cmd_db_entry entry;

	if (name == NULL || bcm == NULL || !polaris_cmd_db_entry_lookup(name, &entry) || entry.auxiliary_length != 8u) {
		printf("adreno-sdm845: CMD_DB BCM %s has no 8-byte BCM auxiliary record\n", name != NULL ? name : "(null)");
		return false;
	}
	bcm->address = entry.address;
	bcm->unit_size = read_le32(entry.auxiliary);
	bcm->width = read_le16(entry.auxiliary + 4);
	bcm->clock_domain = entry.auxiliary[6];
	if (bcm->address == 0 || (bcm->address & 3u) != 0 || bcm->unit_size == 0 || bcm->width == 0 ||
	    bcm->clock_domain >= 16u || entry.auxiliary[7] != 0) {
		printf("adreno-sdm845: CMD_DB BCM %s has invalid address, unit size, width, or clock domain\n", name);
		return false;
	}
	return true;
}

/* RPMH_REGULATOR_LEVEL_* encodings from the SDM845 clock-controller binding. */
static bool
polaris_gpu_voltage_corner(uint32_t frequency_hz, uint16_t *corner)
{
	if (corner == NULL) {
		return false;
	}
	switch (frequency_hz) {
	case 0:
		*corner = 0;
		return true;
	case 180000000u: *corner = 48u; return true;  /* VDD_GX_MIN */
	case 257000000u: *corner = 64u; return true;  /* VDD_GX_LOWER */
	case 342000000u: *corner = 128u; return true; /* VDD_GX_LOW */
	case 414000000u: *corner = 192u; return true; /* VDD_GX_LOW_L1 */
	case 520000000u: *corner = 256u; return true; /* VDD_GX_NOMINAL */
	case 596000000u: *corner = 320u; return true; /* VDD_GX_NOMINAL_L1 */
	case 675000000u: *corner = 384u; return true; /* VDD_GX_HIGH */
	case 710000000u: *corner = 416u; return true; /* VDD_GX_HIGH_L1 */
	default:
		return false;
	}
}

static bool
polaris_gmu_voltage_corner(uint32_t frequency_hz, uint16_t *corner)
{
	if (corner == NULL) {
		return false;
	}
	switch (frequency_hz) {
	case 0:
		*corner = 0;
		return true;
	case 200000000u: *corner = 48u; return true; /* VDD_CX_MIN */
	case 500000000u: *corner = 128u; return true; /* VDD_CX_LOW */
	default:
		return false;
	}
}

/* Match KGSL's RPMh ARC selection: first primary level >= corner, then a dependent MX level. */
static bool
polaris_cmd_db_arc_vote(const struct polaris_cmd_db_resource *primary,
	const struct polaris_cmd_db_resource *secondary, uint16_t corner, uint32_t *vote)
{
	uint32_t primary_index = 0, secondary_index = 0;
	bool primary_found = false;

	if (primary == NULL || secondary == NULL || vote == NULL || primary->level_count == 0 ||
	    secondary->level_count == 0 || primary->level_count > UINT8_MAX || secondary->level_count > UINT8_MAX) {
		return false;
	}
	for (uint32_t i = 0; i < primary->level_count; i++) {
		if (primary->levels[i] >= corner) {
			primary_index = i;
			primary_found = true;
			break;
		}
	}
	if (!primary_found) {
		return false;
	}
	for (uint32_t i = 0; i < secondary->level_count; i++) {
		if (secondary->levels[i] >= primary->levels[primary_index] || i + 1u == secondary->level_count) {
			secondary_index = i;
			break;
		}
	}
	*vote = ((uint32_t)primary->levels[primary_index] << 16) |
	    (secondary_index << 8) | primary_index;
	return true;
}

/* KGSL's OOB clear writes the GMU interrupt-set register with its acknowledgement mask. */
static void
polaris_gmu_oob_clear(uint32_t mask)
{
	if (gmu.virtual != 0 && mask != 0) {
		probe_write(&gmu, GMU_HOST2GMU_INTR_SET, mask);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

/* Hold GX awake while the A630 core and CP bootstrap are initialized. */
static bool
polaris_gmu_gpu_oob_request(void)
{
	bool acknowledged = false;

	if (!polaris_gmu_firmware_started || !polaris_gmu_hfi_ready || !polaris_gmu_hfi_sealed ||
	    !polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready ||
	    !polaris_gmu_boot_rail_active || polaris_gmu_temp_bus_vote_active || polaris_gpu_oob_active ||
	    polaris_gpu_core_ready || !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ||
	    gmu.virtual == 0) {
		printf("adreno-sdm845: step 17 requires a running GMU with acknowledged HFI tables, initial DCVS, and boot rail; GPU OOB was not sent\n");
		return false;
	}
	probe_write(&gmu, GMU_HOST2GMU_INTR_SET, A630_GMU_OOB_GPU_SET_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_QUEUE_START_POLLS; poll++) {
		if ((probe_read(&gmu, GMU_GMU2HOST_INTR_INFO) & A630_GMU_OOB_GPU_ACK_MASK) != 0) {
			acknowledged = true;
			break;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	probe_write(&gmu, GMU_GMU2HOST_INTR_CLR, A630_GMU_OOB_GPU_ACK_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!acknowledged) {
		polaris_gmu_oob_clear(A630_GMU_OOB_GPU_ACK_MASK);
		printf("adreno-sdm845: step 17 timed out waiting for the A630 GPU OOB acknowledgement\n");
		return false;
	}
	polaris_gpu_oob_active = true;
	return true;
}

static void
polaris_gmu_gpu_oob_release(void)
{
	if (polaris_gpu_oob_active) {
		polaris_gmu_oob_clear(A630_GMU_OOB_GPU_ACK_MASK);
		polaris_gpu_oob_active = false;
	}
}

/* Send KGSL's A630 boot/slumber OOB request with the DT-selected initial GX/MX ARC vote. */
static bool
polaris_gmu_boot_rail_request(void)
{
	struct polaris_cmd_db_resource gfx_level, mx_level;
	const struct polaris_gpu_operating_point *initial;
	uint32_t vote, gx_index, mx_index;
	uint16_t corner;
	bool acknowledged = false;

	polaris_gmu_boot_rail_active = false;
	if (!polaris_gmu_firmware_started || polaris_gpu_pwrlevel_count < 2u ||
	    polaris_gpu_initial_pwrlevel >= polaris_gpu_pwrlevel_count - 1u || gmu.virtual == 0) {
		printf("adreno-sdm845: step 12 cannot request the GMU boot rail without a running GMU and validated initial GPU powerlevel\n");
		return false;
	}
	initial = &polaris_gpu_pwrlevels[polaris_gpu_initial_pwrlevel];
	if (initial->frequency_hz == 0 ||
	    !polaris_gpu_voltage_corner(initial->frequency_hz, &corner) ||
	    !polaris_cmd_db_resource_lookup("gfx.lvl", &gfx_level) ||
	    !polaris_cmd_db_resource_lookup("mx.lvl", &mx_level) ||
	    !polaris_cmd_db_arc_vote(&gfx_level, &mx_level, corner, &vote)) {
		printf("adreno-sdm845: step 12 could not map initial GPU powerlevel %u (%u Hz) to validated GX/MX ARC indices\n",
		    polaris_gpu_initial_pwrlevel, initial->frequency_hz);
		return false;
	}
	gx_index = vote & 0xffu;
	mx_index = (vote >> 8) & 0xffu;
	printf("adreno-sdm845: step 12 requests initial GX/MX ARC vote for GPU powerlevel %u (%u Hz; GX=%u MX=%u)\n",
	    polaris_gpu_initial_pwrlevel, initial->frequency_hz, gx_index, mx_index);
	probe_write(&gmu, GMU_BOOT_SLUMBER_OPTION, 0u); /* OOB_BOOT_OPTION */
	probe_write(&gmu, GMU_GX_VOTE_IDX, gx_index);
	probe_write(&gmu, GMU_MX_VOTE_IDX, mx_index);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&gmu, GMU_BOOT_SLUMBER_OPTION) != 0u ||
	    probe_read(&gmu, GMU_GX_VOTE_IDX) != gx_index || probe_read(&gmu, GMU_MX_VOTE_IDX) != mx_index) {
		printf("adreno-sdm845: step 12 GMU boot rail register readback failed; no OOB request was sent\n");
		return false;
	}
	probe_write(&gmu, GMU_HOST2GMU_INTR_SET, A630_GMU_OOB_BOOT_SET_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_QUEUE_START_POLLS; poll++) {
		if ((probe_read(&gmu, GMU_GMU2HOST_INTR_INFO) & A630_GMU_OOB_BOOT_ACK_MASK) != 0) {
			acknowledged = true;
			break;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	probe_write(&gmu, GMU_GMU2HOST_INTR_CLR, A630_GMU_OOB_BOOT_ACK_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!acknowledged) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK);
		printf("adreno-sdm845: step 12 timed out waiting for the A630 boot/slumber OOB acknowledgement\n");
		return false;
	}
	polaris_gmu_boot_rail_active = true;
	return true;
}

static bool
polaris_gmu_hfi_perf_table_build(uint16_t sequence, uint32_t message[POLARIS_A630_HFI_PERF_TABLE_DWORDS])
{
	struct polaris_cmd_db_resource cx_level, gfx_level, mx_level;
	struct polaris_a630_hfi_opp gpu[POLARIS_A630_HFI_MAX_GPU_LEVELS];
	struct polaris_a630_hfi_opp gmu[POLARIS_A630_HFI_MAX_GMU_LEVELS];
	uint32_t previous_frequency = 0;

	if (message == NULL || polaris_gpu_pwrlevel_count == 0 ||
	    polaris_gpu_pwrlevel_count > POLARIS_A630_HFI_MAX_GPU_LEVELS || polaris_gmu_pwrlevel_count == 0 ||
	    polaris_gmu_pwrlevel_count > POLARIS_A630_HFI_MAX_GMU_LEVELS ||
	    !polaris_cmd_db_resource_lookup("gfx.lvl", &gfx_level) ||
	    !polaris_cmd_db_resource_lookup("cx.lvl", &cx_level) ||
	    !polaris_cmd_db_resource_lookup("mx.lvl", &mx_level)) {
		printf("adreno-sdm845: step 13 could not read bounded GPU, GMU, and MX ARC level tables\n");
		return false;
	}

	/* KGSL reverses the descending DT powerlevel list so HFI starts at idle/lowest frequency. */
	for (uint32_t i = 0; i < polaris_gpu_pwrlevel_count; i++) {
		const struct polaris_gpu_operating_point *level =
		    &polaris_gpu_pwrlevels[polaris_gpu_pwrlevel_count - 1u - i];
		uint16_t corner;

		if ((i == 0 && level->frequency_hz != 0) || (i != 0 && level->frequency_hz <= previous_frequency) ||
		    level->frequency_hz % 1000u != 0 || !polaris_gpu_voltage_corner(level->frequency_hz, &corner) ||
		    !polaris_cmd_db_arc_vote(&gfx_level, &mx_level, corner, &gpu[i].vote)) {
			printf("adreno-sdm845: step 13 cannot map GPU HFI powerlevel %u (%u Hz) to an ARC vote\n",
			    i, level->frequency_hz);
			return false;
		}
		gpu[i].frequency_khz = level->frequency_hz / 1000u;
		previous_frequency = level->frequency_hz;
	}

	previous_frequency = 0;
	for (uint32_t i = 0; i < polaris_gmu_pwrlevel_count; i++) {
		uint32_t frequency = polaris_gmu_pwrlevels[i];
		uint16_t corner;

		if ((i == 0 && frequency != 0) || (i != 0 && frequency <= previous_frequency) ||
		    frequency % 1000u != 0 || !polaris_gmu_voltage_corner(frequency, &corner) ||
		    !polaris_cmd_db_arc_vote(&cx_level, &mx_level, corner, &gmu[i].vote)) {
			printf("adreno-sdm845: step 13 cannot map GMU HFI powerlevel %u (%u Hz) to an ARC vote\n",
			    i, frequency);
			return false;
		}
		gmu[i].frequency_khz = frequency / 1000u;
		previous_frequency = frequency;
	}

	if (!polaris_a630_hfi_make_perf_table(sequence, gpu, polaris_gpu_pwrlevel_count, gmu,
	    polaris_gmu_pwrlevel_count, message)) {
		printf("adreno-sdm845: step 13 could not encode the fixed A630 HFI v1 performance-table layout\n");
		return false;
	}
	printf("adreno-sdm845: step 13 built HFI PERF_TBL: %u GPU levels (max=%u kHz), %u GMU levels (max=%u kHz); no bus votes were built\n",
	    polaris_gpu_pwrlevel_count, gpu[polaris_gpu_pwrlevel_count - 1u].frequency_khz,
	    polaris_gmu_pwrlevel_count, gmu[polaris_gmu_pwrlevel_count - 1u].frequency_khz);
	return true;
}

#define POLARIS_A630_BCM_COMMIT_MASK  0x40000000u
#define POLARIS_A630_BCM_VALID_MASK   0x20000000u
#define POLARIS_A630_BCM_VOTE_MASK    0x3fffu
#define POLARIS_A630_BCM_ACV_APPS     0x8u
#define POLARIS_A630_BCM_VCD_COUNT    16u

struct polaris_a630_hfi_bus_node_ref {
	const char *path;
	uint32_t cell_id;
};

struct polaris_a630_hfi_bcm_ref {
	uint32_t node_index;
	const char *path;
	const char *name;
};

struct polaris_a630_hfi_bus_route {
	const char *name;
	const struct polaris_a630_hfi_bus_node_ref *nodes;
	uint32_t node_count;
	const struct polaris_a630_hfi_bcm_ref *bcms;
	uint32_t bcm_count;
};

struct polaris_a630_hfi_bus_node {
	DTEntry entry;
	uint32_t width;
	uint32_t aggregate_ports;
	uint32_t phandle;
};

struct polaris_a630_hfi_bus_vote {
	const char *name;
	uint32_t address;
	uint32_t unit_size;
	uint32_t clock_domain;
	uint16_t width;
	uint32_t node_mask;
	bool acv;
};

struct polaris_a630_hfi_bus_route_state {
	uint32_t node_count;
	struct polaris_a630_hfi_bus_node nodes[4];
	uint32_t vote_count;
	struct polaris_a630_hfi_bus_vote votes[POLARIS_A630_HFI_MAX_DDR_COMMANDS];
};

static const struct polaris_a630_hfi_bus_node_ref polaris_a630_ddr_bus_nodes[] = {
	{ "/soc/ad-hoc-bus/mas-qxm-gpu", 0x001au },
	{ "/soc/ad-hoc-bus/slv-qns-llcc", 0x0302u },
	{ "/soc/ad-hoc-bus/mas-llcc-mc", 0x0081u },
	{ "/soc/ad-hoc-bus/slv-ebi", 0x0200u },
};

static const struct polaris_a630_hfi_bcm_ref polaris_a630_ddr_bus_bcms[] = {
	{ 1u, "/soc/ad-hoc-bus/bcm-sh0", "SH0" },
	{ 3u, "/soc/ad-hoc-bus/bcm-mc0", "MC0" },
	{ 3u, "/soc/ad-hoc-bus/bcm-acv", "ACV" },
};

static const struct polaris_a630_hfi_bus_node_ref polaris_a630_cnoc_bus_nodes[] = {
	{ "/soc/ad-hoc-bus/mas-qxm-gpu", 0x001au },
	{ "/soc/ad-hoc-bus/slv-qns-memnoc-snoc", 0x0308u },
	{ "/soc/ad-hoc-bus/mas-qnm-memnoc", 0x008eu },
	{ "/soc/ad-hoc-bus/slv-qns-cnoc", 0x2734u },
};

static const struct polaris_a630_hfi_bcm_ref polaris_a630_cnoc_bus_bcms[] = {
	{ 1u, "/soc/ad-hoc-bus/bcm-sh2", "SH2" },
	{ 2u, "/soc/ad-hoc-bus/bcm-sn15", "SN15" },
	{ 3u, "/soc/ad-hoc-bus/bcm-sn3", "SN3" },
};

static bool
polaris_dt_u32_array_contains(DTEntry node, const char *name, uint32_t value)
{
	const uint8_t *property;
	unsigned int size = 0;

	if (node == NULL || SecureDTGetProperty(node, name, (const void **)&property, &size) != kSuccess ||
	    property == NULL || size == 0 || (size & (sizeof(uint32_t) - 1u)) != 0) {
		return false;
	}
	for (unsigned int offset = 0; offset < size; offset += sizeof(uint32_t)) {
		if (polaris_dt_be32(property + offset) == value) {
			return true;
		}
	}
	return false;
}

static bool
polaris_dt_u32_array_equals(DTEntry node, const char *name, const uint32_t *values, uint32_t count)
{
	const uint8_t *property = NULL;
	unsigned int size = 0;
	int result;

	if (node == NULL || (count != 0 && values == NULL)) {
		return false;
	}
	result = SecureDTGetProperty(node, name, (const void **)&property, &size);
	if (count == 0) {
		return result != kSuccess || size == 0;
	}
	if (result != kSuccess || property == NULL || size != count * sizeof(uint32_t)) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (polaris_dt_be32(property + i * sizeof(uint32_t)) != values[i]) {
			return false;
		}
	}
	return true;
}

static bool
polaris_a630_hfi_bus_route_load(const struct polaris_a630_hfi_bus_route *route,
	const struct polaris_msm_bus_vector *vector, struct polaris_a630_hfi_bus_route_state *state)
{
	DTEntry apps_rsc = NULL;
	uint32_t apps_rsc_phandle;

	if (route == NULL || vector == NULL || state == NULL || route->nodes == NULL || route->bcms == NULL ||
	    route->node_count < 2 || route->node_count > sizeof(state->nodes) / sizeof(state->nodes[0]) ||
	    route->bcm_count == 0 || route->bcm_count > sizeof(state->votes) / sizeof(state->votes[0]) ||
	    vector->source != route->nodes[0].cell_id || vector->destination != route->nodes[route->node_count - 1u].cell_id ||
	    SecureDTLookupEntry(NULL, "/soc/ad-hoc-bus/rsc-apps", &apps_rsc) != kSuccess || apps_rsc == NULL ||
	    !polaris_dt_get_u32(apps_rsc, "phandle", &apps_rsc_phandle)) {
		printf("adreno-sdm845: HFI %s bus route endpoints or Apps RSC do not match the runtime DT\n",
		    route != NULL && route->name != NULL ? route->name : "unnamed");
		return false;
	}
	memset(state, 0, sizeof(*state));
	state->node_count = route->node_count;

	for (uint32_t i = 0; i < route->node_count; i++) {
		const struct polaris_a630_hfi_bus_node_ref *reference = &route->nodes[i];
		struct polaris_a630_hfi_bus_node *node = &state->nodes[i];
		uint32_t cell_id;

		if (SecureDTLookupEntry(NULL, reference->path, &node->entry) != kSuccess || node->entry == NULL ||
		    !polaris_dt_get_u32(node->entry, "cell-id", &cell_id) || cell_id != reference->cell_id ||
		    !polaris_dt_get_u32(node->entry, "qcom,buswidth", &node->width) || node->width == 0 ||
		    !polaris_dt_get_u32(node->entry, "qcom,agg-ports", &node->aggregate_ports) ||
		    node->aggregate_ports == 0 || node->width > UINT32_MAX / node->aggregate_ports) {
			printf("adreno-sdm845: HFI %s route node %u is incomplete or differs from the validated Polaris bus topology\n",
			    route->name, i);
			return false;
		}
		if (i != 0 && !polaris_dt_get_u32(node->entry, "phandle", &node->phandle)) {
			printf("adreno-sdm845: HFI %s route node %u has no phandle for its upstream connection\n",
			    route->name, i);
			return false;
		}
		if (i + 1u < route->node_count) {
			struct polaris_a630_hfi_bus_node *next = &state->nodes[i + 1u];
			DTEntry next_entry = NULL;

			if (SecureDTLookupEntry(NULL, route->nodes[i + 1u].path, &next_entry) != kSuccess || next_entry == NULL ||
			    !polaris_dt_get_u32(next_entry, "phandle", &next->phandle) ||
			    !polaris_dt_u32_array_contains(node->entry, "qcom,connections", next->phandle)) {
				printf("adreno-sdm845: HFI %s route edge %u is absent from the runtime bus graph\n", route->name, i);
				return false;
			}
		}
	}

	for (uint32_t node_index = 0; node_index < route->node_count; node_index++) {
		uint32_t expected_phandles[POLARIS_A630_HFI_MAX_DDR_COMMANDS];
		uint32_t expected_count = 0;

		for (uint32_t i = 0; i < route->bcm_count; i++) {
			const struct polaris_a630_hfi_bcm_ref *reference = &route->bcms[i];
			DTEntry bcm_node = NULL;
			uint32_t phandle;
			struct polaris_cmd_db_bcm cmd_db_bcm;
			struct polaris_a630_hfi_bus_vote *vote = NULL;

			if (reference->node_index != node_index) {
				continue;
			}
			if (expected_count >= sizeof(expected_phandles) / sizeof(expected_phandles[0]) ||
			    SecureDTLookupEntry(NULL, reference->path, &bcm_node) != kSuccess || bcm_node == NULL ||
			    !polaris_dt_string_equals(bcm_node, "qcom,bcm-name", reference->name) ||
			    !polaris_dt_has_empty_property(bcm_node, "qcom,bcm-dev") ||
			    !polaris_dt_get_u32(bcm_node, "phandle", &phandle) ||
			    !polaris_dt_u32_array_equals(bcm_node, "qcom,rscs", &apps_rsc_phandle, 1u) ||
			    !polaris_cmd_db_bcm_lookup(reference->name, &cmd_db_bcm)) {
				printf("adreno-sdm845: HFI %s BCM %s disagrees with the Apps RSC or AOP CMD_DB\n",
				    route->name, reference->name);
				return false;
			}
			expected_phandles[expected_count++] = phandle;
			for (uint32_t j = 0; j < state->vote_count; j++) {
				if (strcmp(state->votes[j].name, reference->name) == 0) {
					vote = &state->votes[j];
					break;
				}
			}
			if (vote == NULL) {
				if (state->vote_count >= sizeof(state->votes) / sizeof(state->votes[0])) {
					printf("adreno-sdm845: HFI %s route exceeds the bounded BCM count\n", route->name);
					return false;
				}
				vote = &state->votes[state->vote_count++];
				vote->name = reference->name;
				vote->address = cmd_db_bcm.address;
				vote->unit_size = cmd_db_bcm.unit_size;
				vote->width = cmd_db_bcm.width;
				vote->clock_domain = cmd_db_bcm.clock_domain;
				vote->acv = strcmp(reference->name, "ACV") == 0;
			} else if (vote->address != cmd_db_bcm.address || vote->unit_size != cmd_db_bcm.unit_size ||
			    vote->width != cmd_db_bcm.width || vote->clock_domain != cmd_db_bcm.clock_domain) {
				printf("adreno-sdm845: HFI %s BCM %s has inconsistent CMD_DB entries\n", route->name, reference->name);
				return false;
			}
			vote->node_mask |= 1u << node_index;
		}
		if (!polaris_dt_u32_array_equals(state->nodes[node_index].entry, "qcom,bcms",
		    expected_phandles, expected_count)) {
			printf("adreno-sdm845: HFI %s node %u BCM links differ from the route used to build the vote table\n",
			    route->name, node_index);
			return false;
		}
	}

	/* msm_bus_query_gen() emits BCM TCS commands by VCD, retaining path order within each VCD. */
	for (uint32_t i = 1; i < state->vote_count; i++) {
		struct polaris_a630_hfi_bus_vote vote = state->votes[i];
		uint32_t j = i;

		while (j != 0 && state->votes[j - 1u].clock_domain > vote.clock_domain) {
			state->votes[j] = state->votes[j - 1u];
			j--;
		}
		state->votes[j] = vote;
	}
	return state->vote_count != 0;
}

static bool
polaris_msm_bus_div64(uint64_t numerator, uint64_t denominator, uint64_t *quotient)
{
	if (denominator == 0 || denominator > UINT32_MAX || quotient == NULL) {
		return false;
	}
	/* msm_bus_div64() rounds a nonzero sub-unit vote up to one, then truncates larger values. */
	*quotient = numerator != 0 && numerator < denominator ? 1u : numerator / denominator;
	return true;
}

static bool
polaris_a630_hfi_bus_row_build(const struct polaris_a630_hfi_bus_route_state *route,
	const struct polaris_msm_bus_vector *vector, uint32_t *row, uint32_t *addresses, uint32_t *wait_mask)
{
	if (route == NULL || vector == NULL || row == NULL || wait_mask == NULL || route->vote_count == 0 ||
	    route->vote_count > POLARIS_A630_HFI_MAX_DDR_COMMANDS) {
		return false;
	}
	*wait_mask = 0;
	for (uint32_t i = 0; i < route->vote_count; i++) {
		const struct polaris_a630_hfi_bus_vote *vote = &route->votes[i];
		uint64_t max_ab = 0, max_ib = 0, vote_ab, vote_ib;
		bool commit = i + 1u == route->vote_count || route->votes[i + 1u].clock_domain != vote->clock_domain;

		if (addresses != NULL) {
			addresses[i] = vote->address;
		}
		if (vote->acv) {
			vote_ib = vector->average_kbps != 0 || vector->instantaneous_kbps != 0 ? POLARIS_A630_BCM_ACV_APPS : 0;
			vote_ab = 0;
		} else {
			for (uint32_t node_index = 0; node_index < route->node_count; node_index++) {
				const struct polaris_a630_hfi_bus_node *node = &route->nodes[node_index];
				uint64_t node_ab, node_ib;
				uint64_t ab_divisor = (uint64_t)node->width * node->aggregate_ports;

				if ((vote->node_mask & (1u << node_index)) == 0) {
					continue;
				}
				if (!polaris_msm_bus_div64((uint64_t)vector->average_kbps * vote->width, ab_divisor, &node_ab) ||
				    !polaris_msm_bus_div64((uint64_t)vector->instantaneous_kbps * vote->width, node->width, &node_ib)) {
					return false;
				}
				max_ab = node_ab > max_ab ? node_ab : max_ab;
				max_ib = node_ib > max_ib ? node_ib : max_ib;
			}
			if (!polaris_msm_bus_div64(max_ab, vote->unit_size, &vote_ab) ||
			    !polaris_msm_bus_div64(max_ib, vote->unit_size, &vote_ib)) {
				return false;
			}
		}

		if (vote_ab > POLARIS_A630_BCM_VOTE_MASK) {
			vote_ab = POLARIS_A630_BCM_VOTE_MASK;
		}
		if (vote_ib > POLARIS_A630_BCM_VOTE_MASK) {
			vote_ib = POLARIS_A630_BCM_VOTE_MASK;
		}
		row[i] = (commit ? POLARIS_A630_BCM_COMMIT_MASK : 0) |
		    ((vote_ab != 0 || vote_ib != 0) ? POLARIS_A630_BCM_VALID_MASK : 0) |
		    ((uint32_t)vote_ab << 14) | (uint32_t)vote_ib;
		/* Qualcomm 4.9 KGSL computes this HFI mask only for usecase 0 and sets BIT(0). */
		if (commit) {
			*wait_mask |= 1u;
		} else {
			*wait_mask &= ~1u;
		}
	}
	return true;
}

static bool
polaris_a630_hfi_row_commit_mask(const uint32_t *row, uint32_t command_count, uint32_t *wait_mask)
{
	if (row == NULL || wait_mask == NULL || command_count == 0 ||
	    command_count > POLARIS_A630_HFI_MAX_DDR_COMMANDS) {
		return false;
	}
	*wait_mask = 0;
	for (uint32_t command = 0; command < command_count; command++) {
		if ((row[command] & POLARIS_A630_BCM_COMMIT_MASK) != 0) {
			*wait_mask |= 1u << command;
		}
	}
	return *wait_mask != 0;
}

static bool
polaris_gmu_hfi_bw_table_build(uint16_t sequence, struct polaris_a630_hfi_bw_table *table)
{
	static const struct polaris_a630_hfi_bus_route ddr_route = {
		.name = "GPU-to-DDR",
		.nodes = polaris_a630_ddr_bus_nodes,
		.node_count = sizeof(polaris_a630_ddr_bus_nodes) / sizeof(polaris_a630_ddr_bus_nodes[0]),
		.bcms = polaris_a630_ddr_bus_bcms,
		.bcm_count = sizeof(polaris_a630_ddr_bus_bcms) / sizeof(polaris_a630_ddr_bus_bcms[0]),
	};
	static const struct polaris_a630_hfi_bus_route cnoc_route = {
		.name = "GPU-to-CNOC",
		.nodes = polaris_a630_cnoc_bus_nodes,
		.node_count = sizeof(polaris_a630_cnoc_bus_nodes) / sizeof(polaris_a630_cnoc_bus_nodes[0]),
		.bcms = polaris_a630_cnoc_bus_bcms,
		.bcm_count = sizeof(polaris_a630_cnoc_bus_bcms) / sizeof(polaris_a630_cnoc_bus_bcms[0]),
	};
	struct polaris_a630_hfi_bus_route_state ddr_state, cnoc_state;

	if (table == NULL || sequence == 0 || sizeof(*table) / sizeof(uint32_t) != POLARIS_A630_HFI_BW_TABLE_DWORDS ||
	    polaris_gpu_bus_level_count == 0 || polaris_gpu_bus_level_count > POLARIS_A630_HFI_MAX_BW_LEVELS ||
	    polaris_gpu_bus_level_count != POLARIS_EXPECTED_GPU_BUS_LEVELS ||
	    polaris_gmu_cnoc_level_count != POLARIS_A630_HFI_MAX_CNOC_LEVELS ||
	    !polaris_a630_hfi_bus_route_load(&ddr_route, &polaris_gpu_bus_levels[0], &ddr_state) ||
	    !polaris_a630_hfi_bus_route_load(&cnoc_route, &polaris_gmu_cnoc_levels[0], &cnoc_state) ||
	    ddr_state.vote_count > POLARIS_A630_HFI_MAX_DDR_COMMANDS ||
	    cnoc_state.vote_count > POLARIS_A630_HFI_MAX_CNOC_COMMANDS) {
		printf("adreno-sdm845: cannot reconstruct bounded A630 GPU DDR/CNOC TCS tables from Polaris DT and CMD_DB\n");
		return false;
	}

	memset(table, 0, sizeof(*table));
	table->bw_level_num = polaris_gpu_bus_level_count;
	table->ddr_cmds_num = ddr_state.vote_count;
	table->cnoc_cmds_num = cnoc_state.vote_count;
	for (uint32_t i = 0; i < polaris_gpu_bus_level_count; i++) {
		uint32_t *row = table->ddr_cmd_data[i];
		uint32_t *addresses = i == 0 ? table->ddr_cmd_addrs : NULL;
		uint32_t wait_mask;

		if (!polaris_a630_hfi_bus_row_build(&ddr_state, &polaris_gpu_bus_levels[i], row, addresses, &wait_mask)) {
			printf("adreno-sdm845: could not convert GPU bandwidth usecase %u into BCM TCS commands\n", i);
			return false;
		}
		if (i == 0) {
			table->ddr_wait_bitmask = wait_mask;
		}
	}
	for (uint32_t i = 0; i < polaris_gmu_cnoc_level_count; i++) {
		uint32_t *row = table->cnoc_cmd_data[i];
		uint32_t *addresses = i == 0 ? table->cnoc_cmd_addrs : NULL;
		uint32_t wait_mask;

		if (!polaris_a630_hfi_bus_row_build(&cnoc_state, &polaris_gmu_cnoc_levels[i], row, addresses, &wait_mask)) {
			printf("adreno-sdm845: could not convert GMU CNOC usecase %u into BCM TCS commands\n", i);
			return false;
		}
		if (i == 0) {
			table->cnoc_wait_bitmask = wait_mask;
		}
	}
	if (!polaris_a630_hfi_make_header(POLARIS_A630_HFI_ID_BW_VOTE_TABLE,
	    POLARIS_A630_HFI_BW_TABLE_DWORDS, POLARIS_A630_HFI_TYPE_COMMAND, sequence, &table->header)) {
		printf("adreno-sdm845: could not encode the fixed A630 HFI v1 BW_VOTE_TBL header\n");
		return false;
	}
	printf("adreno-sdm845: built A630 HFI BW_VOTE_TBL: %u DDR levels/%u BCM commands and %u CNOC levels/%u BCM commands; wait masks=%#x/%#x\n",
	    table->bw_level_num, table->ddr_cmds_num, polaris_gmu_cnoc_level_count,
	    table->cnoc_cmds_num, table->ddr_wait_bitmask, table->cnoc_wait_bitmask);
	return true;
}

/*
 * Submit one Apps-RSC active TCS usecase. Polaris' validated DRV2 v2.7 layout uses TCS0 for
 * active requests; serialize it with the other HakiOS RPMh clients and leave an in-flight TCS
 * untouched if it does not complete within the bounded wait.
 */
static bool
polaris_rpmh_active_tcs_request(const uint32_t *addresses, const uint32_t *data,
	uint32_t command_count, uint32_t wait_mask, bool *request_submitted)
{
	uint32_t revision, major, minor, drv_config, hardware_tcs, commands_per_tcs;
	uint32_t tcs_offset = polaris_rpmh_tcs_offset;
	uint32_t tcs_status_offset, tcs_control_offset, tcs_cmd_enable_offset;
	uint32_t enabled_mask, status, control, done = 0, command_status[POLARIS_A630_HFI_MAX_DDR_COMMANDS];
	bool completed = false, cleanable = false, success = false;

	if (request_submitted != NULL) {
		*request_submitted = false;
	}
	if (addresses == NULL || data == NULL || command_count == 0 ||
	    command_count > POLARIS_A630_HFI_MAX_DDR_COMMANDS || command_count >= 32u ||
	    (polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_ACTIVE_TYPE] == 0) ||
	    (wait_mask & ~((1u << command_count) - 1u)) != 0 ||
	    polaris_rpmh_drv.physical == 0 || polaris_rpmh_drv.size == 0 || !map_block(&polaris_rpmh_drv)) {
		printf("adreno-sdm845: Apps RSC active-TCS request has invalid bounds or no validated DRV2 mapping\n");
		return false;
	}
	for (uint32_t command = 0; command < command_count; command++) {
		bool commit = (data[command] & POLARIS_A630_BCM_COMMIT_MASK) != 0;

		if (addresses[command] == 0 || commit != ((wait_mask & (1u << command)) != 0)) {
			printf("adreno-sdm845: Apps RSC command %u has no address or its commit/wait mask disagrees\n", command);
			return false;
		}
	}

	revision = probe_read(&polaris_rpmh_drv, POLARIS_RPMH_DRV_ID);
	major = (revision >> POLARIS_RPMH_ID_MAJOR_SHIFT) & POLARIS_RPMH_ID_VERSION_MASK;
	minor = (revision >> POLARIS_RPMH_ID_MINOR_SHIFT) & POLARIS_RPMH_ID_VERSION_MASK;
	if (major != 2u || minor != 7u) {
		printf("adreno-sdm845: Apps RSC active-TCS writes require DRV2 v2.7, found v%u.%u; request refused\n",
		    major, minor);
		return false;
	}
	drv_config = probe_read(&polaris_rpmh_drv, POLARIS_RPMH_DRV_CONFIG);
	hardware_tcs = (drv_config >> (POLARIS_RPMH_DRV_NUM_TCS_SHIFT * polaris_rpmh_drv_id)) &
	    POLARIS_RPMH_DRV_NUM_TCS_MASK;
	commands_per_tcs = (drv_config >> POLARIS_RPMH_DRV_NCPT_SHIFT) & POLARIS_RPMH_DRV_NCPT_MASK;
	if (hardware_tcs != polaris_rpmh_total_tcs ||
	    hardware_tcs < polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_ACTIVE_TYPE] ||
	    commands_per_tcs < command_count || tcs_offset + POLARIS_RPMH_TCS_STRIDE > polaris_rpmh_drv.size) {
		printf("adreno-sdm845: Apps RSC TCS layout has %u/%u commands, hardware TCS=%u, DT TCS=%u; request refused\n",
		    commands_per_tcs, command_count, hardware_tcs, polaris_rpmh_total_tcs);
		return false;
	}
	tcs_status_offset = POLARIS_RPMH_TCS_STATUS_V27;
	tcs_control_offset = POLARIS_RPMH_TCS_CONTROL_V27;
	tcs_cmd_enable_offset = POLARIS_RPMH_TCS_CMD_ENABLE_V27;
	enabled_mask = (1u << command_count) - 1u;

	lck_mtx_lock(&hakios_rsc_lock);
	status = probe_read(&polaris_rpmh_drv, tcs_offset + tcs_status_offset);
	control = probe_read(&polaris_rpmh_drv, tcs_offset + tcs_control_offset);
	if (status == 0 || probe_read(&polaris_rpmh_drv, tcs_offset + tcs_cmd_enable_offset) != 0) {
		printf("adreno-sdm845: Apps RSC active TCS0 is busy or has enabled commands (status=%#x control=%#x); request refused\n",
		    status, control);
		goto out;
	}

	probe_write(&polaris_rpmh_drv, tcs_offset + POLARIS_RPMH_TCS_IRQ_CLEAR, 1u);
	for (uint32_t command = 0; command < command_count; command++) {
		uint32_t command_offset = tcs_offset + POLARIS_RPMH_TCS_COMMAND_BASE +
		    POLARIS_RPMH_TCS_COMMAND_STRIDE * command;

		probe_write(&polaris_rpmh_drv, command_offset, POLARIS_RPMH_TCS_MSGID_WRITE_WAITED);
		probe_write(&polaris_rpmh_drv, command_offset + 4u, addresses[command]);
		probe_write(&polaris_rpmh_drv, command_offset + 8u, data[command]);
	}
	probe_write(&polaris_rpmh_drv, tcs_offset + POLARIS_RPMH_TCS_WAIT_FOR_DONE, wait_mask);
	probe_write(&polaris_rpmh_drv, tcs_offset + tcs_cmd_enable_offset, enabled_mask);
	__asm__ volatile ("dsb sy" ::: "memory");
	control = probe_read(&polaris_rpmh_drv, tcs_offset + tcs_control_offset) & ~POLARIS_RPMH_TCS_TRIGGER;
	probe_write(&polaris_rpmh_drv, tcs_offset + tcs_control_offset, control);
	probe_write(&polaris_rpmh_drv, tcs_offset + tcs_control_offset, control | POLARIS_RPMH_TCS_ACTIVE_MODE);
	probe_write(&polaris_rpmh_drv, tcs_offset + tcs_control_offset,
	    control | POLARIS_RPMH_TCS_ACTIVE_MODE | POLARIS_RPMH_TCS_TRIGGER);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (request_submitted != NULL) {
		*request_submitted = true;
	}
	for (uint32_t poll = 0; poll < POLARIS_RPMH_TCS_POLL_LIMIT; poll++) {
		done = probe_read(&polaris_rpmh_drv, tcs_offset + POLARIS_RPMH_TCS_IRQ_STATUS) & 1u;
		if (done != 0) {
			completed = true;
			break;
		}
		delay(POLARIS_RPMH_TCS_POLL_DELAY_US);
	}
	status = probe_read(&polaris_rpmh_drv, tcs_offset + tcs_status_offset);
	cleanable = status != 0;
	if (completed && cleanable) {
		success = true;
		for (uint32_t command = 0; command < command_count; command++) {
			uint32_t command_offset = tcs_offset + POLARIS_RPMH_TCS_COMMAND_BASE +
			    POLARIS_RPMH_TCS_COMMAND_STRIDE * command;

			command_status[command] = probe_read(&polaris_rpmh_drv, command_offset + 12u);
			if (command_status[command] != 0) {
				printf("adreno-sdm845: Apps RSC TCS0 command %u returned status %#x\n",
				    command, command_status[command]);
				success = false;
			}
		}
	}
	if (!completed || !cleanable) {
		printf("adreno-sdm845: Apps RSC TCS0 %s (IRQ=%#x status=%#x); in-flight state is left intact when busy\n",
		    completed ? "reported completion but remains busy" : "timed out", done, status);
	}
	if (cleanable) {
		probe_write(&polaris_rpmh_drv, tcs_offset + POLARIS_RPMH_TCS_IRQ_CLEAR, 1u);
		probe_write(&polaris_rpmh_drv, tcs_offset + tcs_control_offset,
		    probe_read(&polaris_rpmh_drv, tcs_offset + tcs_control_offset) &
		    ~(POLARIS_RPMH_TCS_TRIGGER | POLARIS_RPMH_TCS_ACTIVE_MODE));
		probe_write(&polaris_rpmh_drv, tcs_offset + tcs_cmd_enable_offset, 0u);
		probe_write(&polaris_rpmh_drv, tcs_offset + POLARIS_RPMH_TCS_WAIT_FOR_DONE, 0u);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
out:
	lck_mtx_unlock(&hakios_rsc_lock);
	return success;
}

/* Hold a host DDR vote for GMU boot, then release it after GMU DCVS owns the default vote. */
static bool
polaris_gmu_temp_bus_vote_set(bool hold_vote)
{
	struct polaris_a630_hfi_bw_table table;
	uint32_t bus_index, tcs_wait_mask;
	bool request_submitted = false;

	if (!hold_vote && !polaris_gmu_temp_bus_vote_active) {
		return true;
	}
	if (hold_vote && polaris_gmu_temp_bus_vote_active) {
		return false;
	}
	if (polaris_gpu_pwrlevel_count == 0 || polaris_gpu_initial_pwrlevel >= polaris_gpu_pwrlevel_count ||
	    !polaris_gmu_hfi_bw_table_build(1u, &table)) {
		printf("adreno-sdm845: cannot derive a temporary startup DDR request from the validated A630 bus table\n");
		return false;
	}
	bus_index = hold_vote ? polaris_gpu_pwrlevels[polaris_gpu_initial_pwrlevel].bus_frequency : 0u;
	if (bus_index >= table.bw_level_num) {
		printf("adreno-sdm845: initial GPU powerlevel bus index %u exceeds the generated DDR table\n", bus_index);
		return false;
	}
	if (hold_vote && bus_index == 0u) {
		printf("adreno-sdm845: initial GPU bus usecase is zero; no temporary Apps RSC vote is needed\n");
		return true;
	}
	if (!polaris_a630_hfi_row_commit_mask(table.ddr_cmd_data[bus_index], table.ddr_cmds_num, &tcs_wait_mask) ||
	    !polaris_rpmh_active_tcs_request(table.ddr_cmd_addrs, table.ddr_cmd_data[bus_index],
	    table.ddr_cmds_num, tcs_wait_mask, &request_submitted)) {
		if (hold_vote && request_submitted) {
			/* The RSC may have applied a vote before a lost or negative completion. */
			polaris_gmu_temp_bus_vote_active = true;
		}
		printf("adreno-sdm845: Apps RSC did not acknowledge the %s startup DDR vote (usecase %u)\n",
		    hold_vote ? "temporary" : "release", bus_index);
		return false;
	}
	polaris_gmu_temp_bus_vote_active = hold_vote;
	printf("adreno-sdm845: Apps RSC %s startup DDR vote (usecase %u, %u BCM commands, wait mask=%#x)\n",
	    hold_vote ? "accepted temporary" : "released temporary", bus_index,
	    table.ddr_cmds_num, tcs_wait_mask);
	return true;
}

static bool
probe_zap_mdt(const uint8_t *header, uint64_t size)
{
	uint32_t phoff, hash_index = UINT32_MAX, loadable = 0, header_size;
	uint32_t min_address = UINT32_MAX;
	uint16_t phentsize, phnum;
	uint64_t table_end, hash_size = 0, max_address = 0;
	bool relocatable = false;
	const uint8_t *hash_data;
	char name[32];
	uint64_t file_size = 0;

	polaris_zap_manifest_valid = false;
	polaris_zap_segment_count = 0;
	polaris_zap_mdt = NULL;
	polaris_zap_hash = NULL;
	polaris_zap_mdt_size = 0;
	polaris_zap_header_size = 0;
	polaris_zap_hash_size = 0;
	polaris_zap_memory_setup_size = 0;
	if (header == NULL || size < 52 || size > A630_MDT_MAX_BYTES ||
	    memcmp(header, "\177ELF\1\1\1", 7) != 0 || read_le16(header + 16) != 2 ||
	    read_le16(header + 18) != 164 || read_le16(header + 40) != 52) {
		printf("adreno-sdm845: ZAP MDT is not a bounded little-endian Qualcomm DSP ELF header (%llu bytes)\n", size);
		return false;
	}
	phoff = read_le32(header + 28);
	phentsize = read_le16(header + 42);
	phnum = read_le16(header + 44);
	if (phoff != 52 || phentsize != 32 || phnum < 2 || phnum > A630_MDT_MAX_SEGMENTS ||
	    (uint64_t)phoff + (uint64_t)phentsize * phnum > size) {
		printf("adreno-sdm845: ZAP MDT has invalid program-header bounds (offset=%#x entry=%u count=%u size=%llu)\n",
		    phoff, phentsize, phnum, size);
		return false;
	}
	table_end = (uint64_t)phoff + (uint64_t)phentsize * phnum;
	header_size = read_le32(header + phoff + 16);
	if (read_le32(header + phoff) == 1 || header_size < table_end || header_size > size) {
		printf("adreno-sdm845: ZAP MDT first program header does not describe its complete ELF header\n");
		return false;
	}
	for (uint32_t i = 0; i < phnum; i++) {
		const uint8_t *ph = header + phoff + (uint64_t)i * phentsize;
		uint32_t type = read_le32(ph);
		uint32_t address = read_le32(ph + 12);
		uint32_t file_size = read_le32(ph + 16);
		uint32_t memory_size = read_le32(ph + 20);
		uint32_t flags = read_le32(ph + 24);

		if (A630_MDT_HASH(flags)) {
			if (type == 1 || hash_index != UINT32_MAX || file_size == 0) {
				printf("adreno-sdm845: ZAP MDT has duplicate or empty hash metadata\n");
				return false;
			}
			hash_index = i;
			hash_size = file_size;
			continue;
		}
		if (type != 1 || memory_size == 0) {
			continue;
		}
		if (file_size > memory_size) {
			printf("adreno-sdm845: ZAP loadable segment %u has filesz %#x > memsz %#x\n", i, file_size, memory_size);
			return false;
		}
		if ((uint64_t)address + memory_size > UINT32_MAX + 1ull ||
		    polaris_zap_segment_count == A630_MDT_MAX_SEGMENTS) {
			printf("adreno-sdm845: ZAP loadable segment %u has an invalid physical range\n", i);
			return false;
		}
		if (address < min_address) {
			min_address = address;
		}
		uint64_t segment_end = (uint64_t)address + memory_size;
		uint64_t aligned_end = (segment_end + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
		if (aligned_end < segment_end) {
			printf("adreno-sdm845: ZAP loadable segment %u page-alignment overflows\n", i);
			return false;
		}
		if (aligned_end > max_address) {
			max_address = aligned_end;
		}
		if (flags & A630_MDT_RELOCATABLE) {
			relocatable = true;
		}
		const uint8_t *segment = NULL;
		if (file_size != 0) {
			snprintf(name, sizeof(name), "a630_zap.b%02u", i);
			segment = hakios_image_file("fw", name, &file_size, NULL);
			if (segment == NULL || file_size != read_le32(ph + 16)) {
				printf("adreno-sdm845: ZAP segment %u expects fw/%s (%#x bytes), found %llu\n",
				    i, name, read_le32(ph + 16), file_size);
				return false;
			}
			printf("adreno-sdm845: ZAP loadable segment %u: fw/%s, %u bytes at %#x, memory %#x\n",
			    i, name, read_le32(ph + 16), address, memory_size);
		}
		struct polaris_zap_segment *loaded = &polaris_zap_segments[polaris_zap_segment_count++];
		loaded->paddr = address;
		loaded->file_size = read_le32(ph + 16);
		loaded->memory_size = memory_size;
		loaded->flags = flags;
		loaded->data = segment;
		loadable++;
	}
	if (hash_index == UINT32_MAX || loadable == 0 || hash_size > A630_MDT_MAX_BYTES ||
	    (uint64_t)header_size + hash_size > A630_MDT_MAX_BYTES ||
	    (size != header_size && size != (uint64_t)header_size + hash_size) ||
	    min_address == UINT32_MAX || max_address <= min_address || !relocatable ||
	    max_address - min_address > polaris_zap_memory_size || max_address - min_address > UINT32_MAX) {
		printf("adreno-sdm845: ZAP MDT header/hash/load map does not fit the validated PAS memory-region\n");
		return false;
	}
	snprintf(name, sizeof(name), "a630_zap.b%02u", hash_index);
	hash_data = hakios_image_file("fw", name, &file_size, NULL);
	if (hash_data == NULL || file_size != hash_size ||
	    (size == (uint64_t)header_size + hash_size && memcmp(header + header_size, hash_data, hash_size) != 0)) {
		printf("adreno-sdm845: ZAP signed metadata expects fw/%s (%llu bytes); staged copy is missing, truncated or differs\n",
		    name, hash_size);
		return false;
	}
	uint32_t entry = read_le32(header + 24);
	bool entry_found = false;
	for (uint32_t i = 0; i < polaris_zap_segment_count; i++) {
		const struct polaris_zap_segment *segment = &polaris_zap_segments[i];
		uint64_t end = (uint64_t)segment->paddr + segment->memory_size;
		if (entry >= segment->paddr && (uint64_t)entry < end) {
			entry_found = true;
		}
		for (uint32_t j = i + 1; j < polaris_zap_segment_count; j++) {
			const struct polaris_zap_segment *other = &polaris_zap_segments[j];
			uint64_t other_end = (uint64_t)other->paddr + other->memory_size;
			if ((uint64_t)segment->paddr < other_end && (uint64_t)other->paddr < end) {
				printf("adreno-sdm845: ZAP loadable segments %u and %u overlap\n", i, j);
				return false;
			}
		}
	}
	if (!entry_found) {
		printf("adreno-sdm845: ZAP ELF entry point %#x is outside its loadable segments\n", entry);
		return false;
	}
	polaris_zap_mdt = header;
	polaris_zap_hash = hash_data;
	polaris_zap_mdt_size = size;
	polaris_zap_header_size = header_size;
	polaris_zap_hash_size = (uint32_t)hash_size;
	polaris_zap_min_address = min_address;
	polaris_zap_memory_setup_size = (uint32_t)(max_address - min_address);
	polaris_zap_manifest_valid = true;
	printf("adreno-sdm845: ZAP MDT: %u program headers, %u PT_LOAD segment(s), metadata=%u+%u bytes, "
	    "PAS memory=%#llx/%#x bytes at relocatable base %#x\n", phnum, loadable, header_size,
	    (uint32_t)hash_size, polaris_zap_memory_phys, polaris_zap_memory_setup_size, min_address);
	return true;
}

static bool
probe_firmware(void)
{
	uint64_t gmu_size = 0, sqe_size = 0, zap_size = 0;
	const uint8_t *gmu_firmware, *sqe_firmware, *zap_header;
	uint32_t sqe_word0, sqe_word2;
	bool sqe_safe = false, valid = true;

	polaris_sqe_safe = false;
	polaris_sqe_image = NULL;
	polaris_sqe_image_size = 0;
	printf("adreno-sdm845: step 5: inspect staged A630 firmware and validate the runtime PAS memory region\n");
	if (!polaris_runtime_pas_validate()) {
		return false;
	}
	gmu_firmware = hakios_image_file("fw", "a630_gmu.bin", &gmu_size, NULL);
	sqe_firmware = hakios_image_file("fw", "a630_sqe.fw", &sqe_size, NULL);
	zap_header = hakios_image_file("fw", "a630_zap.mdt", &zap_size, NULL);
	if (gmu_firmware == NULL || sqe_firmware == NULL || zap_header == NULL) {
		printf("adreno-sdm845: firmware set is incomplete: GMU=%s SQE=%s ZAP MDT=%s\n",
		    gmu_firmware ? "present" : "missing", sqe_firmware ? "present" : "missing",
		    zap_header ? "present" : "missing");
		return false;
	}
	printf("adreno-sdm845: GMU image=%llu bytes, SQE image=%llu bytes, ZAP MDT=%llu bytes\n",
	    gmu_size, sqe_size, zap_size);
	if (gmu_size == 0 || gmu_size > 0x8000 || (gmu_size & 3u) != 0) {
		printf("adreno-sdm845: GMU raw image is outside the legacy 32-KiB TCM loader bounds\n");
		valid = false;
	}
	/* Linux's adreno_fw_create_bo() strips the first file dword before checking/loading the SQE. */
	if (sqe_size < 16 || sqe_size > 0x40000 || (sqe_size & 3u) != 0) {
		printf("adreno-sdm845: A630 SQE image size/alignment is invalid\n");
		valid = false;
	} else {
		sqe_word0 = read_le32(sqe_firmware + sizeof(uint32_t));
		sqe_word2 = read_le32(sqe_firmware + sizeof(uint32_t) + 2u * sizeof(uint32_t));
		/* Match Linux's post-header a6xx_ucode_check_version() payload and A630 security floor. */
		sqe_safe = (((sqe_word0 & 0xfu) == 0xau && (sqe_word2 & 0xfu) >= 1u) ||
		    ((sqe_word0 & 0xfffu) >= 0x190u));
		polaris_sqe_safe = sqe_safe;
		printf("adreno-sdm845: A630 SQE file prefix=%#x payload version word=%#x patch word=%#x; upstream safety floor %s\n",
		    read_le32(sqe_firmware), sqe_word0, sqe_word2,
		    sqe_safe ? "passes" : "FAILS (need version >= 0x190 or the patched marker)");
		if (sqe_safe) {
			polaris_sqe_image = sqe_firmware;
			polaris_sqe_image_size = sqe_size;
		}
	}
	if (!probe_zap_mdt(zap_header, zap_size)) {
		valid = false;
	}
	if (!valid) {
		printf("adreno-sdm845: firmware structure validation failed; startup remains forbidden\n");
	} else if (!polaris_sqe_safe) {
		printf("adreno-sdm845: GMU/ZAP files are structurally valid, but this SQE cannot be used for user commands\n");
	}
	return valid;
}

/*
 * Load only the legacy A630 GMU image into ITCM. Qualcomm KGSL writes this raw image while CM3
 * is held in reset; releasing reset, programming HFI, and starting firmware are separate stages.
 */
static bool
polaris_gmu_firmware_load(void)
{
	const uint8_t *firmware;
	uint64_t firmware_size = 0;
	uint32_t reset, init_result;

	if (!polaris_gmu_domain_ready || polaris_gpu_core_ready || gmu.virtual == 0) {
		printf("adreno-sdm845: step 10 requires CX/GMU clock on, the 3D core off, and a mapped GMU window\n");
		return false;
	}
	firmware = hakios_image_file("fw", "a630_gmu.bin", &firmware_size, NULL);
	if (firmware == NULL || firmware_size == 0 || firmware_size > A630_GMU_FW_MAX_BYTES ||
	    (firmware_size & 3u) != 0) {
		printf("adreno-sdm845: step 10 refuses missing, unaligned, or oversized A630 GMU firmware (%llu bytes)\n",
		    firmware_size);
		return false;
	}
	init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
	reset = probe_read(&gmu, GMU_CM3_SYSRESET);
	if (init_result == A630_GMU_FW_READY || (reset & 1u) == 0) {
		printf("adreno-sdm845: step 10 refuses to replace a live or non-reset GMU (init=%#x reset=%#x)\n",
		    init_result, reset);
		return false;
	}
	printf("adreno-sdm845: step 10: load %llu bytes of raw A630 GMU firmware to ITCM while CM3 reset remains asserted\n",
	    firmware_size);
	for (uint32_t offset = 0; offset < firmware_size; offset += sizeof(uint32_t)) {
		probe_write(&gmu, GMU_CM3_ITCM_START + offset, read_le32(firmware + offset));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t offset = 0; offset < firmware_size; offset += sizeof(uint32_t)) {
		uint32_t expected = read_le32(firmware + offset);
		uint32_t actual = probe_read(&gmu, GMU_CM3_ITCM_START + offset);

		if (actual != expected) {
			printf("adreno-sdm845: step 10 ITCM readback failed at %#x: expected %#x, got %#x; CM3 remains in reset\n",
			    offset, expected, actual);
			return false;
		}
	}
	polaris_gmu_firmware_loaded = true;
	printf("adreno-sdm845: step 10 verified %llu ITCM bytes; reset is still asserted and GMU/HFI remain stopped\n",
	    firmware_size);
	return true;
}

static bool
polaris_gmu_hfi_send_command(const uint32_t *command, uint32_t command_dwords,
	uint32_t *reply, uint32_t reply_capacity, uint32_t *reply_dwords)
{
	uint32_t message_header, status = UINT32_MAX;

	if (reply_dwords != NULL) {
		*reply_dwords = 0;
	}
	if (polaris_gmu_hfi_sealed) {
		printf("adreno-sdm845: step 14 rejected an HFI command after the A630 legacy HFI_TEST closed the message phase\n");
		return false;
	}
	if (command == NULL || command_dwords == 0 || !polaris_hfi_state.initialized ||
	    (reply == NULL && reply_capacity != 0) || reply_dwords == NULL) {
		printf("adreno-sdm845: step 12 rejected an invalid A630 HFI transaction\n");
		return false;
	}
	message_header = command[0];
	if (!polaris_a630_hfi_write(&polaris_hfi_state, POLARIS_A630_HFI_QUEUE_COMMAND,
	    command, command_dwords)) {
		printf("adreno-sdm845: step 12 could not queue A630 HFI command %#x\n", message_header & 0xffu);
		return false;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&gmu, GMU_HOST2GMU_INTR_SET, POLARIS_A630_HFI_INTR_SEND);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_ACK_POLLS; poll++) {
		uint32_t interrupt = probe_read(&gmu, GMU_GMU2HOST_INTR_INFO);

		if (interrupt & A630_GMU_HFI_MESSAGE_INTERRUPT) {
			probe_write(&gmu, GMU_GMU2HOST_INTR_CLR, A630_GMU_HFI_MESSAGE_INTERRUPT);
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		for (;;) {
			uint32_t response[UINT8_MAX], response_dwords = 0;
			enum polaris_a630_hfi_read_result result = polaris_a630_hfi_read(&polaris_hfi_state,
			    POLARIS_A630_HFI_QUEUE_MESSAGE, response,
			    sizeof(response) / sizeof(response[0]), &response_dwords);

			if (result == POLARIS_A630_HFI_READ_EMPTY) {
				break;
			}
			if (result != POLARIS_A630_HFI_READ_MESSAGE) {
				printf("adreno-sdm845: step 12 received a malformed A630 HFI message (result=%d)\n", result);
				return false;
			}
			if (!polaris_a630_hfi_v1_ack_matches(response, response_dwords, message_header, &status)) {
				printf("adreno-sdm845: step 12 ignored unmatched HFI message header=%#x (%u dwords)\n",
				    response[0], response_dwords);
				continue;
			}
			if (status != 0) {
				printf("adreno-sdm845: step 12 HFI command %#x was rejected with status %#x\n",
				    message_header & 0xffu, status);
				return false;
			}
			if (response_dwords > reply_capacity && reply != NULL) {
				printf("adreno-sdm845: step 12 HFI reply has %u dwords, caller accepts %u\n",
				    response_dwords, reply_capacity);
				return false;
			}
			if (reply != NULL) {
				memcpy(reply, response, response_dwords * sizeof(response[0]));
			}
			*reply_dwords = response_dwords;
			return true;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	printf("adreno-sdm845: step 12 timed out waiting for A630 HFI command %#x acknowledgement (header=%#x)\n",
	    message_header & 0xffu, message_header);
	return false;
}

static bool
polaris_gmu_hfi_send_init(void)
{
	uint32_t message[5], response_dwords = 0;
	uint32_t sequence = polaris_a630_hfi_next_sequence(&polaris_hfi_state);

	if (sequence == 0 || !polaris_a630_hfi_make_init(A630_GMU_DUMP_VA, A630_GMU_HFI_DUMP_BYTES,
	    A630_GMU_BOOT_COLD, (uint16_t)sequence, message) ||
	    !polaris_gmu_hfi_send_command(message, sizeof(message) / sizeof(message[0]), NULL, 0,
	    &response_dwords)) {
		printf("adreno-sdm845: step 12 A630 GMU_INIT failed\n");
		return false;
	}
	if (response_dwords < 3u) {
		printf("adreno-sdm845: step 12 GMU_INIT ACK is too short (%u dwords)\n", response_dwords);
		return false;
	}
	return true;
}

static bool
polaris_gmu_hfi_verify_firmware_version(uint32_t *version)
{
	uint32_t message[2], response[4], response_dwords = 0;
	uint32_t sequence = polaris_a630_hfi_next_sequence(&polaris_hfi_state);
	uint32_t firmware_version, major, minor;

	if (version != NULL) {
		*version = 0;
	}
	if (version == NULL || sequence == 0 ||
	    !polaris_a630_hfi_make_fw_version(POLARIS_A630_GMU_VERSION, (uint16_t)sequence, message) ||
	    !polaris_gmu_hfi_send_command(message, sizeof(message) / sizeof(message[0]), response,
	    sizeof(response) / sizeof(response[0]), &response_dwords)) {
		printf("adreno-sdm845: step 12 could not verify the A630 GMU firmware version over HFI\n");
		return false;
	}
	if (response_dwords < 4u) {
		printf("adreno-sdm845: step 12 GMU firmware-version ACK is too short (%u dwords)\n", response_dwords);
		return false;
	}
	firmware_version = response[3];
	major = (firmware_version >> 28) & 0xfu;
	minor = (firmware_version >> 16) & 0xfffu;
	if (major != POLARIS_A630_GMU_VERSION_MAJOR || minor < POLARIS_A630_GMU_VERSION_MINOR) {
		printf("adreno-sdm845: step 12 incompatible GMU firmware version %#x (need %u.%u or newer in major %u)\n",
		    firmware_version, POLARIS_A630_GMU_VERSION_MAJOR, POLARIS_A630_GMU_VERSION_MINOR,
		    POLARIS_A630_GMU_VERSION_MAJOR);
		return false;
	}
	*version = firmware_version;
	return true;
}

static bool polaris_gmu_hfi_start(void);
static bool probe_pas_capabilities(void);

static bool
polaris_gmu_hfi_start_stage(uint32_t step)
{
	static bool (*const step_probes[])(void) = { probe_gpucc, probe_smmu, probe_gmu, probe_core, probe_firmware,
	    probe_pas_capabilities };

	for (uint32_t i = 0; i < sizeof(step_probes) / sizeof(step_probes[0]); i++) {
		if (!step_probes[i]()) {
			printf("adreno-sdm845: step %u prerequisite %u failed; GMU/HFI is not started\n", step, i + 1u);
			return false;
		}
	}
	if (!polaris_hfi_software_prepare()) {
		printf("adreno-sdm845: step %u could not prepare GMU HFI/dump/log memory; no GMU start was attempted\n", step);
		return false;
	}
	if (!polaris_gmu_firmware_load()) {
		printf("adreno-sdm845: step %u could not load GMU ITCM; reset remains asserted\n", step);
		return false;
	}
	if (!probe_smmu() || !polaris_smmu_gmu_kernel_route.valid || !polaris_smmu_gmu_user_route.valid ||
	    !polaris_smmu_gmu_kernel_attach() || !polaris_smmu_gmu_user_attach()) {
		printf("adreno-sdm845: step %u could not attach both verified GMU SMMUv2 contexts; GMU remains stopped\n", step);
		return false;
	}
	if (polaris_smmu_gpu_route.valid && !polaris_smmu_gpu_attach()) {
		printf("adreno-sdm845: step %u could not attach the clear 3D SID0/CB0 context; GMU remains stopped\n", step);
		return false;
	}
	if (!polaris_gmu_hfi_start()) {
		printf("adreno-sdm845: step %u GMU/HFI startup did not complete\n", step);
		return false;
	}
	return true;
}

static bool
polaris_gmu_hfi_send_perf_table(void)
{
	uint32_t message[POLARIS_A630_HFI_PERF_TABLE_DWORDS];
	uint32_t response_dwords = 0;
	uint32_t sequence = polaris_a630_hfi_next_sequence(&polaris_hfi_state);

	if (sequence == 0 || !polaris_gmu_hfi_perf_table_build((uint16_t)sequence, message) ||
	    !polaris_gmu_hfi_send_command(message, POLARIS_A630_HFI_PERF_TABLE_DWORDS, NULL, 0,
	    &response_dwords)) {
		printf("adreno-sdm845: step 13 A630 HFI PERF_TBL was not acknowledged\n");
		return false;
	}
	if (response_dwords < 3u) {
		printf("adreno-sdm845: step 13 PERF_TBL ACK is too short (%u dwords)\n", response_dwords);
		return false;
	}
	polaris_gmu_hfi_perf_table_ready = true;
	return true;
}

static bool
polaris_gmu_hfi_send_bw_vote_table(void)
{
	struct polaris_a630_hfi_bw_table table;
	uint32_t response_dwords = 0;
	uint32_t sequence = polaris_a630_hfi_next_sequence(&polaris_hfi_state);

	if (sequence == 0 || !polaris_gmu_hfi_bw_table_build((uint16_t)sequence, &table) ||
	    !polaris_gmu_hfi_send_command((const uint32_t *)&table,
	    sizeof(table) / sizeof(uint32_t), NULL, 0, &response_dwords)) {
		printf("adreno-sdm845: step 14 A630 HFI BW_VOTE_TBL was not acknowledged\n");
		return false;
	}
	if (response_dwords < 3u) {
		printf("adreno-sdm845: step 14 BW_VOTE_TBL ACK is too short (%u dwords)\n", response_dwords);
		return false;
	}
	polaris_gmu_hfi_bw_table_ready = true;
	return true;
}

static bool
polaris_gmu_hfi_send_test(void)
{
	uint32_t message[2], response_dwords = 0;
	uint32_t sequence = polaris_a630_hfi_next_sequence(&polaris_hfi_state);

	if (!polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready || sequence == 0 ||
	    !polaris_a630_hfi_make_test((uint16_t)sequence, message) ||
	    !polaris_gmu_hfi_send_command(message, sizeof(message) / sizeof(message[0]), NULL, 0,
	    &response_dwords)) {
		printf("adreno-sdm845: step 14 requires acknowledged A630 performance/bandwidth tables before HFI_TEST\n");
		return false;
	}
	if (response_dwords < 3u) {
		printf("adreno-sdm845: step 14 HFI_TEST ACK is too short (%u dwords)\n", response_dwords);
		return false;
	}
	polaris_gmu_hfi_sealed = true;
	return true;
}

/* A630 HFI_USE_REG targets take their initial GPU/BW level through GMU DCVS registers. */
static bool
polaris_gmu_hfi_set_initial_dcvs(void)
{
	const struct polaris_gpu_operating_point *initial;
	uint32_t perf_index, bw_index, frequency_vote, bandwidth_vote, dcvs_result = UINT32_MAX;
	bool acknowledged = false;

	if (!polaris_gmu_hfi_ready || !polaris_gmu_hfi_sealed || !polaris_gmu_hfi_perf_table_ready ||
	    !polaris_gmu_hfi_bw_table_ready || polaris_gpu_pwrlevel_count < 2u ||
	    polaris_gpu_initial_pwrlevel >= polaris_gpu_pwrlevel_count - 1u ||
	    gmu.virtual == 0) {
		printf("adreno-sdm845: step 14 requires acknowledged A630 PERF_TBL, BW_VOTE_TBL, and HFI_TEST before DCVS\n");
		return false;
	}
	initial = &polaris_gpu_pwrlevels[polaris_gpu_initial_pwrlevel];
	perf_index = polaris_gpu_pwrlevel_count - polaris_gpu_initial_pwrlevel - 1u;
	bw_index = initial->bus_frequency;
	if (perf_index >= POLARIS_A630_HFI_MAX_GPU_LEVELS || bw_index >= polaris_gpu_bus_level_count ||
	    bw_index > 0xffu) {
		printf("adreno-sdm845: step 14 initial GPU/bus index is outside the acknowledged A630 HFI tables\n");
		return false;
	}
	frequency_vote = (perf_index & 0xffu) | (A630_GMU_DCVS_CLKSET_AT_LEAST << 28);
	/* Polaris follows the A630 4.9 HFI_USE_REG layout: BW index occupies bits 0..7. */
	bandwidth_vote = bw_index;
	probe_write(&gmu, GMU_DCVS_ACK_OPTION, A630_GMU_DCVS_ACK_NONBLOCK);
	probe_write(&gmu, GMU_DCVS_PERF_SETTING, frequency_vote);
	probe_write(&gmu, GMU_DCVS_BW_SETTING, bandwidth_vote);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&gmu, GMU_DCVS_ACK_OPTION) != A630_GMU_DCVS_ACK_NONBLOCK ||
	    probe_read(&gmu, GMU_DCVS_PERF_SETTING) != frequency_vote ||
	    probe_read(&gmu, GMU_DCVS_BW_SETTING) != bandwidth_vote) {
		printf("adreno-sdm845: step 14 GMU DCVS register readback failed; no DCVS OOB was sent\n");
		return false;
	}
	probe_write(&gmu, GMU_HOST2GMU_INTR_SET, A630_GMU_OOB_DCVS_SET_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_ACK_POLLS; poll++) {
		if ((probe_read(&gmu, GMU_GMU2HOST_INTR_INFO) & A630_GMU_OOB_DCVS_ACK_MASK) != 0) {
			acknowledged = true;
			break;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	probe_write(&gmu, GMU_GMU2HOST_INTR_CLR, A630_GMU_OOB_DCVS_ACK_MASK);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (acknowledged) {
		dcvs_result = probe_read(&gmu, GMU_DCVS_RETURN);
	}
	polaris_gmu_oob_clear(A630_GMU_OOB_DCVS_ACK_MASK);
	if (!acknowledged || dcvs_result != 0u) {
		printf("adreno-sdm845: step 14 initial GMU DCVS failed (ack=%u result=%#x; pwrlevel=%u perf=%u bus=%u)\n",
		    (unsigned)acknowledged, dcvs_result, polaris_gpu_initial_pwrlevel, perf_index, bw_index);
		return false;
	}
	printf("adreno-sdm845: step 14 initial GPU DCVS accepted (DT powerlevel=%u, HFI perf=%u, bus=%u)\n",
	    polaris_gpu_initial_pwrlevel, perf_index, bw_index);
	return true;
}

static void
polaris_gmu_hfi_stop_after_failure(uint32_t step)
{
	uint32_t reset = UINT32_MAX;

	polaris_gmu_gpu_oob_release();
	if (gmu.virtual != 0) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK | A630_GMU_OOB_DCVS_ACK_MASK);
		probe_write(&gmu, GMU_CM3_SYSRESET, 1u);
		__asm__ volatile ("dsb sy" ::: "memory");
		reset = probe_read(&gmu, GMU_CM3_SYSRESET);
	}
	if (polaris_gmu_temp_bus_vote_active && !polaris_gmu_temp_bus_vote_set(false)) {
		printf("adreno-sdm845: step %u could not confirm release of the temporary Apps RSC DDR vote\n", step);
	}
	polaris_gmu_firmware_started = false;
	polaris_gmu_hfi_ready = false;
	polaris_gmu_hfi_sealed = false;
	polaris_gmu_boot_rail_active = false;
	polaris_gmu_hfi_perf_table_ready = false;
	polaris_gmu_hfi_bw_table_ready = false;
	polaris_hfi_state.initialized = false;
	polaris_smmu_backend_faulted = true;
	printf("adreno-sdm845: step %u failure cleanup requested CM3 reset (readback=%#x); GMU SID5/SID4 contexts remain attached and backend stays faulted\n",
	    step, reset);
}

/* Boot GMU/HFI with the host DDR vote; the GMU-user and GMU-kernel contexts are both active. */
static bool
polaris_gmu_hfi_start(void)
{
	struct polaris_smmu_address_space *space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_KERNEL];
	const struct polaris_smmu_route_candidate *route = &polaris_smmu_gmu_kernel_route;
	struct polaris_smmu_address_space *user_space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GMU_USER];
	const struct polaris_smmu_route_candidate *user_route = &polaris_smmu_gmu_user_route;
	uint32_t reset, init_result, context, s2cr, log_info, hfi_version, firmware_version;

	if (!polaris_gmu_firmware_loaded || polaris_gmu_firmware_started || polaris_gmu_hfi_ready ||
	    polaris_gmu_hfi_sealed ||
	    !polaris_gmu_domain_ready || polaris_gpu_core_ready || !polaris_hfi_space_mapped ||
	    !polaris_hfi_state.initialized || polaris_hfi_state.gmu_address != POLARIS_A630_HFI_GMU_KERNEL_BASE ||
	    !space->context_active || space->context_bank == UINT32_MAX || !route->valid ||
	    route->stream_id != 5u || route->context_bank != space->context_bank ||
	    !user_space->context_active || user_space->context_bank == UINT32_MAX || !user_route->valid ||
	    user_route->stream_id != 4u || user_route->context_bank != user_space->context_bank ||
	    polaris_smmu_backend_faulted || smmu.virtual == 0 || gmu.virtual == 0) {
		printf("adreno-sdm845: step 12 requires verified GMU memory and active SID5/SID4 kernel/user contexts; no GMU start writes were made\n");
		return false;
	}
	reset = probe_read(&gmu, GMU_CM3_SYSRESET);
	init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
	if ((reset & 1u) == 0 || init_result == A630_GMU_FW_READY ||
	    (probe_read(&smmu, SMMU_SCR0) & SMMU_SCR0_CLIENTPD) != 0) {
		printf("adreno-sdm845: step 12 requires a reset, stopped GMU and enabled SMMU client (reset=%#x init=%#x)\n",
		    reset, init_result);
		return false;
	}
	context = polaris_smmu_context_base + space->context_bank * polaris_smmu_context_page_size;
	s2cr = probe_read(&smmu, SMMU_S2CR(route->slot));
	if ((s2cr & (SMMU_S2CR_TYPE_FIELD_MASK | SMMU_S2CR_CBNDX_MASK)) !=
	    ((SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | space->context_bank) ||
	    (probe_read(&smmu, context + SMMU_CB_SCTLR) & SMMU_SCTLR_M) == 0 ||
	    (route->stream_matching &&
	    probe_read(&smmu, SMMU_SMR(route->slot)) !=
	    (SMMU_SMR_VALID | (route->stream_id & SMMU_SMR_ID_MASK)))) {
		printf("adreno-sdm845: step 12 SID5 route or GMU-kernel context changed after attachment; GMU start refused\n");
		return false;
	}
	context = polaris_smmu_context_base + user_space->context_bank * polaris_smmu_context_page_size;
	s2cr = probe_read(&smmu, SMMU_S2CR(user_route->slot));
	if ((s2cr & (SMMU_S2CR_TYPE_FIELD_MASK | SMMU_S2CR_CBNDX_MASK)) !=
	    ((SMMU_S2CR_TYPE_TRANS << SMMU_S2CR_TYPE_SHIFT) | user_space->context_bank) ||
	    (probe_read(&smmu, context + SMMU_CB_SCTLR) & SMMU_SCTLR_M) == 0 ||
	    (user_route->stream_matching &&
	    probe_read(&smmu, SMMU_SMR(user_route->slot)) !=
	    (SMMU_SMR_VALID | (user_route->stream_id & SMMU_SMR_ID_MASK)))) {
		printf("adreno-sdm845: step 12 SID4 route or GMU-user context changed after attachment; GMU start refused\n");
		return false;
	}
	log_info = A630_GMU_LOG_VA | (A630_GMU_LOG_BYTES / POLARIS_SMMU_TABLE_BYTES - 1u);
	printf("adreno-sdm845: step 12: configure TCM/HFI memory and A630 SFR ID; hold the DT-default DDR vote before starting CM3, request GX/MX, then initialize HFI\n");

	polaris_gmu_hfi_perf_table_ready = false;
	polaris_gmu_hfi_bw_table_ready = false;
	/* Qualcomm's TCM boot setup: all shared-memory addresses are in GMU-kernel SID5. */
	probe_write(&gmu, GMU_GENERAL_7, 1u); /* retain GMU TCM */
	probe_write(&gmu, GMU_CM3_FW_INIT_RESULT, 0u);
	probe_write(&gmu, GMU_CM3_BOOT_CONFIG, A630_GMU_BOOT_TCM);
	probe_write(&gmu, GMU_HFI_QTBL_ADDR, polaris_hfi_state.gmu_address);
	probe_write(&gmu, GMU_HFI_QTBL_INFO, 1u);
	probe_write(&gmu, GMU_AHB_FENCE_RANGE_0, A630_GMU_HFI_FENCE_RANGE);
	probe_write(&gmu, GMU_HFI_SFR_ADDR, A630_GMU_HFI_SFR_CHIP_ID);
	probe_write(&gmu, GMU_PWR_COL_CP_RESP, 0u);
	probe_write(&gmu, GMU_PWR_COL_CP_MSG, log_info);
	probe_write(&gmu, GMU_SYS_BUS_CONFIG, 1u);
	probe_write(&gmu, GMU_ICACHE_CONFIG, 1u);
	probe_write(&gmu, GMU_DCACHE_CONFIG, 1u);
	probe_write(&gmu, GMU_PWR_COL_INTER_FRAME_CTRL, 0x09c40400u);
	probe_write(&gmu, GMU_GMU2HOST_INTR_CLR, UINT32_MAX);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&gmu, GMU_CM3_FW_INIT_RESULT) != 0u ||
	    probe_read(&gmu, GMU_CM3_BOOT_CONFIG) != A630_GMU_BOOT_TCM ||
	    probe_read(&gmu, GMU_HFI_QTBL_ADDR) != polaris_hfi_state.gmu_address ||
	    probe_read(&gmu, GMU_HFI_QTBL_INFO) != 1u ||
	    probe_read(&gmu, GMU_HFI_SFR_ADDR) != A630_GMU_HFI_SFR_CHIP_ID ||
	    probe_read(&gmu, GMU_PWR_COL_CP_MSG) != log_info) {
		printf("adreno-sdm845: step 12 GMU boot register readback failed; CM3 remains held in reset\n");
		return false;
	}
	if (!polaris_gmu_temp_bus_vote_set(true)) {
		if (polaris_gmu_temp_bus_vote_active) {
			(void)polaris_gmu_temp_bus_vote_set(false);
		}
		printf("adreno-sdm845: step 12 could not establish the temporary DDR vote; CM3 remains held in reset\n");
		return false;
	}

	probe_write(&gmu, GMU_CM3_SYSRESET, 0u);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_QUEUE_START_POLLS; poll++) {
		init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
		if (init_result == A630_GMU_FW_READY) {
			break;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	init_result = probe_read(&gmu, GMU_CM3_FW_INIT_RESULT);
	if (init_result != A630_GMU_FW_READY) {
		probe_write(&gmu, GMU_CM3_SYSRESET, 1u);
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)polaris_gmu_temp_bus_vote_set(false);
		polaris_smmu_backend_faulted = true;
		printf("adreno-sdm845: step 12 GMU boot timed out or reported failure (init=%#x); CM3 was reset and rendering remains disabled\n",
		    init_result);
		return false;
	}
	polaris_gmu_firmware_started = true;
	if (!polaris_gmu_boot_rail_request()) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK);
		probe_write(&gmu, GMU_CM3_SYSRESET, 1u);
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)polaris_gmu_temp_bus_vote_set(false);
		polaris_gmu_firmware_started = false;
		polaris_smmu_backend_faulted = true;
		return false;
	}
	probe_write(&gmu, GMU_HFI_CTRL_INIT, 1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t poll = 0; poll < A630_GMU_HFI_QUEUE_START_POLLS; poll++) {
		if ((probe_read(&gmu, GMU_HFI_CTRL_STATUS) & 1u) != 0) {
			break;
		}
		delay(A630_GMU_HFI_POLL_DELAY_US);
	}
	if ((probe_read(&gmu, GMU_HFI_CTRL_STATUS) & 1u) == 0) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK);
		polaris_gmu_boot_rail_active = false;
		probe_write(&gmu, GMU_CM3_SYSRESET, 1u);
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)polaris_gmu_temp_bus_vote_set(false);
		polaris_gmu_firmware_started = false;
		polaris_smmu_backend_faulted = true;
		printf("adreno-sdm845: step 12 GMU HFI queue initialization timed out; CM3 was reset\n");
		return false;
	}
	hfi_version = probe_read(&gmu, GMU_HFI_VERSION_INFO);
	if (((hfi_version >> 28) & 0xfu) != 1u || !polaris_gmu_hfi_send_init() ||
	    !polaris_gmu_hfi_verify_firmware_version(&firmware_version)) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK);
		polaris_gmu_boot_rail_active = false;
		probe_write(&gmu, GMU_CM3_SYSRESET, 1u);
		__asm__ volatile ("dsb sy" ::: "memory");
		(void)polaris_gmu_temp_bus_vote_set(false);
		polaris_gmu_firmware_started = false;
		polaris_smmu_backend_faulted = true;
		if (((hfi_version >> 28) & 0xfu) != 1u) {
			printf("adreno-sdm845: step 12 expected legacy HFI v1 for A630, register reports %#x; CM3 was reset\n",
			    hfi_version);
		}
		return false;
	}
	polaris_gmu_hfi_ready = true;
	printf("adreno-sdm845: step 12 GMU firmware and legacy HFI are ready (HFI=%#x GMU=%#x); GMU SID5/SID4 contexts and initial GX/MX rail vote are active%s, while ZAP, CP, and rendering remain disabled\n",
	    hfi_version, firmware_version,
	    polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ? " with the 3D SID0/CB0 context" : "");
	return true;
}

/* A bounded copy of the Qualcomm SCM call convention used by this kernel's other PAS drivers. */
static bool
polaris_scm_call(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
	int32_t *status, uint64_t *answer)
{
	uint64_t session = 0;

	for (uint32_t retry = 0; retry < 100; retry++) {
		register uint64_t x0 __asm__("x0") = retry == 0 ? function : POLARIS_SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != POLARIS_SCM_INTERRUPTED) {
			*status = (int32_t)x0;
			*answer = x1;
			return true;
		}
	}
	return false;
}

static bool
polaris_scm_call_available(uint64_t convention, uint64_t call, const char *name, bool *available)
{
	int32_t status;
	uint64_t answer = 0;

	if (!polaris_scm_call(convention | POLARIS_SCM_IS_CALL_AVAILABLE, POLARIS_SCM_ONE_VALUE,
	    POLARIS_SCM_SIP | call, 0, 0, &status, &answer)) {
		printf("adreno-sdm845: TrustZone availability query for %s stayed interrupted\n", name);
		return false;
	}
	*available = status == 0 && answer == 1;
	printf("adreno-sdm845: TrustZone call %-22s: status=%d answer=%#llx available=%s\n", name, status, answer,
	    *available ? "yes" : "no");
	return true;
}

static bool
probe_pas_capabilities(void)
{
	static const struct {
		uint64_t function;
		const char *name;
	} pas_calls[] = {
		{ POLARIS_SCM_PAS_INIT_IMAGE, "PAS_INIT_IMAGE" },
		{ POLARIS_SCM_PAS_MEM_SETUP, "PAS_MEM_SETUP" },
		{ POLARIS_SCM_PAS_AUTH_RESET, "PAS_AUTH_AND_RESET" },
		{ POLARIS_SCM_PAS_SHUTDOWN, "PAS_SHUTDOWN" },
		{ POLARIS_SCM_PAS_IS_SUPPORTED, "PAS_IS_SUPPORTED" },
	};
	uint64_t convention = POLARIS_SCM_FAST | POLARIS_SCM_64 | POLARIS_SCM_SIP;
	bool query_available = false, calls[sizeof(pas_calls) / sizeof(pas_calls[0])] = { false };
	int32_t status;
	uint64_t answer = 0;

	polaris_pas_calls_ready = false;
	polaris_pas_gpu_supported = false;
	polaris_pas_scm_width = 0;
	printf("adreno-sdm845: step 6: query TrustZone PAS capabilities (no image handoff or GPU writes)\n");
	if (!polaris_scm_call_available(convention, POLARIS_SCM_IS_CALL_AVAILABLE, "IS_CALL_AVAILABLE",
	    &query_available)) {
		return false;
	}
	if (!query_available) {
		convention = POLARIS_SCM_FAST | POLARIS_SCM_SIP;
		polaris_pas_scm_width = 0;
		if (!polaris_scm_call_available(convention, POLARIS_SCM_IS_CALL_AVAILABLE, "IS_CALL_AVAILABLE (32-bit)",
		    &query_available)) {
			return false;
		}
	}
	if (!query_available) {
		printf("adreno-sdm845: this TrustZone does not expose the Linux SCM availability query\n");
		return true;
	}
	if (convention & POLARIS_SCM_64) {
		polaris_pas_scm_width = POLARIS_SCM_64;
	}
	for (uint32_t i = 0; i < sizeof(pas_calls) / sizeof(pas_calls[0]); i++) {
		if (!polaris_scm_call_available(convention, pas_calls[i].function, pas_calls[i].name, &calls[i])) {
			return false;
		}
	}
	polaris_pas_calls_ready = calls[0] && calls[1] && calls[2] && calls[3];
	if (calls[4]) {
		if (!polaris_scm_call(convention | POLARIS_SCM_PAS_IS_SUPPORTED, POLARIS_SCM_ONE_VALUE,
		    POLARIS_PAS_GPU, 0, 0, &status, &answer)) {
			printf("adreno-sdm845: PAS_IS_SUPPORTED for image 13 stayed interrupted\n");
			return false;
		}
		polaris_pas_gpu_supported = status == 0 && answer != 0;
		printf("adreno-sdm845: TrustZone PAS image 13 (GPU ZAP): status=%d supported=%s (answer=%#llx)\n",
		    status, status == 0 && answer != 0 ? "yes" : "no", answer);
	}
	printf("adreno-sdm845: capability queries only; no PAS_INIT_IMAGE, PAS_MEM_SETUP or PAS_AUTH_AND_RESET was issued; "
	    "loader calls=%s\n", polaris_pas_calls_ready ? "available" : "incomplete");
	return true;
}

static bool
probe_rpmh(void)
{
	struct polaris_cmd_db_resource cx_level, gfx_level;
	uint32_t revision, major, minor, solver, drv_config, irq_status, hardware_tcs, commands_per_tcs;
	uint32_t tcs_status_offset, tcs_control_offset, tcs_cmd_enable_offset;

	printf("adreno-sdm845: step 8: inspect Apps RPMh DRV2 and AOP CMD_DB (read-only; no RPMh requests)\n");
	if (polaris_rpmh_drv.physical == 0 || polaris_rpmh_drv.size == 0 || !map_block(&polaris_rpmh_drv)) {
		return false;
	}
	revision = probe_read(&polaris_rpmh_drv, POLARIS_RPMH_DRV_ID);
	major = (revision >> POLARIS_RPMH_ID_MAJOR_SHIFT) & POLARIS_RPMH_ID_VERSION_MASK;
	minor = (revision >> POLARIS_RPMH_ID_MINOR_SHIFT) & POLARIS_RPMH_ID_VERSION_MASK;
	solver = probe_read(&polaris_rpmh_drv, POLARIS_RPMH_DRV_SOLVER_CONFIG);
	drv_config = probe_read(&polaris_rpmh_drv, POLARIS_RPMH_DRV_CONFIG);
	irq_status = probe_read(&polaris_rpmh_drv, polaris_rpmh_tcs_offset + POLARIS_RPMH_TCS_IRQ_STATUS);
	hardware_tcs = (drv_config >> (POLARIS_RPMH_DRV_NUM_TCS_SHIFT * polaris_rpmh_drv_id)) &
	    POLARIS_RPMH_DRV_NUM_TCS_MASK;
	commands_per_tcs = (drv_config >> POLARIS_RPMH_DRV_NCPT_SHIFT) & POLARIS_RPMH_DRV_NCPT_MASK;
	printf("adreno-sdm845: Apps RPMh DRV%u v%u.%u solver=%u hardware TCS=%u/%u commands-per-TCS, "
	    "DT TCS=%u IRQ-status=%#x\n", polaris_rpmh_drv_id, major, minor,
	    (unsigned)!!(solver & (1u << 24)), hardware_tcs, commands_per_tcs, polaris_rpmh_total_tcs, irq_status);
	if (major == 2 && minor == 7) {
		tcs_status_offset = POLARIS_RPMH_TCS_STATUS_V27;
		tcs_control_offset = POLARIS_RPMH_TCS_CONTROL_V27;
		tcs_cmd_enable_offset = POLARIS_RPMH_TCS_CMD_ENABLE_V27;
	} else if (major == 3 && minor == 0) {
		tcs_status_offset = POLARIS_RPMH_TCS_STATUS_V30;
		tcs_control_offset = POLARIS_RPMH_TCS_CONTROL_V30;
		tcs_cmd_enable_offset = POLARIS_RPMH_TCS_CMD_ENABLE_V30;
	} else {
		printf("adreno-sdm845: unsupported RPMh DRV register revision %u.%u; no TCS details are read\n", major, minor);
		return false;
	}
	if (hardware_tcs != polaris_rpmh_total_tcs || commands_per_tcs == 0) {
		printf("adreno-sdm845: RPMh hardware TCS layout disagrees with the validated device tree; no request will be sent\n");
		return false;
	}
	for (uint32_t tcs = 0; tcs < polaris_rpmh_tcs_counts[POLARIS_RPMH_TCS_ACTIVE_TYPE]; tcs++) {
		uint32_t offset = polaris_rpmh_tcs_offset + POLARIS_RPMH_TCS_STRIDE * tcs;
		uint32_t status = probe_read(&polaris_rpmh_drv, offset + tcs_status_offset);
		uint32_t control = probe_read(&polaris_rpmh_drv, offset + tcs_control_offset);
		uint32_t enabled = probe_read(&polaris_rpmh_drv, offset + tcs_cmd_enable_offset);

		printf("adreno-sdm845: RPMh active TCS%u status=%#x%s control=%#x commands=%#x irq=%u\n", tcs, status,
		    status == 0 ? " (busy)" : " (idle)", control, enabled, (unsigned)!!(irq_status & (1u << tcs)));
	}
	if (!polaris_cmd_db_resource_lookup("cx.lvl", &cx_level) ||
	    !polaris_cmd_db_resource_lookup("gfx.lvl", &gfx_level)) {
		printf("adreno-sdm845: CMD_DB lacks a valid CX or GFX ARC level table; GPU power votes remain unavailable\n");
		return false;
	}
	printf("adreno-sdm845: CMD_DB cx.lvl address=%#x levels:", cx_level.address);
	for (uint32_t i = 0; i < cx_level.level_count; i++) {
		printf(" %u", cx_level.levels[i]);
	}
	printf("\n");
	printf("adreno-sdm845: CMD_DB gfx.lvl address=%#x levels:", gfx_level.address);
	for (uint32_t i = 0; i < gfx_level.level_count; i++) {
		printf(" %u", gfx_level.levels[i]);
	}
	printf("\n");
	printf("adreno-sdm845: Polaris GPU pwrlevels (Hz/bus):");
	for (uint32_t i = 0; i < polaris_gpu_pwrlevel_count; i++) {
		printf(" %u:%u/%u", i, polaris_gpu_pwrlevels[i].frequency_hz,
		    polaris_gpu_pwrlevels[i].bus_frequency);
	}
	printf("; GMU pwrlevels (Hz):");
	for (uint32_t i = 0; i < polaris_gmu_pwrlevel_count; i++) {
		printf(" %u:%u", i, polaris_gmu_pwrlevels[i]);
	}
	printf("; grp3d bus usecases (index:AB/IB KB/s):");
	for (uint32_t i = 0; i < polaris_gpu_bus_level_count; i++) {
		printf(" %u:%u/%u", i, polaris_gpu_bus_levels[i].average_kbps,
		    polaris_gpu_bus_levels[i].instantaneous_kbps);
	}
	printf("; GMU CNOC usecases (index:AB/IB KB/s):");
	for (uint32_t i = 0; i < polaris_gmu_cnoc_level_count; i++) {
		printf(" %u:%u/%u", i, polaris_gmu_cnoc_levels[i].average_kbps,
		    polaris_gmu_cnoc_levels[i].instantaneous_kbps);
	}
	printf("\n");
	printf("adreno-sdm845: RPMh discovery complete; no TCS registers or rail votes were written\n");
	return true;
}

static bool
polaris_pas_command(const char *name, uint64_t command, uint64_t arguments, uint64_t first, uint64_t second,
	uint64_t third)
{
	int32_t status = 0;
	uint64_t answer = ~0ull;
	uint64_t function = POLARIS_SCM_SIP | polaris_pas_scm_width | command;

	if (!polaris_scm_call(function, arguments, first, second, third, &status, &answer)) {
		printf("adreno-sdm845: TrustZone PAS %s remained interrupted\n", name);
		return false;
	}
	printf("adreno-sdm845: TrustZone PAS %s returned status=%d answer=%#llx\n", name, status, answer);
	return status == 0 && answer == 0;
}

static void
polaris_pas_metadata_free(void)
{
	if (polaris_pas_metadata_virtual != 0 && polaris_pas_metadata_allocation != 0) {
		kmem_free(kernel_map, polaris_pas_metadata_virtual, polaris_pas_metadata_allocation);
	}
	polaris_pas_metadata_virtual = 0;
	polaris_pas_metadata_allocation = 0;
	polaris_pas_metadata_physical = 0;
}

static bool
polaris_pas_shutdown(void)
{
	if (!polaris_pas_command("PAS_SHUTDOWN", POLARIS_SCM_PAS_SHUTDOWN, POLARIS_SCM_ONE_VALUE,
	    POLARIS_PAS_GPU, 0, 0)) {
		return false;
	}
	polaris_pas_image_active = false;
	polaris_zap_authenticated = false;
	polaris_pas_metadata_free();
	return true;
}

static bool
polaris_zap_pas_load(uint32_t step, bool authenticate)
{
	uint64_t metadata_size = (uint64_t)polaris_zap_header_size + polaris_zap_hash_size;
	vm_size_t metadata_allocation;
	vm_offset_t metadata = 0;
	uint64_t metadata_phys;
	volatile uint8_t *memory;

	printf("adreno-sdm845: step %u: stage A630 ZAP metadata/segments through PAS%s\n", step,
	    authenticate ? " and authenticate/reset image 13" : "; no AUTH_AND_RESET");
	if (!polaris_zap_manifest_valid || !polaris_pas_calls_ready || !polaris_pas_gpu_supported ||
	    polaris_zap_memory_phys == 0 || polaris_zap_memory_size == 0 || polaris_zap_memory_setup_size == 0) {
		printf("adreno-sdm845: step %u refused: firmware manifest, PAS 13 support/calls, or reserved memory is not verified\n", step);
		return false;
	}
	if (polaris_zap_authenticated) {
		if (authenticate && polaris_pas_image_active) {
			printf("adreno-sdm845: step %u ZAP image 13 is already authenticated\n", step);
			return true;
		}
		printf("adreno-sdm845: step %u refused to replace an authenticated ZAP image\n", step);
		return false;
	}
	if (!polaris_gmu_domain_ready || !polaris_gpu_core_ready) {
		printf("adreno-sdm845: step %u refused: CX/GMU clock and GX/GFX3D clock must already be on; this step does not power them\n", step);
		return false;
	}
	if (metadata_size == 0 || metadata_size > A630_MDT_MAX_BYTES ||
	    polaris_zap_memory_setup_size > polaris_zap_memory_size) {
		printf("adreno-sdm845: step %u refused: PAS metadata or relocated image span exceeds validated bounds\n", step);
		return false;
	}
	for (uint32_t i = 0; i < polaris_zap_segment_count; i++) {
		const struct polaris_zap_segment *segment = &polaris_zap_segments[i];
		uint64_t offset = (uint64_t)segment->paddr - polaris_zap_min_address;

		if (segment->paddr < polaris_zap_min_address || offset + segment->memory_size > polaris_zap_memory_setup_size ||
		    segment->file_size > segment->memory_size || (segment->file_size != 0 && segment->data == NULL)) {
			printf("adreno-sdm845: step %u refused: ZAP segment %u escaped the validated memory manifest\n", step, i);
			return false;
		}
	}
	if (polaris_pas_image_active && !polaris_pas_shutdown()) {
		printf("adreno-sdm845: step %u refused: TrustZone did not release any previous GPU PAS image\n", step);
		return false;
	}
	metadata_allocation = (vm_size_t)round_page(metadata_size);
	if (metadata_allocation < metadata_size ||
	    kmem_alloc_contig(kernel_map, &metadata, metadata_allocation, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno-sdm845: step %u could not allocate physically contiguous PAS metadata (%llu bytes)\n", step, metadata_size);
		return false;
	}
	metadata_phys = kvtophys(metadata);
	if ((metadata_phys & PAGE_MASK) != 0) {
		kmem_free(kernel_map, metadata, metadata_allocation);
		printf("adreno-sdm845: step %u PAS metadata is not 4-KiB aligned\n", step);
		return false;
	}
	if (polaris_pas_scm_width == 0 &&
	    (metadata_phys > UINT32_MAX || polaris_zap_memory_phys > UINT32_MAX ||
	    polaris_zap_memory_setup_size > UINT32_MAX - polaris_zap_memory_phys + 1ull)) {
		kmem_free(kernel_map, metadata, metadata_allocation);
		printf("adreno-sdm845: step %u refuses 32-bit SCM addresses that do not fit\n", step);
		return false;
	}
	for (vm_size_t offset = PAGE_SIZE; offset < metadata_allocation; offset += PAGE_SIZE) {
		if (kvtophys(metadata + offset) != metadata_phys + offset) {
			kmem_free(kernel_map, metadata, metadata_allocation);
			printf("adreno-sdm845: step %u PAS metadata allocation is not physically contiguous\n", step);
			return false;
		}
	}
	memcpy((void *)metadata, polaris_zap_mdt, polaris_zap_header_size);
	memcpy((void *)(metadata + polaris_zap_header_size), polaris_zap_hash, polaris_zap_hash_size);
	polaris_cache_lines(metadata, metadata_allocation);
	__asm__ volatile ("dsb sy" ::: "memory");
	polaris_pas_metadata_virtual = metadata;
	polaris_pas_metadata_allocation = metadata_allocation;
	polaris_pas_metadata_physical = metadata_phys;
	polaris_pas_image_active = true;
	if (!polaris_pas_command("PAS_INIT_IMAGE", POLARIS_SCM_PAS_INIT_IMAGE,
	    POLARIS_SCM_VALUE_AND_BUFFER, POLARIS_PAS_GPU, metadata_phys, 0)) {
		(void)polaris_pas_shutdown();
		return false;
	}
	if (!polaris_pas_command("PAS_MEM_SETUP", POLARIS_SCM_PAS_MEM_SETUP,
	    POLARIS_SCM_THREE_VALUES, POLARIS_PAS_GPU, polaris_zap_memory_phys, polaris_zap_memory_setup_size)) {
		(void)polaris_pas_shutdown();
		return false;
	}
	if (polaris_zap_memory_virtual == 0) {
		polaris_zap_memory_virtual = ml_io_map_wcomb((vm_offset_t)polaris_zap_memory_phys,
		    (vm_size_t)polaris_zap_memory_size);
	}
	if (polaris_zap_memory_virtual == 0) {
		printf("adreno-sdm845: step %u could not map the DT-reserved ZAP memory after PAS_MEM_SETUP\n", step);
		(void)polaris_pas_shutdown();
		return false;
	}
	memory = (volatile uint8_t *)polaris_zap_memory_virtual;
	for (uint32_t offset = 0; offset < polaris_zap_memory_setup_size; offset++) {
		memory[offset] = 0;
	}
	for (uint32_t i = 0; i < polaris_zap_segment_count; i++) {
		const struct polaris_zap_segment *segment = &polaris_zap_segments[i];
		uint32_t offset = segment->paddr - polaris_zap_min_address;

		for (uint32_t byte = 0; byte < segment->memory_size; byte++) {
			memory[offset + byte] = byte < segment->file_size ? segment->data[byte] : 0;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (authenticate) {
		if (!polaris_pas_command("PAS_AUTH_AND_RESET", POLARIS_SCM_PAS_AUTH_RESET,
		    POLARIS_SCM_ONE_VALUE, POLARIS_PAS_GPU, 0, 0)) {
			(void)polaris_pas_shutdown();
			return false;
		}
		polaris_zap_authenticated = true;
		polaris_pas_metadata_free();
		printf("adreno-sdm845: step %u authenticated A630 ZAP image 13; PAS remains active until GPU shutdown\n", step);
		return true;
	}
	if (!polaris_pas_shutdown()) {
		printf("adreno-sdm845: step %u could not release the staged PAS image; it remains active for stop-time cleanup\n", step);
		return false;
	}
	printf("adreno-sdm845: step %u PAS staging probe finished; image 13 was shut down without authentication\n", step);
	return true;
}

static bool
stage_zap_shader(void)
{
	return polaris_zap_pas_load(7u, false);
}

static void
sdm845_gpu_stop(void)
{
	struct polaris_smmu_address_space *space = &polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU];
	bool cp_buffers_present = polaris_gpu_sqe_buffer.virtual != 0 || polaris_gpu_ring_buffer.virtual != 0 ||
	    polaris_gpu_rptr_buffer.virtual != 0;

	/* Never remove GX/PAS resources underneath an SQE that has been released from halt. */
	if (polaris_gpu_cp_started || polaris_pas_image_active) {
		printf("adreno-sdm845: GPU stop retains CP/GX/SMMU/PAS state; A630 idle, ring stop, and reset sequencing is not implemented\n");
		return;
	}

	polaris_gmu_gpu_oob_release();
	if (polaris_gmu_boot_rail_active) {
		polaris_gmu_oob_clear(A630_GMU_OOB_BOOT_ACK_MASK);
		polaris_gmu_boot_rail_active = false;
	}
	polaris_gpu_core_configured = false;
	if (polaris_gmu_temp_bus_vote_active && !polaris_gmu_temp_bus_vote_set(false)) {
		printf("adreno-sdm845: GPU stop could not confirm release of the temporary Apps RSC DDR vote\n");
	}
	if (polaris_pas_image_active && !polaris_pas_shutdown()) {
		printf("adreno-sdm845: GPU stop could not shut down PAS image 13; metadata remains allocated\n");
	}
	if (cp_buffers_present) {
		if (space->context_active || polaris_gpu_core_ready) {
			printf("adreno-sdm845: GPU stop retains SQE/CP buffers while the 3D context or core is active\n");
		} else {
			polaris_gpu_dma_buffer_release(&polaris_gpu_rptr_buffer);
			polaris_gpu_dma_buffer_release(&polaris_gpu_ring_buffer);
			polaris_gpu_dma_buffer_release(&polaris_gpu_sqe_buffer);
			printf("adreno-sdm845: GPU stop released detached SQE/CP buffers with the 3D context and core inactive\n");
		}
	}
}

static void
polaris_a630_hwcg_write_range(uint32_t first_word, uint32_t count, uint32_t value)
{
	for (uint32_t i = 0; i < count; i++) {
		probe_write(&gpu, GPU_WORD(first_word + i), value);
	}
}

/* Port of the A630 HWCG table and its GMU SPTPRAC clock sequencing. */
static bool
polaris_a630_hwcg_start(void)
{
	static const struct { uint32_t word; uint32_t value; } fixed[] = {
		{ 0x10b, 0x22222222 }, { 0x10c, 0x22222222 }, { 0x10d, 0x22222222 },
		{ 0x10e, 0x00222222 }, { 0x110, 0x00000004 }, { 0x10f, 0x00000002 },
		{ 0x104, 0x00022022 }, { 0x105, 0x00005550 }, { 0x106, 0x00010011 },
		{ 0x107, 0x00445044 }, { 0x108, 0x04222220 }, { 0x114, 0x00202222 },
		{ 0x111, 0x00002222 }, { 0x10a, 0x00000000 }, { 0x116, 0x04104004 },
		{ 0x113, 0x00000000 }, { 0x11c, 0x00000000 }, { 0x109, 0x00004000 },
		{ 0x115, 0x00000200 }, { 0x112, 0x00002222 }, { 0x117, 0x00000002 },
		{ 0x11b, 0x00002222 }, { 0x118, 0x00000222 }, { 0x119, 0x00000111 },
		{ 0x11a, 0x00000555 },
	};
	uint32_t clock_control, sptprac;

	probe_write(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_GMU_AO_CGC_MODE_WORD), 0x00020222u);
	probe_write(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_GMU_AO_CGC_DELAY_WORD), 0x00010111u);
	probe_write(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_GMU_AO_CGC_HYST_WORD), 0x00050555u);
	clock_control = probe_read(&gpu, GPU_RBBM_CLOCK_CNTL);
	if (clock_control == A630_RBBM_CLOCK_CNTL_ON) {
		return true;
	}
	sptprac = probe_read(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_HWC_GMU_SPTPRAC_WORD));
	probe_write(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_HWC_GMU_SPTPRAC_WORD), sptprac & ~1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 4; i++) {
		polaris_a630_hwcg_write_range(0x0b0u + i, 1u, 0x02222222u);
		polaris_a630_hwcg_write_range(0x0b4u + i, 1u, 0x02022220u);
		polaris_a630_hwcg_write_range(0x0b8u + i, 1u, 0x0000f3cfu);
		polaris_a630_hwcg_write_range(0x0bcu + i, 1u, 0x00000080u);
		polaris_a630_hwcg_write_range(0x0c0u + i, 1u, 0x02222222u);
		polaris_a630_hwcg_write_range(0x0c4u + i, 1u, 0x22222222u);
		polaris_a630_hwcg_write_range(0x0c8u + i, 1u, 0x22222222u);
		polaris_a630_hwcg_write_range(0x0ccu + i, 1u, 0x00022222u);
		polaris_a630_hwcg_write_range(0x0e0u + i, 1u, 0x77777777u);
		polaris_a630_hwcg_write_range(0x0e4u + i, 1u, 0x77777777u);
		polaris_a630_hwcg_write_range(0x0e8u + i, 1u, 0x07777777u);
		polaris_a630_hwcg_write_range(0x0ecu + i, 1u, 0x00077777u);
		polaris_a630_hwcg_write_range(0x0d0u + i, 1u, 0x11111111u);
		polaris_a630_hwcg_write_range(0x0d4u + i, 1u, 0x11111111u);
		polaris_a630_hwcg_write_range(0x0d8u + i, 1u, 0x11111111u);
		polaris_a630_hwcg_write_range(0x0dcu + i, 1u, 0x00011111u);
		polaris_a630_hwcg_write_range(0x0f0u + i, 1u, 0x22222222u);
		polaris_a630_hwcg_write_range(0x0f4u + i, 1u, 0x00002222u);
		polaris_a630_hwcg_write_range(0x0f8u + i, 1u, 0x00000000u);
		polaris_a630_hwcg_write_range(0x100u + i, 1u, 0x00040f00u);
	}
	for (uint32_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
		probe_write(&gpu, GPU_WORD(fixed[i].word), fixed[i].value);
	}
	probe_write(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_HWC_GMU_SPTPRAC_WORD), (sptprac & ~1u) | 1u);
	probe_write(&gpu, GPU_RBBM_CLOCK_CNTL, A630_RBBM_CLOCK_CNTL_ON);
	__asm__ volatile ("dsb sy" ::: "memory");
	return probe_read(&gpu, GPU_RBBM_CLOCK_CNTL) == A630_RBBM_CLOCK_CNTL_ON &&
	    (probe_read(&gmu, GMU_REGISTER_GLOBAL_WORD(A630_HWC_GMU_SPTPRAC_WORD)) & 1u) != 0;
}

static bool
polaris_a630_gpu_core_initialize(void)
{
	DTEntry gpu_node = NULL, iommu_node = NULL;
	const uint8_t *protect;
	unsigned int protect_size = 0;
	uint32_t highest_bank_bit, min_access_length = 32u, ubwc_mode = 0u;
	uint32_t bank_field, mal_field, rb_mode, tpl1_mode, uche_mode;
	uint32_t protect_base, protect_words, protect_range, protect_index, value;

	printf("adreno-sdm845: step 18: apply A630 core HWCG, VBIF, UCHE, CP defaults, and register protection\n");
	if (polaris_gpu_core_configured) {
		printf("adreno-sdm845: step 18 A630 core defaults are already programmed\n");
		return true;
	}
	if (!polaris_gpu_core_ready || !polaris_gpu_oob_active || !polaris_gmu_hfi_ready ||
	    !polaris_gmu_hfi_sealed || !polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready ||
	    !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active || polaris_smmu_backend_faulted ||
	    !map_block(&gpu) || !map_block(&gmu) ||
	    SecureDTLookupEntry(NULL, POLARIS_GPU_DT_PATH, &gpu_node) != kSuccess || gpu_node == NULL ||
	    !polaris_dt_get_u32(gpu_node, "qcom,highest-bank-bit", &highest_bank_bit) ||
	    highest_bank_bit != A630_HIGHEST_BANK_BIT) {
		printf("adreno-sdm845: step 18 requires powered GX/GFX3D, active SID0, the live GMU/HFI handoff, and Polaris A630 DT parameters\n");
		return false;
	}
	if (polaris_dt_has_property(gpu_node, "qcom,min-access-length") &&
	    !polaris_dt_get_u32(gpu_node, "qcom,min-access-length", &min_access_length)) {
		printf("adreno-sdm845: step 18 rejected malformed qcom,min-access-length\n");
		return false;
	}
	if (polaris_dt_has_property(gpu_node, "qcom,ubwc-mode") &&
	    !polaris_dt_get_u32(gpu_node, "qcom,ubwc-mode", &ubwc_mode)) {
		printf("adreno-sdm845: step 18 rejected malformed qcom,ubwc-mode\n");
		return false;
	}
	if ((min_access_length != 0u && min_access_length != 32u && min_access_length != 64u) || ubwc_mode > 2u ||
	    SecureDTLookupEntry(NULL, "/soc/qcom,kgsl-iommu", &iommu_node) != kSuccess || iommu_node == NULL ||
	    SecureDTGetProperty(iommu_node, "qcom,protect", (const void **)&protect, &protect_size) != kSuccess ||
	    protect == NULL || protect_size != 8u || polaris_dt_be32(protect) != 0x40000u ||
	    polaris_dt_be32(protect + 4u) != 0xc000u) {
		printf("adreno-sdm845: step 18 refused unsupported A630 memory attributes or a changed KGSL protect aperture\n");
		return false;
	}
	bank_field = (highest_bank_bit - 13u) & 3u;
	mal_field = min_access_length == 64u ? 1u : 0u;
	rb_mode = (mal_field << 3) | (bank_field << 1) | (ubwc_mode == 1u ? 1u : 0u);
	tpl1_mode = (mal_field << 3) | (bank_field << 1) | (ubwc_mode == 1u ? 1u : 0u);
	uche_mode = (mal_field << 23) | (bank_field << 21);
	protect_base = polaris_dt_be32(protect) / sizeof(uint32_t);
	protect_words = polaris_dt_be32(protect + 4u) / sizeof(uint32_t);
	protect_range = 1u;
	while (protect_range <= protect_words / 2u) {
		protect_range <<= 1;
	}
	if (protect_base != A630_SMMU_PROTECT_BASE_WORD || protect_range != A630_SMMU_PROTECT_RANGE_WORDS ||
	    A630_FIXED_PROTECTED_REGS + 1u > A630_MAX_PROTECTED_REGS) {
		printf("adreno-sdm845: step 18 refused a qcom,protect range that does not fit the A630 protection table\n");
		return false;
	}
	if (!polaris_a630_hwcg_start()) {
		goto fault;
	}

	probe_write(&gpu, GPU_VBIF_GATE_OFF_WRREQ_EN, 0x9u);
	probe_write(&gpu, GPU_RBBM_VBIF_CLIENT_QOS_CNTL, 0x3u);
	probe_write(&gpu, GPU_RBBM_PERFCTR_GPU_BUSY_MASKED, UINT32_MAX);
	probe_write(&gpu, GPU_UCHE_WRITE_RANGE_MAX_LO, 0xffffffc0u);
	probe_write(&gpu, GPU_UCHE_WRITE_RANGE_MAX_HI, 0x0001ffffu);
	probe_write(&gpu, GPU_UCHE_TRAP_BASE_LO, 0xfffff000u);
	probe_write(&gpu, GPU_UCHE_TRAP_BASE_HI, 0x0001ffffu);
	probe_write(&gpu, GPU_UCHE_WRITE_THRU_BASE_LO, 0xfffff000u);
	probe_write(&gpu, GPU_UCHE_WRITE_THRU_BASE_HI, 0x0001ffffu);
	probe_write(&gpu, GPU_UCHE_GMEM_RANGE_MIN_LO, A630_GMEM_BASE);
	probe_write(&gpu, GPU_UCHE_GMEM_RANGE_MIN_HI, 0u);
	probe_write(&gpu, GPU_UCHE_GMEM_RANGE_MAX_LO, A630_GMEM_BASE + A630_GMEM_SIZE - 1u);
	probe_write(&gpu, GPU_UCHE_GMEM_RANGE_MAX_HI, 0u);
	probe_write(&gpu, GPU_UCHE_FILTER_CNTL, 0x804u);
	probe_write(&gpu, GPU_UCHE_CACHE_WAYS, 0x4u);
	probe_write(&gpu, GPU_CP_ROQ_THRESHOLDS_2, 0x010000c0u);
	probe_write(&gpu, GPU_CP_ROQ_THRESHOLDS_1, 0x8040362cu);
	probe_write(&gpu, GPU_CP_MEM_POOL_SIZE, 128u);
	probe_write(&gpu, GPU_PC_DBG_ECO_CNTL, 0x300u << 11);
	probe_write(&gpu, GPU_CP_AHB_CNTL, 0x1u);
	probe_write(&gpu, GPU_RBBM_PERFCTR_CNTL, 0x1u);
	probe_write(&gpu, GPU_RB_NC_MODE_CNTL, rb_mode);
	probe_write(&gpu, GPU_TPL1_NC_MODE_CNTL, tpl1_mode);
	probe_write(&gpu, GPU_SP_NC_MODE_CNTL, tpl1_mode);
	probe_write(&gpu, GPU_UCHE_MODE_CNTL, uche_mode);
	probe_write(&gpu, GPU_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0x001fffffu);
	probe_write(&gpu, GPU_UCHE_CLIENT_PF, 0x1u);

	probe_write(&gpu, GPU_CP_PROTECT_CNTL, 0x3u);
	for (protect_index = 0; protect_index < A630_FIXED_PROTECTED_REGS; protect_index++) {
		const struct polaris_a630_protected_reg *reg = &polaris_a630_protected_regs[protect_index];
		value = reg->base | (reg->count << 18) | (reg->read_protect << 31);
		probe_write(&gpu, GPU_CP_PROTECT_REG + protect_index * sizeof(uint32_t), value);
	}
	value = protect_base | ((protect_range - 1u) << 18) | (1u << 31);
	probe_write(&gpu, GPU_CP_PROTECT_REG + protect_index * sizeof(uint32_t), value);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i <= protect_index; i++) {
		uint32_t expected;

		if (i < A630_FIXED_PROTECTED_REGS) {
			const struct polaris_a630_protected_reg *reg = &polaris_a630_protected_regs[i];
			expected = reg->base | (reg->count << 18) | (reg->read_protect << 31);
		} else {
			expected = value;
		}
		if (probe_read(&gpu, GPU_CP_PROTECT_REG + i * sizeof(uint32_t)) != expected) {
			goto fault;
		}
	}

	if (probe_read(&gpu, GPU_RBBM_CLOCK_CNTL) != A630_RBBM_CLOCK_CNTL_ON ||
	    probe_read(&gpu, GPU_VBIF_GATE_OFF_WRREQ_EN) != 0x9u ||
	    probe_read(&gpu, GPU_RBBM_VBIF_CLIENT_QOS_CNTL) != 0x3u ||
	    probe_read(&gpu, GPU_UCHE_GMEM_RANGE_MIN_LO) != A630_GMEM_BASE ||
	    probe_read(&gpu, GPU_UCHE_GMEM_RANGE_MAX_LO) != A630_GMEM_BASE + A630_GMEM_SIZE - 1u ||
	    probe_read(&gpu, GPU_CP_ROQ_THRESHOLDS_1) != 0x8040362cu ||
	    probe_read(&gpu, GPU_CP_ROQ_THRESHOLDS_2) != 0x010000c0u ||
	    probe_read(&gpu, GPU_CP_PROTECT_CNTL) != 0x3u ||
	    probe_read(&gpu, GPU_RBBM_INTERFACE_HANG_INT_CNTL) != ((1u << 30) | 0x001fffffu)) {
		goto fault;
	}
	polaris_gpu_core_configured = true;
	printf("adreno-sdm845: step 18 programmed A630 HWCG, VBIF, 1-MiB GMEM/UCHE, bank-bit %u, and %u CP-protect descriptors; SQE/CP remain stopped\n",
	    highest_bank_bit, protect_index + 1u);
	return true;

fault:
	polaris_gpu_core_configured = false;
	polaris_smmu_backend_faulted = true;
	printf("adreno-sdm845: step 18 A630 core initialization/readback failed after hardware writes; GPU OOB remains asserted and backend is faulted\n");
	return false;
}

/* Start the single A630 ring and wait for CP_ME_INIT to retire before any secure transition. */
static bool
polaris_a630_cp_ring_start(void)
{
	uint64_t ring_address = polaris_gpu_ring_buffer.gpu_address;
	uint64_t rptr_address = polaris_gpu_rptr_buffer.gpu_address;
	uint64_t sqe_address = polaris_gpu_sqe_buffer.gpu_address;
	uint32_t ring_rptr, ring_wptr, cp_interrupts, cp_fault, protect_status, rbbm_status;

	printf("adreno-sdm845: step 19: start the A630 SQE and submit CP_ME_INIT on the bounded ring\n");
	if (polaris_gpu_cp_initialized) {
		printf("adreno-sdm845: step 19 CP_ME_INIT has already retired\n");
		return true;
	}
	if (polaris_gpu_cp_started || !polaris_gpu_core_ready || !polaris_gpu_core_configured ||
	    !polaris_gpu_oob_active || !polaris_gmu_hfi_ready || !polaris_gmu_hfi_sealed ||
	    !polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready ||
	    !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active || polaris_smmu_backend_faulted ||
	    !map_block(&gpu) || polaris_gpu_sqe_buffer.virtual == 0 ||
	    polaris_gpu_ring_buffer.virtual == 0 || polaris_gpu_rptr_buffer.virtual == 0 ||
	    polaris_gpu_sqe_buffer.requested_size == 0 ||
	    polaris_gpu_ring_buffer.requested_size != A630_CP_RING_BYTES ||
	    polaris_gpu_rptr_buffer.requested_size < sizeof(uint32_t) ||
	    (ring_address & 0xffffu) != 0 || (rptr_address & 0xffffu) != 0 ||
	    (sqe_address & 0xffffu) != 0 || ring_address > UINT32_MAX ||
	    rptr_address > UINT32_MAX || sqe_address > UINT32_MAX ||
	    A630_CP_RING_DWORDS < A630_CP_INIT_PACKET_DWORDS) {
		printf("adreno-sdm845: step 19 requires the configured A630 core, attached SID0, a live GPU OOB request, and aligned 32-bit SQE/ring/RPTR IOVAs\n");
		return false;
	}

	ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
	cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
	protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
	rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
	if (ring_rptr != 0 || ring_wptr != 0 ||
	    (cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 ||
	    protect_status != 0 || (rbbm_status & A630_RBBM_CP_BUSY_MASK) != 0) {
		printf("adreno-sdm845: step 19 refused a non-idle CP before startup: RPTR=%#x WPTR=%#x CP_INT=%#x CP_FAULT=%#x CP_PROTECT=%#x RBBM_STATUS=%#x\n",
		    ring_rptr, ring_wptr, cp_interrupts, cp_fault, protect_status, rbbm_status);
		return false;
	}

	/* The CP fetches only these read-only packets; its RPTR shadow is a separate writable page. */
	*(volatile uint32_t *)polaris_gpu_rptr_buffer.virtual = 0;
	polaris_cache_lines(polaris_gpu_ring_buffer.virtual, polaris_gpu_ring_buffer.allocation_size);
	polaris_cache_lines(polaris_gpu_rptr_buffer.virtual, sizeof(uint32_t));
	__asm__ volatile ("dsb sy" ::: "memory");

	probe_write(&gpu, GPU_CP_RB_RPTR_ADDR_LO, (uint32_t)rptr_address);
	probe_write(&gpu, GPU_CP_RB_RPTR_ADDR_HI, (uint32_t)(rptr_address >> 32));
	probe_write(&gpu, GPU_CP_RB_CNTL, A630_CP_RB_CNTL_DEFAULT);
	probe_write(&gpu, GPU_CP_RB_BASE, (uint32_t)ring_address);
	probe_write(&gpu, GPU_CP_RB_BASE_HI, (uint32_t)(ring_address >> 32));
	probe_write(&gpu, GPU_CP_SQE_INSTR_BASE_LO, (uint32_t)sqe_address);
	probe_write(&gpu, GPU_CP_SQE_INSTR_BASE_HI, (uint32_t)(sqe_address >> 32));
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&gpu, GPU_CP_RB_RPTR_ADDR_LO) != (uint32_t)rptr_address ||
	    probe_read(&gpu, GPU_CP_RB_RPTR_ADDR_HI) != (uint32_t)(rptr_address >> 32) ||
	    probe_read(&gpu, GPU_CP_RB_CNTL) != A630_CP_RB_CNTL_DEFAULT ||
	    probe_read(&gpu, GPU_CP_RB_BASE) != (uint32_t)ring_address ||
	    probe_read(&gpu, GPU_CP_RB_BASE_HI) != (uint32_t)(ring_address >> 32) ||
	    probe_read(&gpu, GPU_CP_SQE_INSTR_BASE_LO) != (uint32_t)sqe_address ||
	    probe_read(&gpu, GPU_CP_SQE_INSTR_BASE_HI) != (uint32_t)(sqe_address >> 32)) {
		goto fault;
	}

	/* Match KGSL's A6xx order: authenticate ZAP before releasing SQE_HALT. */
	if ((!polaris_zap_authenticated && !polaris_zap_pas_load(19u, true)) ||
	    !polaris_zap_authenticated || !polaris_pas_image_active) {
		printf("adreno-sdm845: step 19 requires successful PAS authentication before SQE startup\n");
		return false;
	}
	polaris_gpu_cp_started = true;
	probe_write(&gpu, GPU_CP_SQE_CNTL, 1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&gpu, GPU_CP_RB_WPTR, A630_CP_INIT_PACKET_DWORDS);
	__asm__ volatile ("dsb sy" ::: "memory");
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	if (ring_wptr != A630_CP_INIT_PACKET_DWORDS) {
		goto fault;
	}
	for (uint32_t elapsed = 0; elapsed < A630_CP_INIT_TIMEOUT_US; elapsed += A630_CP_INIT_POLL_US) {
		ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
		cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
		cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
		protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
		rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
		if ((cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 ||
		    protect_status != 0) {
			goto fault;
		}
		if (ring_rptr == A630_CP_INIT_PACKET_DWORDS &&
		    (rbbm_status & A630_RBBM_CP_BUSY_MASK) == 0) {
			polaris_gpu_cp_initialized = true;
			printf("adreno-sdm845: step 19 CP_ME_INIT retired: RPTR=%u CP idle; ZAP is authenticated, secure mode remains enabled, and user submissions remain disabled\n",
			    ring_rptr);
			return true;
		}
		delay(A630_CP_INIT_POLL_US);
	}
	goto fault;

fault:
	polaris_smmu_backend_faulted = true;
	printf("adreno-sdm845: step 19 SQE/CP startup or CP_ME_INIT failed: RPTR=%#x CP_INT=%#x CP_FAULT=%#x CP_PROTECT=%#x RBBM_STATUS=%#x; buffers and GPU power are retained for diagnosis\n",
	    probe_read(&gpu, GPU_CP_RB_RPTR), probe_read(&gpu, GPU_CP_INTERRUPT_STATUS),
	    probe_read(&gpu, GPU_CP_HW_FAULT), probe_read(&gpu, GPU_CP_PROTECT_STATUS),
	    probe_read(&gpu, GPU_RBBM_STATUS));
	return false;
}

/* Leave secure mode only after PAS authenticated ZAP and CP_ME_INIT retired. */
static bool
polaris_a630_cp_set_unsecured_mode(void)
{
	uint32_t cp_init[A630_CP_INIT_PACKET_DWORDS];
	uint32_t secure_mode[A630_CP_SECURE_MODE_DWORDS];
	uint32_t ring_rptr, ring_wptr, cp_interrupts, cp_fault, protect_status, rbbm_status;
	uint32_t packet_offset = A630_CP_INIT_PACKET_DWORDS * sizeof(uint32_t);
	uint32_t target_rptr = A630_CP_INIT_PACKET_DWORDS + A630_CP_SECURE_MODE_DWORDS;

	printf("adreno-sdm845: step 20: submit CP_SET_SECURE_MODE(0) through the authenticated CP ring\n");
	if (polaris_gpu_unsecured_mode) {
		printf("adreno-sdm845: step 20 GPU is already in unsecured mode\n");
		return true;
	}
	if (!polaris_gpu_cp_initialized || !polaris_gpu_cp_started || !polaris_zap_authenticated ||
	    !polaris_pas_image_active || !polaris_gpu_core_ready || !polaris_gpu_core_configured ||
	    !polaris_gpu_oob_active || !polaris_gmu_hfi_ready || !polaris_gmu_hfi_sealed ||
	    !polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready ||
	    !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active || polaris_smmu_backend_faulted ||
	    polaris_gpu_ring_buffer.virtual == 0 ||
	    polaris_gpu_ring_buffer.requested_size != A630_CP_RING_BYTES ||
	    target_rptr >= A630_CP_RING_DWORDS || !map_block(&gpu) ||
	    !polaris_a630_cp_bootstrap_packets(cp_init, sizeof(cp_init) / sizeof(cp_init[0]),
	    secure_mode, sizeof(secure_mode) / sizeof(secure_mode[0]))) {
		printf("adreno-sdm845: step 20 requires authenticated ZAP, an idle initialized CP ring, and the active GX/SID0 handoff\n");
		return false;
	}

	ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
	cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
	protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
	rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
	if (ring_rptr != A630_CP_INIT_PACKET_DWORDS || ring_wptr != A630_CP_INIT_PACKET_DWORDS ||
	    (cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 || protect_status != 0 ||
	    (rbbm_status & A630_RBBM_CP_BUSY_MASK) != 0) {
		goto fault;
	}

	memcpy((void *)(polaris_gpu_ring_buffer.virtual + packet_offset), secure_mode, sizeof(secure_mode));
	polaris_cache_lines(polaris_gpu_ring_buffer.virtual + packet_offset, sizeof(secure_mode));
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&gpu, GPU_CP_RB_WPTR, target_rptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	if (ring_wptr != target_rptr) {
		goto fault;
	}
	for (uint32_t elapsed = 0; elapsed < A630_CP_INIT_TIMEOUT_US; elapsed += A630_CP_INIT_POLL_US) {
		ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
		cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
		cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
		protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
		rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
		if ((cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 || protect_status != 0) {
			goto fault;
		}
		if (ring_rptr == target_rptr && (rbbm_status & A630_RBBM_CP_BUSY_MASK) == 0) {
			polaris_gpu_unsecured_mode = true;
			printf("adreno-sdm845: step 20 CP_SET_SECURE_MODE(0) retired: RPTR=%u CP idle; user submissions remain disabled\n",
			    ring_rptr);
			return true;
		}
		delay(A630_CP_INIT_POLL_US);
	}
	goto fault;

fault:
	polaris_smmu_backend_faulted = true;
	printf("adreno-sdm845: step 20 secure-mode transition or idle check failed: RPTR=%#x WPTR=%#x CP_INT=%#x CP_FAULT=%#x CP_PROTECT=%#x RBBM_STATUS=%#x; user submissions remain disabled\n",
	    probe_read(&gpu, GPU_CP_RB_RPTR), probe_read(&gpu, GPU_CP_RB_WPTR),
	    probe_read(&gpu, GPU_CP_INTERRUPT_STATUS), probe_read(&gpu, GPU_CP_HW_FAULT),
	    probe_read(&gpu, GPU_CP_PROTECT_STATUS), probe_read(&gpu, GPU_RBBM_STATUS));
	return false;
}

/* Validate one kernel-owned write fence through the writable SID0 test page. */
static bool
polaris_a630_cp_fence_smoke_test(void)
{
	uint32_t packet[A630_CP_FENCE_TEST_DWORDS];
	uint32_t ring_rptr, ring_wptr, target_rptr, cp_interrupts, cp_fault, protect_status, rbbm_status;
	uint32_t completed_value;
	uint32_t packet_offset, packet_dwords = 0;
	uint64_t fence_address = polaris_gpu_rptr_buffer.gpu_address + A630_CP_FENCE_TEST_OFFSET;
	vm_offset_t fence_virtual = polaris_gpu_rptr_buffer.virtual + A630_CP_FENCE_TEST_OFFSET;
	volatile uint32_t *fence = (volatile uint32_t *)fence_virtual;

	printf("adreno-sdm845: step 21: verify a bounded CP memory-write fence in the SID0 scratch page\n");
	if (polaris_gpu_fence_smoke_test_done) {
		printf("adreno-sdm845: step 21 CP memory-write fence has already completed\n");
		return true;
	}
	if (!polaris_gpu_unsecured_mode || !polaris_gpu_cp_initialized || !polaris_gpu_cp_started ||
	    !polaris_zap_authenticated || !polaris_pas_image_active || !polaris_gpu_core_ready ||
	    !polaris_gpu_core_configured || !polaris_gpu_oob_active || !polaris_gmu_hfi_ready ||
	    !polaris_gmu_hfi_sealed || !polaris_gmu_hfi_perf_table_ready || !polaris_gmu_hfi_bw_table_ready ||
	    !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ||
	    polaris_smmu_backend_faulted || polaris_gpu_ring_buffer.virtual == 0 ||
	    polaris_gpu_rptr_buffer.virtual == 0 ||
	    polaris_gpu_ring_buffer.requested_size != A630_CP_RING_BYTES ||
	    polaris_gpu_rptr_buffer.requested_size < A630_CP_FENCE_TEST_OFFSET + sizeof(uint32_t) ||
	    !polaris_gpu_ring_buffer.gpu_read_only || polaris_gpu_rptr_buffer.gpu_read_only ||
	    fence_address > UINT32_MAX || (fence_address & (sizeof(uint32_t) - 1u)) != 0 ||
	    A630_CP_FENCE_TEST_OFFSET % POLARIS_SMMU_CACHE_LINE != 0) {
		printf("adreno-sdm845: step 21 requires the authenticated/unsecured CP, active SID0 and a writable fence word separate from the RPTR cache line\n");
		return false;
	}
	if (!map_block(&gpu)) {
		return false;
	}

	ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
	cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
	protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
	rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
	if (ring_rptr != ring_wptr || ring_rptr != A630_CP_INIT_PACKET_DWORDS + A630_CP_SECURE_MODE_DWORDS ||
	    (cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 || protect_status != 0 ||
	    (rbbm_status & A630_RBBM_CP_BUSY_MASK) != 0) {
		goto fault;
	}

	packet_offset = ring_wptr * sizeof(uint32_t);
	packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_MEM_WRITE, 3u);
	packet[packet_dwords++] = (uint32_t)fence_address;
	packet[packet_dwords++] = (uint32_t)(fence_address >> 32);
	packet[packet_dwords++] = A630_CP_FENCE_TEST_VALUE;
	packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_WAIT_MEM_WRITES, 0u);
	if (packet_dwords != A630_CP_FENCE_TEST_DWORDS || packet[0] == 0 || packet[4] == 0 ||
	    ring_wptr + packet_dwords >= A630_CP_RING_DWORDS) {
		goto fault;
	}
	target_rptr = ring_wptr + packet_dwords;
	*fence = 0;
	memcpy((void *)(polaris_gpu_ring_buffer.virtual + packet_offset), packet, sizeof(packet));
	polaris_cache_lines(fence_virtual, sizeof(uint32_t));
	polaris_cache_lines(polaris_gpu_ring_buffer.virtual + packet_offset, sizeof(packet));
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&gpu, GPU_CP_RB_WPTR, target_rptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	if (ring_wptr != target_rptr) {
		goto fault;
	}
	for (uint32_t elapsed = 0; elapsed < A630_CP_INIT_TIMEOUT_US; elapsed += A630_CP_INIT_POLL_US) {
		ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
		cp_interrupts = probe_read(&gpu, GPU_CP_INTERRUPT_STATUS);
		cp_fault = probe_read(&gpu, GPU_CP_HW_FAULT);
		protect_status = probe_read(&gpu, GPU_CP_PROTECT_STATUS);
		rbbm_status = probe_read(&gpu, GPU_RBBM_STATUS);
		polaris_cache_lines(fence_virtual, sizeof(uint32_t));
		__asm__ volatile ("dsb sy" ::: "memory");
		if ((cp_interrupts & A630_CP_INTERRUPT_ERROR_MASK) != 0 || cp_fault != 0 || protect_status != 0) {
			goto fault;
		}
		if (ring_rptr == target_rptr && (rbbm_status & A630_RBBM_CP_BUSY_MASK) == 0 &&
		    *fence == A630_CP_FENCE_TEST_VALUE) {
			completed_value = *fence;
			/* Do not let the smoke-test magic value satisfy the first client fence. */
			*fence = 0;
			polaris_cache_lines(fence_virtual, sizeof(uint32_t));
			__asm__ volatile ("dsb sy" ::: "memory");
			polaris_gpu_ring_write_pointer = target_rptr;
			polaris_gpu_last_fence = 0;
			polaris_gpu_ring_submit_ready = true;
			polaris_gpu_fence_smoke_test_done = true;
			printf("adreno-sdm845: step 21 fence write completed at %#llx with value %#x; RPTR=%u CP idle\n",
			    fence_address, completed_value, ring_rptr);
			return true;
		}
		delay_for_interval(A630_CP_INIT_POLL_US, NSEC_PER_USEC);
	}
	goto fault;

fault:
	polaris_smmu_backend_faulted = true;
	printf("adreno-sdm845: step 21 CP fence submission/readback failed: RPTR=%#x WPTR=%#x fence=%#x CP_INT=%#x CP_FAULT=%#x CP_PROTECT=%#x RBBM_STATUS=%#x; user submissions remain disabled\n",
	    probe_read(&gpu, GPU_CP_RB_RPTR), probe_read(&gpu, GPU_CP_RB_WPTR),
	    (uint32_t)*fence, probe_read(&gpu, GPU_CP_INTERRUPT_STATUS), probe_read(&gpu, GPU_CP_HW_FAULT),
	    probe_read(&gpu, GPU_CP_PROTECT_STATUS), probe_read(&gpu, GPU_RBBM_STATUS));
	return false;
}

/*
 * A630's production A6xx submit stream invalidates both CCU caches, chains each client IB through
 * CP_INDIRECT_BUFFER_PFE, then writes the sequence number through CACHE_FLUSH_TS. The ring is
 * private backend memory; client IOVAs are accepted only from the separate client aperture.
 */
static uint32_t
sdm845_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t packet[A630_CP_SUBMIT_MAX_DWORDS];
	uint32_t packet_dwords = 0, wanted, ring_rptr = 0, ring_wptr, room = 0;
	uint32_t next_wptr, fence;
	uint64_t fence_address;
	vm_offset_t fence_virtual;
	volatile uint32_t *fence_word;

	if (!sdm845_gpu_ready() || !polaris_gpu_ring_submit_ready ||
	    !polaris_gpu_cp_started || !polaris_gpu_cp_initialized || !polaris_gpu_unsecured_mode ||
	    !polaris_gpu_fence_smoke_test_done || !polaris_zap_authenticated ||
	    !polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active || polaris_smmu_backend_faulted ||
	    count > A630_CP_SUBMIT_MAX_IBS || (count != 0 && (addresses == NULL || words == NULL)) ||
	    polaris_gpu_ring_buffer.virtual == 0 || polaris_gpu_rptr_buffer.virtual == 0 ||
	    polaris_gpu_ring_buffer.requested_size != A630_CP_RING_BYTES ||
	    polaris_gpu_rptr_buffer.requested_size < A630_CP_FENCE_TEST_OFFSET + sizeof(uint32_t) ||
	    !polaris_gpu_ring_buffer.gpu_read_only || polaris_gpu_rptr_buffer.gpu_read_only) {
		return 0;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint64_t bytes = (uint64_t)words[i] * sizeof(uint32_t);

		if (words[i] == 0 || (addresses[i] & (sizeof(uint32_t) - 1u)) != 0 ||
		    addresses[i] < POLARIS_GPU_VA_START || addresses[i] >= POLARIS_GPU_VA_END ||
		    bytes > POLARIS_GPU_VA_END - addresses[i]) {
			return 0;
		}
	}
	if (!map_block(&gpu)) {
		return 0;
	}

	ring_wptr = probe_read(&gpu, GPU_CP_RB_WPTR);
	if (ring_wptr >= A630_CP_RING_DWORDS || ring_wptr != polaris_gpu_ring_write_pointer) {
		goto fault;
	}
	wanted = A630_CP_SUBMIT_FIXED_DWORDS + 4u * count;
	for (uint32_t elapsed = 0; elapsed < A630_CP_INIT_TIMEOUT_US; elapsed += A630_CP_INIT_POLL_US) {
		ring_rptr = probe_read(&gpu, GPU_CP_RB_RPTR);
		if (ring_rptr >= A630_CP_RING_DWORDS ||
		    (probe_read(&gpu, GPU_CP_INTERRUPT_STATUS) & A630_CP_INTERRUPT_ERROR_MASK) != 0 ||
		    probe_read(&gpu, GPU_CP_HW_FAULT) != 0 || probe_read(&gpu, GPU_CP_PROTECT_STATUS) != 0) {
			goto fault;
		}
		room = (ring_rptr + A630_CP_RING_DWORDS - ring_wptr - 1u) % A630_CP_RING_DWORDS;
		if (room >= wanted) {
			break;
		}
		delay_for_interval(A630_CP_INIT_POLL_US, NSEC_PER_USEC);
	}
	if (room < wanted) {
		return 0;
	}

	fence = polaris_gpu_last_fence + 1u;
	if (fence == 0) {
		fence = 1u;
	}
	fence_address = polaris_gpu_rptr_buffer.gpu_address + A630_CP_FENCE_TEST_OFFSET;
	fence_virtual = polaris_gpu_rptr_buffer.virtual + A630_CP_FENCE_TEST_OFFSET;
	if (fence_address < POLARIS_GPU_PRIVATE_VA_START || fence_address >= POLARIS_GPU_PRIVATE_VA_END ||
	    (fence_address & (sizeof(uint32_t) - 1u)) != 0 || fence_address > UINT32_MAX) {
		goto fault;
	}
	fence_word = (volatile uint32_t *)fence_virtual;

	packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_EVENT_WRITE, 1u);
	packet[packet_dwords++] = A630_CP_CCU_INVALIDATE_DEPTH;
	packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_EVENT_WRITE, 1u);
	packet[packet_dwords++] = A630_CP_CCU_INVALIDATE_COLOR;
	for (uint32_t i = 0; i < count; i++) {
		packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_INDIRECT_BUFFER_PFE, 3u);
		packet[packet_dwords++] = (uint32_t)addresses[i];
		packet[packet_dwords++] = (uint32_t)(addresses[i] >> 32);
		packet[packet_dwords++] = words[i];
	}
	packet[packet_dwords++] = polaris_a630_type7_packet(A630_CP_EVENT_WRITE, 4u);
	packet[packet_dwords++] = A630_CP_CACHE_FLUSH_TS; /* Poll the memory fence; no IRQ is enabled. */
	packet[packet_dwords++] = (uint32_t)fence_address;
	packet[packet_dwords++] = (uint32_t)(fence_address >> 32);
	packet[packet_dwords++] = fence;
	if (packet_dwords != wanted || packet_dwords > A630_CP_SUBMIT_MAX_DWORDS ||
	    packet[0] == 0 || packet[2] == 0 || packet[packet_dwords - 5u] == 0) {
		goto fault;
	}

	uint32_t first_dwords = packet_dwords < A630_CP_RING_DWORDS - ring_wptr ?
	    packet_dwords : A630_CP_RING_DWORDS - ring_wptr;
	vm_offset_t first = polaris_gpu_ring_buffer.virtual + ring_wptr * sizeof(uint32_t);
	memcpy((void *)first, packet, first_dwords * sizeof(uint32_t));
	polaris_cache_lines(first, first_dwords * sizeof(uint32_t));
	if (first_dwords != packet_dwords) {
		uint32_t second_dwords = packet_dwords - first_dwords;
		memcpy((void *)polaris_gpu_ring_buffer.virtual,
		    packet + first_dwords, second_dwords * sizeof(uint32_t));
		polaris_cache_lines(polaris_gpu_ring_buffer.virtual, second_dwords * sizeof(uint32_t));
	}
	/* The fence cache line is device-owned after startup; clean/invalidate before publishing WPTR. */
	polaris_cache_lines((vm_offset_t)fence_word, sizeof(*fence_word));
	__asm__ volatile ("dsb sy" ::: "memory");
	next_wptr = (ring_wptr + packet_dwords) % A630_CP_RING_DWORDS;
	probe_write(&gpu, GPU_CP_RB_WPTR, next_wptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (probe_read(&gpu, GPU_CP_RB_WPTR) != next_wptr) {
		goto fault;
	}
	polaris_gpu_ring_write_pointer = next_wptr;
	polaris_gpu_last_fence = fence;
	return fence;

fault:
	polaris_smmu_backend_faulted = true;
	polaris_gpu_ring_submit_ready = false;
	printf("adreno-sdm845: A630 CP submit failed or ring state changed: RPTR=%#x WPTR=%#x CP_INT=%#x CP_FAULT=%#x CP_PROTECT=%#x; backend faulted and hardware/resources are retained\n",
	    probe_read(&gpu, GPU_CP_RB_RPTR), probe_read(&gpu, GPU_CP_RB_WPTR),
	    probe_read(&gpu, GPU_CP_INTERRUPT_STATUS), probe_read(&gpu, GPU_CP_HW_FAULT),
	    probe_read(&gpu, GPU_CP_PROTECT_STATUS));
	return 0;
}

static bool
sdm845_gpu_done(uint32_t fence)
{
	volatile uint32_t *fence_word;

	if (fence == 0) {
		return true;
	}
	if (!sdm845_gpu_ready() || !polaris_gpu_ring_submit_ready || polaris_smmu_backend_faulted ||
	    polaris_gpu_rptr_buffer.virtual == 0 || (int32_t)(polaris_gpu_last_fence - fence) < 0) {
		return false;
	}
	fence_word = (volatile uint32_t *)(polaris_gpu_rptr_buffer.virtual + A630_CP_FENCE_TEST_OFFSET);
	polaris_cache_lines((vm_offset_t)fence_word, sizeof(*fence_word));
	__asm__ volatile ("dsb sy" ::: "memory");
	return (int32_t)(*fence_word - fence) >= 0;
}

static uint32_t
sdm845_gpu_last(void)
{
	return polaris_gpu_last_fence;
}

static void
sdm845_gpu_report(const char *why)
{
	uint32_t completed_fence = 0;
	volatile uint32_t *fence_word;

	if (why == NULL) {
		why = "unspecified failure";
	}
	if (!map_block(&gpu)) {
		printf("adreno-sdm845: GPU report: %s; MMIO window unavailable, last fence=%u\n", why,
		    polaris_gpu_last_fence);
		return;
	}
	if (polaris_gpu_ring_submit_ready && polaris_gpu_rptr_buffer.virtual != 0) {
		fence_word = (volatile uint32_t *)(polaris_gpu_rptr_buffer.virtual + A630_CP_FENCE_TEST_OFFSET);
		polaris_cache_lines((vm_offset_t)fence_word, sizeof(*fence_word));
		__asm__ volatile ("dsb sy" ::: "memory");
		completed_fence = *fence_word;
	}
	printf("adreno-sdm845: GPU report: %s; last fence=%u done=%u RPTR=%#x WPTR=%#x software WPTR=%u status=%#x interrupt=%#x CP fault=%#x CP interrupt=%#x protection=%#x\n",
	    why, polaris_gpu_last_fence, completed_fence,
	    probe_read(&gpu, GPU_CP_RB_RPTR), probe_read(&gpu, GPU_CP_RB_WPTR),
	    polaris_gpu_ring_write_pointer, probe_read(&gpu, GPU_RBBM_STATUS),
	    probe_read(&gpu, GPU_RBBM_INT_0_STATUS), probe_read(&gpu, GPU_CP_HW_FAULT),
	    probe_read(&gpu, GPU_CP_INTERRUPT_STATUS), probe_read(&gpu, GPU_CP_PROTECT_STATUS));
}

static bool
polaris_a630_gpu_domain_start(uint32_t step)
{
	static bool (*const step_probes[])(void) = { probe_gpucc, probe_smmu, probe_gmu, probe_core,
	    probe_firmware, probe_pas_capabilities };

	for (uint32_t i = 0; i < sizeof(step_probes) / sizeof(step_probes[0]); i++) {
		if (!step_probes[i]()) {
			printf("adreno-sdm845: step %u prerequisite %u failed; GMU/GX startup is not attempted\n", step, i + 1u);
			return false;
		}
	}
	if (polaris_gpu_core_ready || polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ||
	    polaris_smmu_backend_faulted || !polaris_gpu_cp_software_prepare()) {
		printf("adreno-sdm845: step %u requires an off GPU core, detached SID0 context, and prepared A630 CP buffers\n", step);
		return false;
	}
	if (!polaris_gmu_hfi_start_stage(step)) {
		return false;
	}
	if (!polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ||
	    !polaris_gmu_hfi_send_perf_table() || !polaris_gmu_hfi_send_bw_vote_table() ||
	    !polaris_gmu_hfi_send_test() || !polaris_gmu_hfi_set_initial_dcvs() ||
	    !polaris_gmu_temp_bus_vote_set(false)) {
		polaris_gmu_hfi_stop_after_failure(step);
		printf("adreno-sdm845: step %u could not attach SID0 or complete GMU bandwidth/DCVS handoff; GX startup remains disabled\n", step);
		return false;
	}
	if (!polaris_gmu_gpu_oob_request()) {
		polaris_gmu_hfi_stop_after_failure(step);
		printf("adreno-sdm845: step %u GPU OOB request failed; no A6xx core or CP registers were written\n", step);
		return false;
	}
	if (!probe_core() || !polaris_gpu_core_ready) {
		polaris_gmu_hfi_stop_after_failure(step);
		printf("adreno-sdm845: step %u GMU acknowledged GPU power, but GX/GFX3D readback failed; CP remains stopped\n", step);
		return false;
	}
	return true;
}

void
hakios_adreno_sdm845_init(void)
{
	uint32_t last_step = 0, gpu_enabled = 0;
	bool requested_probe;
	static bool (*const probes[])(void) = { probe_gpucc, probe_smmu, probe_gmu, probe_core, probe_firmware,
	    probe_pas_capabilities, stage_zap_shader };

	if (pe_arm_get_soc_base_phys() != SDM845_SOC_BASE) {
		return;
	}
	polaris_gpu_va_init();
	requested_probe = PE_parse_boot_argn("adreno", &last_step, sizeof(last_step)) && last_step != 0;
	PE_parse_boot_argn("gpu", &gpu_enabled, sizeof(gpu_enabled));
	if ((requested_probe || gpu_enabled) && !polaris_runtime_dt_validate()) {
		return;
	}
	if (!requested_probe) {
		if (gpu_enabled) {
			printf("adreno-sdm845: GPU rendering is not enabled: the A630 backend is at read-only bring-up stage\n");
		}
		return;
	}
	if (last_step == 8) {
		printf("adreno-sdm845: step 8 is a separate read-only RPMh/CMD_DB probe; it runs steps 1..6 and skips PAS staging step 7\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; RPMh registers and CMD_DB are not read\n", i + 1);
				return;
			}
		}
		if (!probe_rpmh()) {
			printf("adreno-sdm845: step 8 failed; no RPMh request was sent\n");
			return;
		}
		printf("adreno-sdm845: read-only RPMh discovery finished; no GPU acceleration was started\n");
		return;
	}
	if (last_step == 9) {
		printf("adreno-sdm845: step 9 prepares a detached GMU-kernel HFI software map; it runs steps 1..6 and skips PAS staging step 7\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; HFI memory and page tables are not prepared\n", i + 1);
				return;
			}
		}
		if (!polaris_hfi_software_prepare()) {
			printf("adreno-sdm845: step 9 failed; the SMMU context and GMU remain untouched\n");
			return;
		}
		printf("adreno-sdm845: detached HFI software preparation finished; no SMMU context, HFI message, GMU startup, or rendering was enabled\n");
		return;
	}
	if (last_step == 10) {
		printf("adreno-sdm845: step 10 explicitly loads the raw A630 GMU image into ITCM; it runs read-only GPUCC/GMU/core/firmware checks and does not release reset or start HFI\n");
		if (!probe_gpucc() || !probe_gmu() || !probe_core() || !probe_firmware()) {
			printf("adreno-sdm845: a read-only precondition failed; GMU ITCM is not written\n");
			return;
		}
		if (!polaris_gmu_firmware_load()) {
			printf("adreno-sdm845: step 10 failed; the GMU remains stopped\n");
			return;
		}
		printf("adreno-sdm845: step 10 finished; firmware is loaded but the GMU is not running and rendering remains disabled\n");
		return;
	}
	if (last_step == 11) {
		printf("adreno-sdm845: step 11 explicitly attaches separate GMU-kernel SID5 and GMU-user SID4 SMMUv2 contexts; it runs steps 1..6, prepares both roots and HFI tables, rechecks SMMU state, then writes CB/SID registers\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; neither GMU SMMUv2 context is attached\n", i + 1);
				return;
			}
		}
		if (!polaris_hfi_software_prepare()) {
			printf("adreno-sdm845: step 11 could not prepare HFI tables; the SMMU remains unchanged\n");
			return;
		}
		if (!probe_smmu() || !polaris_smmu_gmu_kernel_route.valid || !polaris_smmu_gmu_user_route.valid) {
			printf("adreno-sdm845: step 11 could not refresh both GMU attach candidates; the SMMU remains unchanged\n");
			return;
		}
		if (!polaris_smmu_gmu_kernel_attach() || !polaris_smmu_gmu_user_attach()) {
			printf("adreno-sdm845: step 11 did not attach both GMU contexts; HFI and firmware remain stopped\n");
			return;
		}
		if (polaris_smmu_gpu_route.valid && !polaris_smmu_gpu_attach()) {
			printf("adreno-sdm845: step 11 failed to attach the eligible 3D SID0/CB0 context; GMU firmware/HFI remain stopped\n");
			return;
		}
		printf("adreno-sdm845: step 11 attached GMU SID5/SID4 to separate contexts%s; GMU firmware/HFI, ZAP, and rendering remain disabled\n",
		    polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ? " plus the 3D SID0/CB0 context" :
		    "; SID0 had no eligible blank route");
		return;
	}
	if (last_step == 12) {
		printf("adreno-sdm845: step 12 explicitly boots GMU firmware and the first legacy HFI command; it runs steps 1..6, prepares HFI/dump/log memory and both GMU roots, loads ITCM, refreshes SID4/SID5 routing, attaches both contexts, then starts HFI\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; GMU startup is not attempted\n", i + 1);
				return;
			}
		}
		if (!polaris_hfi_software_prepare()) {
			printf("adreno-sdm845: step 12 could not prepare GMU HFI/dump/log memory; no GMU start was attempted\n");
			return;
		}
		if (!polaris_gmu_firmware_load()) {
			printf("adreno-sdm845: step 12 could not load GMU ITCM; reset remains asserted\n");
			return;
		}
		if (!probe_smmu() || !polaris_smmu_gmu_kernel_route.valid || !polaris_smmu_gmu_user_route.valid ||
		    !polaris_smmu_gmu_kernel_attach() || !polaris_smmu_gmu_user_attach()) {
			printf("adreno-sdm845: step 12 could not attach both verified GMU SMMUv2 contexts; GMU remains stopped\n");
			return;
		}
		if (polaris_smmu_gpu_route.valid && !polaris_smmu_gpu_attach()) {
			printf("adreno-sdm845: step 12 could not attach the clear 3D SID0/CB0 context; GMU remains stopped\n");
			return;
		}
		if (!polaris_gmu_hfi_start()) {
			printf("adreno-sdm845: step 12 GMU/HFI startup did not complete; the A630 backend remains disabled\n");
			return;
		}
		printf("adreno-sdm845: step 12 finished; GMU SID5/SID4 contexts%s, the initial GX/MX rail vote, and temporary Apps RSC DDR vote are active; GPU rendering remains disabled\n",
		    polaris_smmu_spaces[POLARIS_SMMU_SPACE_GPU].context_active ? " plus 3D SID0/CB0" : "");
		return;
	}
	if (last_step == 13) {
		printf("adreno-sdm845: step 13 explicitly boots the GMU and sends only HFI PERF_TBL; it runs steps 1..6, prepares HFI memory and both GMU roots, loads GMU ITCM, attaches SID4/SID5, then starts HFI and submits GPU/GMU ARC performance levels\n");
		if (!polaris_gmu_hfi_start_stage(13u)) {
			return;
		}
		if (!polaris_gmu_hfi_send_perf_table()) {
			polaris_gmu_hfi_stop_after_failure(13u);
			printf("adreno-sdm845: step 13 failed; BW table, GX core, CP, and rendering remain disabled\n");
			return;
		}
		printf("adreno-sdm845: step 13 finished; GMU PERF_TBL and the initial GX/MX rail vote are acknowledged, while BW_VOTE_TBL, default DCVS, ZAP authentication, CP submission, and rendering remain disabled; A630 uses legacy HFI, not the A640-only HFI START command\n");
		return;
	}
	if (last_step == 14) {
		printf("adreno-sdm845: step 14 boots GMU/HFI, sends A630 PERF_TBL and BW_VOTE_TBL, closes the HFI message phase with HFI_TEST, then requests the DT-default GPU/bus DCVS level through OOB registers; rendering stays disabled\n");
		if (!polaris_gmu_hfi_start_stage(14u)) {
			return;
		}
		if (!polaris_gmu_hfi_send_perf_table() || !polaris_gmu_hfi_send_bw_vote_table() ||
		    !polaris_gmu_hfi_send_test() || !polaris_gmu_hfi_set_initial_dcvs() ||
		    !polaris_gmu_temp_bus_vote_set(false)) {
			polaris_gmu_hfi_stop_after_failure(14u);
			printf("adreno-sdm845: step 14 failed; legacy HFI/default DCVS setup did not complete; ZAP authentication, CP submission, and rendering remain disabled\n");
			return;
		}
		printf("adreno-sdm845: step 14 finished; PERF_TBL, BW_VOTE_TBL, HFI_TEST, and default DCVS are acknowledged, and the temporary host DDR vote is released; the boot rail request remains active for later GPU start, while ZAP authentication, CP submission, and rendering remain disabled\n");
		return;
	}
	if (last_step == 15) {
		printf("adreno-sdm845: step 15 runs read-only probes 1..6 and constructs A630 CP bootstrap packets in local memory only\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; CP bootstrap packets were not prepared\n", i + 1);
				return;
			}
		}
		if (!prepare_a630_cp_bootstrap_packets()) {
			printf("adreno-sdm845: step 15 failed; GPU/SMMU/PAS hardware remains untouched\n");
			return;
		}
		printf("adreno-sdm845: step 15 finished; command payloads are prepared for review, but SQE/CP are stopped and rendering remains disabled\n");
		return;
	}
	if (last_step == 16) {
		printf("adreno-sdm845: step 16 runs read-only probes 1..6, then prepares SQE, a 32-KiB CP ring, and RPTR scratch in the detached GPU SID0 page table\n");
		for (uint32_t i = 0; i < 6; i++) {
			if (!probes[i]()) {
				printf("adreno-sdm845: step %u failed; detached CP buffers were not prepared\n", i + 1);
				return;
			}
		}
		if (!polaris_gpu_cp_software_prepare()) {
			printf("adreno-sdm845: step 16 failed; SMMU context, GPU registers, and CP remain untouched\n");
			return;
		}
		printf("adreno-sdm845: step 16 finished; detached buffer mappings are ready for review, but SID0 is not attached, the SQE/CP remain stopped, and rendering remains disabled\n");
		return;
	}
	if (last_step == 17) {
		printf("adreno-sdm845: step 17 explicitly maps the A630 CP buffers, starts GMU/HFI and requests GX power; A6xx core programming and SQE/CP startup remain disabled\n");
		if (!polaris_a630_gpu_domain_start(17u)) {
			return;
		}
		printf("adreno-sdm845: step 17 finished; GMU/HFI tables and initial DCVS are acknowledged, SID0 maps the SQE/ring/RPTR buffers, and GX/GFX3D is on; GPU OOB keeps the core awake, but A6xx core setup, SQE/CP, ZAP authentication, and rendering remain disabled\n");
		return;
	}
	if (last_step == 18) {
		printf("adreno-sdm845: step 18 performs guarded GMU/GX startup then applies A630 core initialization and CP register protection; SQE/CP remain stopped\n");
		if (!polaris_a630_gpu_domain_start(18u) || !polaris_a630_gpu_core_initialize()) {
			return;
		}
		printf("adreno-sdm845: step 18 finished; A630 core defaults and protected-register ranges are programmed, GPU OOB remains asserted, and SQE/CP, ZAP authentication, and rendering remain disabled\n");
		return;
	}
	if (last_step == 19) {
		printf("adreno-sdm845: step 19 performs guarded GMU/GX/core setup, authenticates ZAP before SQE startup, then waits for CP_ME_INIT idle; secure mode and submissions remain disabled\n");
		if (!polaris_a630_gpu_domain_start(19u) || !polaris_a630_gpu_core_initialize() ||
		    !polaris_a630_cp_ring_start()) {
			return;
		}
		printf("adreno-sdm845: step 19 finished; SQE is running, CP_ME_INIT retired, and ZAP is authenticated; secure-mode transition, fences, and rendering remain disabled\n");
		return;
	}
	if (last_step == 20) {
		printf("adreno-sdm845: step 20 performs guarded GMU/GX/core setup, authenticates ZAP, initializes CP, then leaves secure mode and waits idle; user submissions remain disabled\n");
		if (!polaris_a630_gpu_domain_start(20u) || !polaris_a630_gpu_core_initialize() ||
		    !polaris_a630_cp_ring_start() || !polaris_a630_cp_set_unsecured_mode()) {
			return;
		}
		printf("adreno-sdm845: step 20 finished; ZAP is authenticated, CP is idle in unsecured mode, but fences, ordinary submissions, and rendering remain disabled\n");
		return;
	}
	if (last_step == 21) {
		printf("adreno-sdm845: step 21 performs guarded GMU/GX/core/ZAP/CP setup, leaves secure mode, then writes one kernel-owned fence word; no user command path is enabled\n");
		if (!polaris_a630_gpu_domain_start(21u) || !polaris_a630_gpu_core_initialize() ||
		    !polaris_a630_cp_ring_start() || !polaris_a630_cp_set_unsecured_mode() ||
		    !polaris_a630_cp_fence_smoke_test()) {
			return;
		}
		printf("adreno-sdm845: step 21 finished; the bounded kernel fence test retired, but the GPU backend remains disabled for client submissions\n");
		return;
	}
	if (last_step > 21u) {
		printf("adreno-sdm845: supported explicit stages end at adreno=21; requested step %u is refused\n", last_step);
		return;
	}
	printf("adreno-sdm845: Polaris Adreno 630 bring-up, steps 1..%u; step 7 stages ZAP only when explicitly requested\n",
	    last_step);
	for (uint32_t i = 0; i < last_step; i++) {
		if (!probes[i]()) {
			printf("adreno-sdm845: step %u failed; later steps are not attempted\n", i + 1);
			return;
		}
	}
	if (last_step >= 7) {
		printf("adreno-sdm845: ZAP PAS stage finished; GPU acceleration remains disabled\n");
	} else {
		printf("adreno-sdm845: read-only diagnostic finished; no GPU acceleration was started\n");
	}
}
