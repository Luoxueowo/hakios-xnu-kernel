/*
 * Bring-up and firmware loading for the Adreno 630 of the Xiaomi MIX 2S (Polaris, SDM845).
 * This is deliberately a separate backend from the Redwood/SC7280 Adreno 642L driver: the
 * GPUCC and KGSL SMMU are different blocks, and the latter is SMMUv2 rather than SMMUv500.
 *
 * adreno=1..6 performs read-only probes. Explicit adreno=7 exercises PAS metadata setup, segment
 * staging, and shutdown after runtime DT, firmware, capability, and powered CX/GX preconditions
 * match. It does not authenticate/start ZAP until the secure GPU SMMU route is verified. The
 * backend's dormant SMMUv2 operations cannot attach a context or start rendering; `ready()` stays
 * false until the A630 GMU, power sequence, live SMMU route, command processor, and shared
 * HakiosGPU path are implemented and exercised.
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
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

#include "hakios.gpu_backend.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

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
#define POLARIS_SMMU_DT_PATH            "/soc/arm,smmu-kgsl@5040000"
#define POLARIS_PIL_GPU_DT_PATH         "/soc/qcom,kgsl-hyp"
#define POLARIS_GPU_MEMORY_DT_PATH      "/reserved-memory/gpu_region@0x8c415000"
#define POLARIS_GPU_CHIP_ID             0x06030001u
#define A630_MDT_MAX_SEGMENTS           16u
#define A630_MDT_MAX_BYTES              0x10000u
#define A630_MDT_HASH(flags)            ((((flags) >> 24) & 7u) == 2u)
#define A630_MDT_RELOCATABLE            (1u << 27)
#define POLARIS_PAS_GPU                 13u

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

/* SDM845 Apps RSC DRV2; resource windows and TCS counts come from the live DT. */
#define POLARIS_RPMH_DRV_ID                0x0000u
#define POLARIS_RPMH_DRV_SOLVER_CONFIG    0x0004u
#define POLARIS_RPMH_DRV_CONFIG           0x000cu
#define POLARIS_RPMH_ID_MAJOR_SHIFT       16u
#define POLARIS_RPMH_ID_MINOR_SHIFT       8u
#define POLARIS_RPMH_ID_VERSION_MASK      0xffu
#define POLARIS_RPMH_TCS_IRQ_STATUS       0x0004u
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
#define SMMU_SMR(n)                     (0x0800u + 4u * (n))
#define SMMU_SMR_VALID                  (1u << 31)
#define SMMU_S2CR(n)                    (0x0c00u + 4u * (n))
#define SMMU_S2CR_TYPE_SHIFT            16
#define SMMU_S2CR_CBNDX_MASK            0xffu
#define SMMU_GR1                       0x1000u
#define SMMU_CBAR(n)                    (SMMU_GR1 + 4u * (n))
#define SMMU_CBA2R(n)                   (SMMU_GR1 + 0x0800u + 4u * (n))
#define SMMU_CB_SCTLR                   0x0000u
#define SMMU_CB_TCR2                    0x0010u
#define SMMU_CB_TTBR0                   0x0020u
#define SMMU_CB_TCR                     0x0030u
#define SMMU_CB_MAIR0                   0x0038u
#define SMMU_CB_FSR                     0x0058u
#define SMMU_CB_TLBIASID                0x0610u
#define SMMU_CB_TLBSYNC                 0x07f0u
#define SMMU_CB_TLBSTATUS               0x07f4u
#define SMMU_SCTLR_M                    (1u << 0)

/* A6xx GMU offsets are in 32-bit words in the register XML. */
#define GMU_WORD(n)                     (4u * (n))
#define GMU_ALWAYS_ON_COUNTER_L         GMU_WORD(0x5088)
#define GMU_ALWAYS_ON_COUNTER_H         GMU_WORD(0x5089)
#define GMU_CM3_FW_INIT_RESULT          GMU_WORD(0x501c)
#define GMU_HFI_VERSION_INFO            GMU_WORD(0x5181)

/* A6xx GPU offsets are in 32-bit words in the register XML. */
#define GPU_WORD(n)                     (4u * (n))
#define GPU_RBBM_INT_0_STATUS           GPU_WORD(0x0201)
#define GPU_RBBM_STATUS                 GPU_WORD(0x0210)
#define GPU_CP_HW_FAULT                 GPU_WORD(0x0821)

/*
 * A630 advertises a 32-bit GPU VA aperture. Reserve its lower GiB for backend-owned
 * firmware/ring mappings and hand the remaining 3 GiB to the shared buffer service.
 * These are software IOVAs; no SMMU translation is installed until the backend is ready.
 */
#define POLARIS_GPU_VA_START             0x40000000ull
#define POLARIS_GPU_VA_END               0x100000000ull
#define POLARIS_GPU_VA_ALIGN             0x10000ull
#define POLARIS_GPU_VA_GRANULES          ((POLARIS_GPU_VA_END - POLARIS_GPU_VA_START) / POLARIS_GPU_VA_ALIGN)
#define POLARIS_GPU_VA_MAX_RANGES        ((POLARIS_GPU_VA_GRANULES + 1) / 2)
#define POLARIS_GPU_VA_MAX_ALLOCS        POLARIS_GPU_VA_GRANULES
#define POLARIS_SMMU_MAX_STREAM_IDS      8u
#define POLARIS_SMMU_TABLE_BYTES         4096u
#define POLARIS_SMMU_MAX_TABLES          2053u /* one L1, four L2, 2048 L3 tables cover 4 GiB */
#define POLARIS_SMMU_PTE_TABLE           3ull
#define POLARIS_SMMU_PTE_PAGE            3ull
#define POLARIS_SMMU_PTE_UNPRIVILEGED    (1ull << 6)
#define POLARIS_SMMU_PTE_READ_ONLY       (1ull << 7)
#define POLARIS_SMMU_PTE_SHARED          (2ull << 8)
#define POLARIS_SMMU_PTE_ACCESSED        (1ull << 10)
#define POLARIS_SMMU_PTE_NOT_GLOBAL      (1ull << 11)
#define POLARIS_SMMU_PTE_ADDRESS         0x0000fffffffff000ull
#define POLARIS_SMMU_PTE_CORE            (POLARIS_SMMU_PTE_UNPRIVILEGED | POLARIS_SMMU_PTE_SHARED | \
					      POLARIS_SMMU_PTE_ACCESSED | POLARIS_SMMU_PTE_NOT_GLOBAL)
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
static uint32_t polaris_gpu_va_free_count;
static uint32_t polaris_gpu_va_allocated_count;
static bool polaris_gpu_va_initialized;

struct polaris_smmu_table {
	uint64_t *entries;
	uint64_t phys;
	bool dirty;
};

static struct polaris_smmu_table polaris_smmu_tables[POLARIS_SMMU_MAX_TABLES];
static uint32_t polaris_smmu_table_count;
static vm_offset_t polaris_smmu_table_chunk;
static uint32_t polaris_smmu_chunk_slots;
static struct polaris_smmu_table *polaris_smmu_root;
static struct polaris_smmu_table *polaris_smmu_cached_l2;
static struct polaris_smmu_table *polaris_smmu_cached_l3;
static uint32_t polaris_smmu_cached_l1_index = UINT32_MAX;
static uint32_t polaris_smmu_cached_l2_index = UINT32_MAX;
static uint32_t polaris_smmu_oas_bits;
static bool polaris_smmu_supports_4k;
static uint32_t polaris_smmu_context_page_size;
static uint32_t polaris_smmu_context_base;
static uint32_t polaris_smmu_context_bank_count;
static uint32_t polaris_smmu_core_context;
static uint32_t polaris_smmu_core_asid;
static uint32_t polaris_smmu_stream_ids[POLARIS_SMMU_MAX_STREAM_IDS];
static uint32_t polaris_smmu_stream_count;
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
static bool polaris_gmu_domain_ready;
static bool polaris_gpu_core_ready;
static bool polaris_smmu_core_context_active;
static bool polaris_smmu_backend_enabled;
static bool polaris_smmu_backend_faulted;
static bool polaris_sqe_safe;

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
static void sdm845_gpu_stop(void);

static bool
sdm845_gpu_ready(void)
{
	return polaris_smmu_backend_enabled && !polaris_smmu_backend_faulted && polaris_sqe_safe;
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
	uint32_t gpu_offset;

	if (!polaris_dt_has_empty_property(smmu_node, "qcom,dynamic") ||
	    !polaris_dt_has_empty_property(smmu_node, "qcom,use-3-lvl-tables") ||
	    SecureDTLookupEntry(NULL, "/soc/qcom,kgsl-iommu", &kgsl_iommu_node) != kSuccess || kgsl_iommu_node == NULL ||
	    !polaris_dt_has_compatible(kgsl_iommu_node, "qcom,kgsl-smmu-v2") ||
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
	printf("; gfx3d_user GPU offset=%#x; three-level SMMU tables required\n", gpu_offset);
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
	printf("adreno-sdm845: runtime DT matches the SDM845 GPU/GFXCC clock tree, GPU/GMU supply domains, and CX/MX/GFX rails\n");
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
polaris_smmu_tlbi_core(void)
{
	uint64_t context_offset;
	uint32_t context;

	if (!polaris_smmu_core_context_active) {
		return true;
	}
	context_offset = (uint64_t)polaris_smmu_context_base +
	    (uint64_t)polaris_smmu_core_context * polaris_smmu_context_page_size;
	if (smmu.virtual == 0 || polaris_smmu_context_page_size == 0 ||
	    polaris_smmu_core_context >= polaris_smmu_context_bank_count ||
	    context_offset + polaris_smmu_context_page_size > smmu.size || context_offset > UINT32_MAX) {
		return false;
	}
	context = (uint32_t)context_offset;
	__asm__ volatile ("dsb sy" ::: "memory");
	probe_write(&smmu, context + SMMU_CB_TLBIASID, polaris_smmu_core_asid);
	probe_write(&smmu, context + SMMU_CB_TLBSYNC, 1);
	for (uint32_t retry = 0; retry < 10000; retry++) {
		if ((probe_read(&smmu, context + SMMU_CB_TLBSTATUS) & 1u) == 0) {
			__asm__ volatile ("dsb sy" ::: "memory");
			return true;
		}
	}
	printf("adreno-sdm845: SMMU context %u did not finish TLB invalidation\n", polaris_smmu_core_context);
	return false;
}

static bool
sdm845_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	uint32_t l1_index, l2_index, l3_index;
	struct polaris_smmu_table *l2, *l3;
	uint64_t descriptor;
	bool ok = false;

	if (address >= POLARIS_GPU_VA_END || (address & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    (phys & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    !polaris_smmu_phys_valid(phys, POLARIS_SMMU_TABLE_BYTES)) {
		return false;
	}
	l1_index = (uint32_t)(address >> 30) & 0x1ffu;
	l2_index = (uint32_t)(address >> 21) & 0x1ffu;
	l3_index = (uint32_t)(address >> 12) & 0x1ffu;

	lck_mtx_lock(&polaris_smmu_pt_lock);
	if (polaris_smmu_root == NULL && (polaris_smmu_root = polaris_smmu_table_new()) == NULL) {
		goto out;
	}
	if (polaris_smmu_cached_l1_index == l1_index && polaris_smmu_cached_l2 != NULL &&
	    polaris_smmu_root->entries[l1_index] == (polaris_smmu_cached_l2->phys | POLARIS_SMMU_PTE_TABLE)) {
		l2 = polaris_smmu_cached_l2;
	} else {
		l2 = polaris_smmu_table_child(polaris_smmu_root, l1_index, true);
		polaris_smmu_cached_l1_index = l1_index;
		polaris_smmu_cached_l2_index = UINT32_MAX;
		polaris_smmu_cached_l2 = l2;
		polaris_smmu_cached_l3 = NULL;
	}
	if (l2 == NULL) {
		goto out;
	}
	if (polaris_smmu_cached_l1_index == l1_index && polaris_smmu_cached_l2_index == l2_index &&
	    polaris_smmu_cached_l3 != NULL &&
	    l2->entries[l2_index] == (polaris_smmu_cached_l3->phys | POLARIS_SMMU_PTE_TABLE)) {
		l3 = polaris_smmu_cached_l3;
	} else {
		l3 = polaris_smmu_table_child(l2, l2_index, true);
		polaris_smmu_cached_l2_index = l2_index;
		polaris_smmu_cached_l3 = l3;
	}
	if (l3 == NULL || l3->entries[l3_index] != 0) {
		goto out;
	}
	descriptor = phys | POLARIS_SMMU_PTE_CORE | POLARIS_SMMU_PTE_PAGE;
	if (read_only) {
		descriptor |= POLARIS_SMMU_PTE_READ_ONLY;
	}
	l3->entries[l3_index] = descriptor;
	l3->dirty = true;
	ok = true;
out:
	lck_mtx_unlock(&polaris_smmu_pt_lock);
	return ok;
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
	if (size == 0 || address >= POLARIS_GPU_VA_END || (address & (POLARIS_SMMU_TABLE_BYTES - 1)) != 0 ||
	    size > POLARIS_GPU_VA_END - address) {
		return;
	}
	lck_mtx_lock(&polaris_smmu_pt_lock);
	for (uint64_t offset = 0; offset < size; offset += POLARIS_SMMU_TABLE_BYTES) {
		uint64_t iova = address + offset;
		uint32_t l1_index = (uint32_t)(iova >> 30) & 0x1ffu;
		uint32_t l2_index = (uint32_t)(iova >> 21) & 0x1ffu;
		uint32_t l3_index = (uint32_t)(iova >> 12) & 0x1ffu;
		struct polaris_smmu_table *l2, *l3;

		if (polaris_smmu_root == NULL) {
			break;
		}
		if (polaris_smmu_cached_l1_index == l1_index && polaris_smmu_cached_l2 != NULL &&
		    polaris_smmu_root->entries[l1_index] == (polaris_smmu_cached_l2->phys | POLARIS_SMMU_PTE_TABLE)) {
			l2 = polaris_smmu_cached_l2;
		} else {
			l2 = polaris_smmu_table_child(polaris_smmu_root, l1_index, false);
			polaris_smmu_cached_l1_index = l1_index;
			polaris_smmu_cached_l2_index = UINT32_MAX;
			polaris_smmu_cached_l2 = l2;
			polaris_smmu_cached_l3 = NULL;
		}
		if (l2 == NULL) {
			continue;
		}
		if (polaris_smmu_cached_l1_index == l1_index && polaris_smmu_cached_l2_index == l2_index &&
		    polaris_smmu_cached_l3 != NULL &&
		    l2->entries[l2_index] == (polaris_smmu_cached_l3->phys | POLARIS_SMMU_PTE_TABLE)) {
			l3 = polaris_smmu_cached_l3;
		} else {
			l3 = polaris_smmu_table_child(l2, l2_index, false);
			polaris_smmu_cached_l2_index = l2_index;
			polaris_smmu_cached_l3 = l3;
		}
		if (l3 != NULL && l3->entries[l3_index] != 0) {
			l3->entries[l3_index] = 0;
			l3->dirty = true;
		}
	}
	polaris_smmu_commit_locked();
	if (!polaris_smmu_tlbi_core()) {
		/* A timed-out invalidation prevents safe IOVA reuse; keep the backend disabled. */
		polaris_smmu_backend_faulted = true;
		printf("adreno-sdm845: A630 backend disabled after SMMU TLB invalidation failure\n");
	}
	lck_mtx_unlock(&polaris_smmu_pt_lock);
}

static void
sdm845_gpu_to_memory(void *start, uint64_t size)
{
	polaris_cache_lines((vm_offset_t)start, size);
	__asm__ volatile ("dsb sy" ::: "memory");
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

	for (uint32_t i = 0; i < context_banks; i++) {
		uint32_t cbar = probe_read(&smmu, SMMU_CBAR(i));
		uint32_t cba2r = probe_read(&smmu, SMMU_CBA2R(i));
		uint32_t base = (uint32_t)context_base + page_size * i;
		uint32_t sctlr = probe_read(&smmu, base + SMMU_CB_SCTLR);
		uint32_t ttbr0_lo = probe_read(&smmu, base + SMMU_CB_TTBR0);
		uint32_t ttbr0_hi = probe_read(&smmu, base + SMMU_CB_TTBR0 + 4u);
		uint32_t tcr = probe_read(&smmu, base + SMMU_CB_TCR);
		uint32_t tcr2 = probe_read(&smmu, base + SMMU_CB_TCR2);
		uint32_t mair0 = probe_read(&smmu, base + SMMU_CB_MAIR0);
		uint32_t fsr = probe_read(&smmu, base + SMMU_CB_FSR);

		if (cbar == 0 && cba2r == 0 && sctlr == 0 && ttbr0_lo == 0 && ttbr0_hi == 0 && tcr == 0 &&
		    tcr2 == 0 && mair0 == 0 && fsr == 0) {
			continue;
		}
		active++;
		printf("adreno-sdm845: SMMU CB%u: CBAR=%#x CBA2R=%#x SCTLR=%#x%s TTBR0=%#x%08x TCR=%#x "
		    "TCR2=%#x MAIR0=%#x FSR=%#x\n", i, cbar, cba2r, sctlr,
		    (sctlr & SMMU_SCTLR_M) ? " (translation on)" : "", ttbr0_hi, ttbr0_lo, tcr, tcr2, mair0, fsr);
	}
	printf("adreno-sdm845: SMMU context-bank window: page=%u bytes, base=%#llx, %u banks, %u nonzero\n",
	    page_size, context_base, context_banks, active);
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
	uint32_t cx, memnoc, idr0, idr1, idr2, idr7;
	uint32_t stream_groups, context_banks, s2_context_banks, sid_bits;
	uint32_t page_shift, major, minor;

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
	printf("adreno-sdm845: SMMU IDR0=%#x IDR1=%#x IDR2=%#x IDR7=%#x\n", idr0, idr1, idr2, idr7);
	stream_groups = idr0 & SMMU_IDR0_NUMSMRG_MASK;
	context_banks = idr1 & SMMU_IDR1_NUMCB_MASK;
	s2_context_banks = (idr1 >> SMMU_IDR1_NUMS2CB_SHIFT) & SMMU_IDR1_NUMCB_MASK;
	sid_bits = (idr0 & SMMU_IDR0_EXIDS) ? 16 :
	    ((idr0 >> SMMU_IDR0_NUMSIDB_SHIFT) & SMMU_IDR0_NUMSIDB_MASK);
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
	polaris_smmu_supports_4k = (idr0 & SMMU_IDR0_S1TS) != 0 && (idr2 & SMMU_IDR2_PTFS_4K) != 0;
	printf("adreno-sdm845: SMMU stage-1 table prerequisites: 4K=%s, OAS=%u bits\n",
	    polaris_smmu_supports_4k ? "supported" : "unavailable", polaris_smmu_oas_bits);
	if (idr0 & SMMU_IDR0_SMS) {
		if (stream_groups == 0) {
			printf("adreno-sdm845: SMMU advertises stream matching but reports no SMR groups\n");
		} else if (stream_groups > (POLARIS_SMMU_SIZE - SMMU_S2CR(0)) / sizeof(uint32_t)) {
			printf("adreno-sdm845: SMMU stream-match count %#x exceeds the mapped register window; entries are not read\n",
			    stream_groups);
		} else {
			for (uint32_t i = 0; i < stream_groups; i++) {
				uint32_t smr = probe_read(&smmu, SMMU_SMR(i));
				uint32_t s2cr;

				if (!(smr & SMMU_SMR_VALID)) {
					continue;
				}
				s2cr = probe_read(&smmu, SMMU_S2CR(i));
				printf("adreno-sdm845: SMMU SMR[%u]=%#x S2CR=%#x type=%u context-bank=%u\n", i, smr, s2cr,
				    (s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u, s2cr & SMMU_S2CR_CBNDX_MASK);
			}
		}
	} else if (sid_bits <= 16) {
		uint32_t sid_limit = 1u << sid_bits;

		for (uint32_t i = 0; i < polaris_smmu_stream_count; i++) {
			uint32_t sid = polaris_smmu_stream_ids[i];
			uint32_t s2cr;

			if (sid >= sid_limit) {
				continue;
			}
			s2cr = probe_read(&smmu, SMMU_S2CR(sid));
			printf("adreno-sdm845: SMMU SID %u S2CR=%#x type=%u context-bank=%u\n", sid, s2cr,
			    (s2cr >> SMMU_S2CR_TYPE_SHIFT) & 3u, s2cr & SMMU_S2CR_CBNDX_MASK);
		}
	}
	probe_smmu_contexts(idr1);
	return true;
}

static bool
probe_gmu(void)
{
	uint32_t cx, clock;
	uint32_t high1 = 0, high2 = 0, low = 0, init_result, hfi_version;
	bool counter_stable = false;

	polaris_gmu_domain_ready = false;
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
polaris_cmd_db_resource_lookup(const char *name, struct polaris_cmd_db_resource *resource)
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
			if (aux_length == 0 || (aux_length & 1u) != 0 || aux_length > POLARIS_CMD_DB_MAX_LEVELS * 2u ||
			    aux_start > POLARIS_CMD_DB_SIZE || aux_length > POLARIS_CMD_DB_SIZE - aux_start) {
				printf("adreno-sdm845: CMD_DB resource %s has invalid level metadata\n", name);
				return false;
			}
			resource->address = read_le32(entry + 16);
			for (uint32_t level_index = 0; level_index < aux_length / 2u; level_index++) {
				uint16_t level = read_le16(polaris_cmd_db + aux_start + level_index * 2u);

				if (level_index != 0 && level == 0) {
					break;
				}
				resource->levels[level_index] = level;
				resource->level_count++;
			}
			return resource->address != 0 && resource->level_count != 0 && resource->levels[0] != 0;
		}
	}
	return false;
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
	uint32_t sqe_word0;
	bool sqe_safe = false, valid = true;

	polaris_sqe_safe = false;
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
	if (sqe_size < 12 || sqe_size > 0x40000 || (sqe_size & 3u) != 0) {
		printf("adreno-sdm845: A630 SQE image size/alignment is invalid\n");
		valid = false;
	} else {
		sqe_word0 = read_le32(sqe_firmware);
		/* Match Linux a6xx_ucode_check_version()'s A630 security and WHERE_AM_I floor. */
		sqe_safe = ((((sqe_word0 & 0xfu) == 0xau) && ((read_le32(sqe_firmware + 8) & 0xfu) >= 1u)) ||
		    ((sqe_word0 & 0xfffu) >= 0x190u));
		polaris_sqe_safe = sqe_safe;
		printf("adreno-sdm845: A630 SQE first word=%#x; upstream safety floor %s\n", sqe_word0,
		    sqe_safe ? "passes" : "FAILS (need version >= 0x190 or the patched marker)");
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
	polaris_pas_metadata_free();
	return true;
}

static bool
stage_zap_shader(void)
{
	uint64_t metadata_size = (uint64_t)polaris_zap_header_size + polaris_zap_hash_size;
	vm_size_t metadata_allocation;
	vm_offset_t metadata = 0;
	uint64_t metadata_phys;
	volatile uint8_t *memory;

	printf("adreno-sdm845: step 7: stage A630 ZAP metadata/segment through PAS, then shut it down (explicit PAS writes)\n");
	if (!polaris_zap_manifest_valid || !polaris_pas_calls_ready || !polaris_pas_gpu_supported ||
	    polaris_zap_memory_phys == 0 || polaris_zap_memory_size == 0 || polaris_zap_memory_setup_size == 0) {
		printf("adreno-sdm845: step 7 refused: firmware manifest, PAS 13 support/calls, or reserved memory is not verified\n");
		return false;
	}
	if (!polaris_gmu_domain_ready || !polaris_gpu_core_ready) {
		printf("adreno-sdm845: step 7 refused: CX/GMU clock and GX/GFX3D clock must already be on; this step does not power them\n");
		return false;
	}
	if (metadata_size == 0 || metadata_size > A630_MDT_MAX_BYTES ||
	    polaris_zap_memory_setup_size > polaris_zap_memory_size) {
		printf("adreno-sdm845: step 7 refused: PAS metadata or relocated image span exceeds validated bounds\n");
		return false;
	}
	for (uint32_t i = 0; i < polaris_zap_segment_count; i++) {
		const struct polaris_zap_segment *segment = &polaris_zap_segments[i];
		uint64_t offset = (uint64_t)segment->paddr - polaris_zap_min_address;

		if (segment->paddr < polaris_zap_min_address || offset + segment->memory_size > polaris_zap_memory_setup_size ||
		    segment->file_size > segment->memory_size || (segment->file_size != 0 && segment->data == NULL)) {
			printf("adreno-sdm845: step 7 refused: ZAP segment %u escaped the validated memory manifest\n", i);
			return false;
		}
	}
	if (!polaris_pas_shutdown()) {
		printf("adreno-sdm845: step 7 refused: TrustZone did not release any previous GPU PAS image\n");
		return false;
	}
	metadata_allocation = (vm_size_t)round_page(metadata_size);
	if (metadata_allocation < metadata_size ||
	    kmem_alloc_contig(kernel_map, &metadata, metadata_allocation, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adreno-sdm845: step 7 could not allocate physically contiguous PAS metadata (%llu bytes)\n", metadata_size);
		return false;
	}
	metadata_phys = kvtophys(metadata);
	if ((metadata_phys & PAGE_MASK) != 0) {
		kmem_free(kernel_map, metadata, metadata_allocation);
		printf("adreno-sdm845: step 7 PAS metadata is not 4-KiB aligned\n");
		return false;
	}
	if (polaris_pas_scm_width == 0 &&
	    (metadata_phys > UINT32_MAX || polaris_zap_memory_phys > UINT32_MAX ||
	    polaris_zap_memory_setup_size > UINT32_MAX - polaris_zap_memory_phys + 1ull)) {
		kmem_free(kernel_map, metadata, metadata_allocation);
		printf("adreno-sdm845: step 7 refuses 32-bit SCM addresses that do not fit\n");
		return false;
	}
	for (vm_size_t offset = PAGE_SIZE; offset < metadata_allocation; offset += PAGE_SIZE) {
		if (kvtophys(metadata + offset) != metadata_phys + offset) {
			kmem_free(kernel_map, metadata, metadata_allocation);
			printf("adreno-sdm845: step 7 PAS metadata allocation is not physically contiguous\n");
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
		printf("adreno-sdm845: step 7 could not map the DT-reserved ZAP memory after PAS_MEM_SETUP\n");
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
	if (!polaris_pas_shutdown()) {
		printf("adreno-sdm845: step 7 could not release the staged PAS image; it remains active for stop-time cleanup\n");
		return false;
	}
	printf("adreno-sdm845: PAS accepted and released the A630 ZAP staging area; AUTH_AND_RESET is deferred until the secure SMMU route is verified\n");
	return true;
}

static void
sdm845_gpu_stop(void)
{
	if (polaris_pas_image_active && !polaris_pas_shutdown()) {
		printf("adreno-sdm845: GPU stop could not shut down PAS image 13; metadata remains allocated\n");
	}
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
	if (last_step > sizeof(probes) / sizeof(probes[0])) {
		printf("adreno-sdm845: steps 1..6 are read-only, step 7 stages ZAP through PAS, and step 8 is the separate "
		    "read-only RPMh probe; "
		    "requested step %u is capped at %zu\n",
		    last_step, sizeof(probes) / sizeof(probes[0]));
		last_step = sizeof(probes) / sizeof(probes[0]);
	}
	printf("adreno-sdm845: Polaris Adreno 630 bring-up, steps 1..%u; step 7 authenticates ZAP only when explicitly requested\n",
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
