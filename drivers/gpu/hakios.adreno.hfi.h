/*
 * A630 legacy GMU HFI queue protocol.
 *
 * The HFI v1 layout and queue identifiers follow Qualcomm KGSL's A630 path.
 * The caller must serialize command producers and provide a GMU-kernel-SMMU-mapped, noncached
 * shared-memory region. These helpers manage its wire format only; they do not attach an
 * SMMU context, perform cache maintenance, ring a GMU interrupt, or poll for
 * firmware replies.
 */
#ifndef _HAKIOS_ADRENO_HFI_H_
#define _HAKIOS_ADRENO_HFI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POLARIS_A630_HFI_QUEUE_COUNT          4u
#define POLARIS_A630_HFI_QUEUE_BYTES          0x1000u
#define POLARIS_A630_HFI_QUEUE_DWORDS         (POLARIS_A630_HFI_QUEUE_BYTES / sizeof(uint32_t))
#define POLARIS_A630_HFI_GMU_KERNEL_BASE      0x60000000u
#define POLARIS_A630_HFI_GMU_KERNEL_END       0x80000000u
#define POLARIS_A630_HFI_GMU_PAGE_BYTES       0x1000u

#define POLARIS_A630_HFI_QUEUE_COMMAND        0u
#define POLARIS_A630_HFI_QUEUE_MESSAGE        1u
#define POLARIS_A630_HFI_QUEUE_DEBUG          2u
#define POLARIS_A630_HFI_QUEUE_DISPATCH       3u

#define POLARIS_A630_HFI_ID_COMMAND           0u
#define POLARIS_A630_HFI_ID_DISPATCH          1u
#define POLARIS_A630_HFI_ID_MESSAGE           4u
#define POLARIS_A630_HFI_ID_DEBUG             5u

#define POLARIS_A630_HFI_TYPE_COMMAND         0u
#define POLARIS_A630_HFI_TYPE_POST            1u
#define POLARIS_A630_HFI_TYPE_ACK             2u
#define POLARIS_A630_HFI_ID_INIT              0u

#define POLARIS_A630_HFI_INTR_MESSAGE         (1u << 0)
#define POLARIS_A630_HFI_INTR_SEND            (1u << 0)

#define POLARIS_A630_HFI_TABLE_BYTES          216u
#define POLARIS_A630_HFI_QUEUE_DATA_OFFSET    224u
#define POLARIS_A630_HFI_REQUIRED_BYTES       \
	(POLARIS_A630_HFI_QUEUE_DATA_OFFSET + POLARIS_A630_HFI_QUEUE_COUNT * POLARIS_A630_HFI_QUEUE_BYTES)
#define POLARIS_A630_HFI_ALLOCATION_BYTES     \
	((POLARIS_A630_HFI_REQUIRED_BYTES + POLARIS_A630_HFI_GMU_PAGE_BYTES - 1u) & \
	 ~(POLARIS_A630_HFI_GMU_PAGE_BYTES - 1u))

struct polaris_a630_hfi_queue_header {
	uint32_t status;
	uint32_t start_address;
	uint32_t type;
	uint32_t queue_size_dwords;
	uint32_t message_size_dwords;
	uint32_t reserved[5];
	uint32_t read_index;
	uint32_t write_index;
};

struct polaris_a630_hfi_table_header {
	uint32_t version;
	uint32_t size_dwords;
	uint32_t first_queue_offset_dwords;
	uint32_t queue_header_size_dwords;
	uint32_t queue_count;
	uint32_t active_queue_count;
};

struct polaris_a630_hfi_table {
	struct polaris_a630_hfi_table_header header;
	struct polaris_a630_hfi_queue_header queue[POLARIS_A630_HFI_QUEUE_COUNT];
};

struct polaris_a630_hfi_state {
	volatile struct polaris_a630_hfi_table *table;
	volatile uint32_t *queue_data[POLARIS_A630_HFI_QUEUE_COUNT];
	uint32_t gmu_address;
	uint16_t sequence;
	bool initialized;
};

enum polaris_a630_hfi_read_result {
	POLARIS_A630_HFI_READ_INVALID = -1,
	POLARIS_A630_HFI_READ_TOO_SMALL = -2,
	POLARIS_A630_HFI_READ_EMPTY = 0,
	POLARIS_A630_HFI_READ_MESSAGE = 1,
};

static inline void
polaris_a630_hfi_barrier(void)
{
	/* The shared queue is mapped noncached; order payload/index before doorbell. */
	__builtin_arm_dmb(0xb); /* inner-shareable */
}

static inline size_t
polaris_a630_hfi_queue_offset(uint32_t queue_index)
{
	return POLARIS_A630_HFI_QUEUE_DATA_OFFSET + (size_t)queue_index * POLARIS_A630_HFI_QUEUE_BYTES;
}

static inline uint32_t
polaris_a630_hfi_legacy_id(uint32_t queue_index)
{
	static const uint8_t ids[POLARIS_A630_HFI_QUEUE_COUNT] = {
		POLARIS_A630_HFI_ID_COMMAND,
		POLARIS_A630_HFI_ID_MESSAGE,
		POLARIS_A630_HFI_ID_DEBUG,
		POLARIS_A630_HFI_ID_DISPATCH,
	};

	return queue_index < POLARIS_A630_HFI_QUEUE_COUNT ? ids[queue_index] : UINT32_MAX;
}

static inline bool
polaris_a630_hfi_layout_valid(void)
{
	return sizeof(struct polaris_a630_hfi_table_header) == 24u &&
	    sizeof(struct polaris_a630_hfi_queue_header) == 48u &&
	    sizeof(struct polaris_a630_hfi_table) == POLARIS_A630_HFI_TABLE_BYTES &&
	    POLARIS_A630_HFI_QUEUE_DATA_OFFSET == ((sizeof(struct polaris_a630_hfi_table) + 15u) & ~(size_t)15u) &&
	    POLARIS_A630_HFI_REQUIRED_BYTES <= POLARIS_A630_HFI_ALLOCATION_BYTES &&
	    polaris_a630_hfi_legacy_id(POLARIS_A630_HFI_QUEUE_COMMAND) == 0u &&
	    polaris_a630_hfi_legacy_id(POLARIS_A630_HFI_QUEUE_MESSAGE) == 4u &&
	    polaris_a630_hfi_legacy_id(POLARIS_A630_HFI_QUEUE_DEBUG) == 5u &&
	    polaris_a630_hfi_legacy_id(POLARIS_A630_HFI_QUEUE_DISPATCH) == 1u;
}

/* Initialize the host view before it is installed in the GMU kernel context. */
static inline bool
polaris_a630_hfi_init(struct polaris_a630_hfi_state *state, volatile void *memory, size_t memory_bytes,
	uint32_t gmu_address)
{
	static const uint8_t priorities[POLARIS_A630_HFI_QUEUE_COUNT] = { 10, 10, 40, 20 };
	static const uint8_t enabled[POLARIS_A630_HFI_QUEUE_COUNT] = { 1, 1, 1, 0 };
	volatile uint32_t *words = (volatile uint32_t *)memory;
	volatile uint8_t *bytes = (volatile uint8_t *)memory;
	volatile struct polaris_a630_hfi_table *table;
	uint64_t gmu_end;

	if (state == NULL) {
		return false;
	}
	state->initialized = false;
	state->table = NULL;
	for (uint32_t i = 0; i < POLARIS_A630_HFI_QUEUE_COUNT; i++) {
		state->queue_data[i] = NULL;
	}
	if (memory == NULL || !polaris_a630_hfi_layout_valid() ||
	    ((uintptr_t)memory & 15u) != 0 || memory_bytes < POLARIS_A630_HFI_ALLOCATION_BYTES ||
	    (memory_bytes & (sizeof(uint32_t) - 1u)) != 0 ||
	    (gmu_address & (POLARIS_A630_HFI_GMU_PAGE_BYTES - 1u)) != 0 ||
	    gmu_address < POLARIS_A630_HFI_GMU_KERNEL_BASE) {
		return false;
	}
	gmu_end = (uint64_t)gmu_address + POLARIS_A630_HFI_ALLOCATION_BYTES;
	if (gmu_end > POLARIS_A630_HFI_GMU_KERNEL_END || gmu_end > UINT32_MAX + 1ull) {
		return false;
	}

	for (size_t i = 0; i < POLARIS_A630_HFI_ALLOCATION_BYTES / sizeof(uint32_t); i++) {
		words[i] = 0;
	}
	table = (volatile struct polaris_a630_hfi_table *)memory;
	table->header.version = 0;
	table->header.size_dwords = sizeof(*table) / sizeof(uint32_t);
	table->header.first_queue_offset_dwords = sizeof(table->header) / sizeof(uint32_t);
	table->header.queue_header_size_dwords = sizeof(table->queue[0]) / sizeof(uint32_t);
	table->header.queue_count = POLARIS_A630_HFI_QUEUE_COUNT;
	/* A630 KGSL uses four active slots even when the dispatch slot status is disabled. */
	table->header.active_queue_count = POLARIS_A630_HFI_QUEUE_COUNT;
	for (uint32_t i = 0; i < POLARIS_A630_HFI_QUEUE_COUNT; i++) {
		volatile struct polaris_a630_hfi_queue_header *queue = &table->queue[i];
		uint64_t queue_address = (uint64_t)gmu_address + polaris_a630_hfi_queue_offset(i);

		if (queue_address > UINT32_MAX) {
			return false;
		}
		queue->status = enabled[i];
		queue->start_address = (uint32_t)queue_address;
		queue->type = polaris_a630_hfi_legacy_id(i) | ((uint32_t)priorities[i] << 8);
		queue->queue_size_dwords = POLARIS_A630_HFI_QUEUE_DWORDS;
		state->queue_data[i] = (volatile uint32_t *)(bytes + polaris_a630_hfi_queue_offset(i));
	}
	state->table = table;
	state->gmu_address = gmu_address;
	state->sequence = 0;
	polaris_a630_hfi_barrier();
	state->initialized = true;
	return true;
}

/* Header size is encoded in dwords; A630 HFI v1 uses command type zero. */
static inline bool
polaris_a630_hfi_make_header(uint8_t id, uint32_t size_dwords, uint8_t type, uint16_t sequence,
	uint32_t *header)
{
	if (header == NULL || size_dwords == 0 || size_dwords > UINT8_MAX || type > 0xfu || sequence > 0xfffu) {
		return false;
	}
	*header = ((uint32_t)sequence << 20) | ((uint32_t)type << 16) | (size_dwords << 8) | id;
	return true;
}

static inline uint16_t
polaris_a630_hfi_next_sequence(struct polaris_a630_hfi_state *state)
{
	uint16_t next;

	if (state == NULL || !state->initialized) {
		return 0;
	}
	next = (uint16_t)((state->sequence + 1u) & 0xfffu);
	if (next == 0) {
		next = 1;
	}
	state->sequence = next;
	return next;
}

/* Publishes one complete message. The caller serializes producers and rings HOST2GMU_INTR_SET[0]. */
static inline bool
polaris_a630_hfi_write(struct polaris_a630_hfi_state *state, uint32_t queue_index,
	const uint32_t *message, uint32_t message_dwords)
{
	volatile struct polaris_a630_hfi_queue_header *header;
	volatile uint32_t *queue;
	uint32_t read_index, write_index, used, free_words, next_write;

	if (state == NULL || !state->initialized || message == NULL || queue_index >= POLARIS_A630_HFI_QUEUE_COUNT ||
	    message_dwords == 0 || message_dwords > UINT8_MAX ||
	    ((message[0] >> 8) & 0xffu) != message_dwords) {
		return false;
	}
	header = &state->table->queue[queue_index];
	queue = state->queue_data[queue_index];
	if (header->status == 0 || header->queue_size_dwords != POLARIS_A630_HFI_QUEUE_DWORDS) {
		return false;
	}
	polaris_a630_hfi_barrier();
	read_index = header->read_index;
	write_index = header->write_index;
	if (read_index >= POLARIS_A630_HFI_QUEUE_DWORDS || write_index >= POLARIS_A630_HFI_QUEUE_DWORDS) {
		return false;
	}
	used = write_index >= read_index ? write_index - read_index :
	    POLARIS_A630_HFI_QUEUE_DWORDS - read_index + write_index;
	free_words = POLARIS_A630_HFI_QUEUE_DWORDS - used - 1u;
	if (message_dwords > free_words) {
		return false;
	}
	next_write = write_index;
	for (uint32_t i = 0; i < message_dwords; i++) {
		queue[next_write] = message[i];
		next_write = (next_write + 1u) % POLARIS_A630_HFI_QUEUE_DWORDS;
	}
	polaris_a630_hfi_barrier();
	header->write_index = next_write;
	polaris_a630_hfi_barrier();
	return true;
}

/* HFI v1 queues carry variable-size messages and do not use v2 dword padding. */
static inline enum polaris_a630_hfi_read_result
polaris_a630_hfi_read(struct polaris_a630_hfi_state *state, uint32_t queue_index,
	uint32_t *message, uint32_t message_capacity_dwords, uint32_t *message_dwords)
{
	volatile struct polaris_a630_hfi_queue_header *header;
	volatile uint32_t *queue;
	uint32_t read_index, write_index, used, size_dwords, next_read;

	if (message_dwords != NULL) {
		*message_dwords = 0;
	}
	if (state == NULL || !state->initialized || message == NULL || message_dwords == NULL ||
	    message_capacity_dwords == 0 || queue_index >= POLARIS_A630_HFI_QUEUE_COUNT) {
		return POLARIS_A630_HFI_READ_INVALID;
	}
	header = &state->table->queue[queue_index];
	queue = state->queue_data[queue_index];
	if (header->status == 0 || header->queue_size_dwords != POLARIS_A630_HFI_QUEUE_DWORDS) {
		return POLARIS_A630_HFI_READ_INVALID;
	}
	polaris_a630_hfi_barrier();
	read_index = header->read_index;
	write_index = header->write_index;
	if (read_index >= POLARIS_A630_HFI_QUEUE_DWORDS || write_index >= POLARIS_A630_HFI_QUEUE_DWORDS) {
		return POLARIS_A630_HFI_READ_INVALID;
	}
	if (read_index == write_index) {
		return POLARIS_A630_HFI_READ_EMPTY;
	}
	used = write_index >= read_index ? write_index - read_index :
	    POLARIS_A630_HFI_QUEUE_DWORDS - read_index + write_index;
	size_dwords = (queue[read_index] >> 8) & 0xffu;
	if (size_dwords == 0 || size_dwords > used) {
		return POLARIS_A630_HFI_READ_INVALID;
	}
	if (size_dwords > message_capacity_dwords) {
		return POLARIS_A630_HFI_READ_TOO_SMALL;
	}
	next_read = read_index;
	for (uint32_t i = 0; i < size_dwords; i++) {
		message[i] = queue[next_read];
		next_read = (next_read + 1u) % POLARIS_A630_HFI_QUEUE_DWORDS;
	}
	polaris_a630_hfi_barrier();
	header->read_index = next_read;
	*message_dwords = size_dwords;
	return POLARIS_A630_HFI_READ_MESSAGE;
}

/* A legacy ACK carries the original request header in word 1 and status in word 2. */
static inline bool
polaris_a630_hfi_v1_ack_matches(const uint32_t *message, uint32_t message_dwords,
	uint32_t sent_header, uint32_t *status)
{
	uint32_t ack_size, request_header;

	if (status != NULL) {
		*status = UINT32_MAX;
	}
	if (message == NULL || message_dwords < 3u) {
		return false;
	}
	ack_size = (message[0] >> 8) & 0xffu;
	request_header = message[1];
	if (((message[0] >> 16) & 0xfu) != POLARIS_A630_HFI_TYPE_ACK || ack_size < 3u ||
	    ack_size > message_dwords || (request_header & 0xffu) != (sent_header & 0xffu) ||
	    (request_header >> 20) != (sent_header >> 20)) {
		return false;
	}
	if (status != NULL) {
		*status = message[2];
	}
	return true;
}

static inline bool
polaris_a630_hfi_make_init(uint32_t dump_address, uint32_t dump_bytes, uint32_t boot_state,
	uint16_t sequence, uint32_t message[5])
{
	if (message == NULL || !polaris_a630_hfi_make_header(POLARIS_A630_HFI_ID_INIT, 5,
	    POLARIS_A630_HFI_TYPE_COMMAND, sequence, &message[0])) {
		return false;
	}
	message[1] = 0; /* A630 uses GMU segment zero. */
	message[2] = dump_address;
	message[3] = dump_bytes;
	message[4] = boot_state;
	return true;
}

#endif /* _HAKIOS_ADRENO_HFI_H_ */
