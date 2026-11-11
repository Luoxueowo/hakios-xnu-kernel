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
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

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

static bool
probe_gpucc(void)
{
	uint32_t cx, gx, gmu_clock, cxo_clock, rcgr, rcgr_cfg, pll1;

	if (!map_block(&gpucc)) {
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

static bool
probe_smmu(void)
{
	uint32_t idr0, idr1, idr2, idr7;

	if (!map_block(&smmu)) {
		return false;
	}
	printf("adreno-sdm845: step 2: read SMMUv2 identity at %#llx (no writes)\n", (uint64_t)smmu.physical);
	idr0 = probe_read(&smmu, SMMU_IDR0);
	idr1 = probe_read(&smmu, SMMU_IDR1);
	idr2 = probe_read(&smmu, SMMU_IDR2);
	idr7 = probe_read(&smmu, SMMU_IDR7);
	printf("adreno-sdm845: SMMU IDR0=%#x IDR1=%#x IDR2=%#x IDR7=%#x\n", idr0, idr1, idr2, idr7);
	return true;
}

static bool
probe_gmu(void)
{
	uint32_t cx, clock;
	uint32_t high1 = 0, high2 = 0, low = 0, init_result, hfi_version;
	bool counter_stable = false;

	if (!map_block(&gpucc)) {
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

	if (!map_block(&gpucc)) {
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
