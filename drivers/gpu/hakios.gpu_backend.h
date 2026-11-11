/*
 * Hardware boundary between the shared HakiosGPU client and an Adreno SoC backend.
 * Backends own address translation, command submission, completion and power-down.
 */
#ifndef _HAKIOS_GPU_BACKEND_H_
#define _HAKIOS_GPU_BACKEND_H_

#include <stdbool.h>
#include <stdint.h>

struct hakios_gpu_backend_ops {
	bool (*ready)(void);
	bool (*parameter)(uint32_t which, uint64_t *value);
	bool (*timestamp)(uint64_t *value); /* optional hardware counter for MSM_PARAM_TIMESTAMP */
	void (*quiesce)(void);
	void (*minute)(void);
	bool (*client_space_create)(void **space);
	void (*client_space_destroy)(void *space, bool safe_to_free);
	uint64_t (*address)(uint64_t size);
	void (*address_free)(uint64_t address, uint64_t size);
	bool (*map)(void *space, uint64_t address, uint64_t phys, bool read_only);
	void (*mapped)(void *space);
	void (*unmap)(void *space, uint64_t address, uint64_t size);
	void (*to_memory)(void *start, uint64_t size);
	uint32_t (*submit)(void *space, const uint64_t *addresses, const uint32_t *words, uint32_t count);
	bool (*done)(uint32_t fence);
	uint32_t (*last)(void);
	void (*worked)(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7]);
	void (*report)(const char *why);
	void (*stop)(void); /* orderly shutdown calls this even when ready() is false */
};

extern const struct hakios_gpu_backend_ops hakios_gpu_redwood_backend_ops;
extern const struct hakios_gpu_backend_ops hakios_gpu_sdm845_backend_ops;

/* Shared by the public HakiosGPU wrappers and each selected SoC backend. */
void hakios_gpu_backend_select(const struct hakios_gpu_backend_ops *ops);
bool hakios_gpu_profile_parameter(uint32_t which, uint64_t *value);

#endif /* _HAKIOS_GPU_BACKEND_H_ */
