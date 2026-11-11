/*
 * Read-only bring-up probes for the Adreno 630 of the Xiaomi MIX 2S (Polaris, SDM845).
 * This is deliberately a separate backend from the Redwood/SC7280 Adreno 642L driver: the
 * GPUCC and KGSL SMMU are different blocks, and the latter is SMMUv2 rather than SMMUv500.
 *
 * Only adreno=1..4 runs these reads. No register is written here. `gpu=1` is not supported by
 * this probe; the rendering backend is not enabled until the A630 firmware, power sequence,
 * SMMUv2, command processor and shared HakiosGPU path are implemented and exercised.
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
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "hakios.gpu_backend.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);

#define SDM845_SOC_BASE                 0x00100000ul
#define POLARIS_GPUCC_PHYS              0x05090000ul
#define POLARIS_GPUCC_SIZE              0x00009000ul
#define POLARIS_GMU_PHYS                0x0506a000ul
#define POLARIS_GMU_SIZE                0x00030000ul
#define POLARIS_SMMU_PHYS               0x05040000ul
#define POLARIS_SMMU_SIZE               0x00010000ul
#define POLARIS_GPU_PHYS                0x05000000ul
#define POLARIS_GPU_SIZE                0x00040000ul

/*
 * SDM845 GCC GPU clock branches share the GPU register window's prerequisites:
 * gcc-sdm845.c puts cfg_ahb at base+0x71004 and memnoc_gfx at base+0x7100c.
 */
#define POLARIS_GCC_GPU_CLOCKS_PHYS      0x00171000ul
#define POLARIS_GCC_GPU_CLOCKS_SIZE      0x00001000ul
#define GCC_GPU_CFG_AHB_CBCR             0x0004u
#define GCC_GPU_MEMNOC_GFX_CBCR          0x000cu

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

struct gpu_va_range {
	uint64_t start;
	uint64_t size;
};

static LCK_GRP_DECLARE(polaris_gpu_lck_grp, "polaris-gpu-va");
static LCK_MTX_DECLARE(polaris_gpu_va_lock, &polaris_gpu_lck_grp);
static struct gpu_va_range polaris_gpu_va_free[POLARIS_GPU_VA_MAX_RANGES];
static struct gpu_va_range polaris_gpu_va_allocated[POLARIS_GPU_VA_MAX_ALLOCS];
static uint32_t polaris_gpu_va_free_count;
static uint32_t polaris_gpu_va_allocated_count;
static bool polaris_gpu_va_initialized;

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

static bool
sdm845_gpu_ready(void)
{
	return false;				/* no writable A630 backend is registered yet */
}

const struct hakios_gpu_backend_ops hakios_gpu_sdm845_backend_ops = {
	.ready = sdm845_gpu_ready,
	.parameter = hakios_gpu_profile_parameter,
	.address = sdm845_gpu_address,
	.address_free = sdm845_gpu_address_free,
	/* The read-only probe exposes no maps, submissions, fences, or Redwood SoC maintenance. */
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

static uint32_t __attribute__((noinline))
probe_read(struct probe_block *block, uint32_t offset)
{
	uint32_t value;

	/* Keep the load form simple; Qualcomm's hypervisor emulates some of these device reads. */
	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (block->virtual + offset) : "memory");
	return value;
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
	static const uint32_t kgsl_stream_ids[] = { 0, 1, 2, 4, 5 };
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

		for (uint32_t i = 0; i < sizeof(kgsl_stream_ids) / sizeof(kgsl_stream_ids[0]); i++) {
			uint32_t sid = kgsl_stream_ids[i];
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

void
hakios_adreno_sdm845_init(void)
{
	uint32_t last_step = 0, gpu_enabled = 0;
	bool requested_probe;
	static bool (*const probes[])(void) = { probe_gpucc, probe_smmu, probe_gmu, probe_core };

	if (pe_arm_get_soc_base_phys() != SDM845_SOC_BASE) {
		return;
	}
	polaris_gpu_va_init();
	requested_probe = PE_parse_boot_argn("adreno", &last_step, sizeof(last_step)) && last_step != 0;
	PE_parse_boot_argn("gpu", &gpu_enabled, sizeof(gpu_enabled));
	if (!requested_probe) {
		if (gpu_enabled) {
			printf("adreno-sdm845: GPU rendering is not enabled: the A630 backend is at read-only bring-up stage\n");
		}
		return;
	}
	if (last_step > sizeof(probes) / sizeof(probes[0])) {
		printf("adreno-sdm845: only read-only steps 1..%zu are available; requested step %u is capped\n",
		    sizeof(probes) / sizeof(probes[0]), last_step);
		last_step = sizeof(probes) / sizeof(probes[0]);
	}
	printf("adreno-sdm845: Polaris Adreno 630 diagnostic, steps 1..%u, all reads and no writes\n", last_step);
	for (uint32_t i = 0; i < last_step; i++) {
		if (!probes[i]()) {
			printf("adreno-sdm845: step %u failed; later steps are not attempted\n", i + 1);
			return;
		}
	}
	printf("adreno-sdm845: read-only diagnostic finished; no GPU acceleration was started\n");
}
