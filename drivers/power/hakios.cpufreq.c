/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X5 Pro (redwood).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. There is no reason for us ever to want less than the
 * top row, for every group that has a core of ours in it (one core, unless boot-arg cpus=N
 * says more).
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern vm_offset_t pe_arm_get_soc_base_phys(void);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               0x18591000UL    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
	}
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		hakios_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			/* (the data starts after the header, so the header counts: this keeps the read inside the mapped database) */
			if (entry[i].length >= sizeof(aux) &&
			    sizeof(*db) + slave->data_offset + entry[i].offset + sizeof(aux) <= CMD_DB_SIZE) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                0x18220000UL
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the most (16383).
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as drivers/input/hakios.goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = 0x3fff;

	PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s\n", vote, ddr_mc0, ddr_sh0, ddr_voted ? "" : ": FAILED");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hakios_ddr_quiesce(void);

void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

/*
 * The OSM: what the SDM845 has in place of EPSS, and what the MIX 2S's cores are clocked by. Two domains of a
 * table of rows -- the little cores at 0x17d43000, the big at 0x17d45800 -- and one register each to ask for a
 * row (PERF_STATE, +0x920); the hardware switches clock and supply itself, so one write is all it takes.
 * Linux: drivers/cpufreq/qcom-cpufreq-hw.c ("osm" variant), sdm845.dtsi (cpufreq@17d43000, domain 0 the little
 * cores, domain 1 at 17d45800 the big). The register layout is the Mi 8's (dipper-xnu,
 * patches/xnu-mi8.patch, EXP-0127), which is the same SoC family and was measured on the phone.
 */
#define OSM_LITTLE_PHYS         0x17d43000UL
#define OSM_BIG_PHYS            0x17d45800UL
#define OSM_DOMAIN_SIZE         0x1400UL
#define OSM_ENABLE              0x000
#define OSM_LUT                 0x110           /* a row every 32 bytes: [31:30] clock source, [7:0] multiples of 19.2 MHz */
#define OSM_LUT_STRIDE          32
#define OSM_TABLE_ROWS          40
#define OSM_PERF_STATE          0x920

struct osm_domain {
	const char *	name;
	uint64_t	phys;
	vm_offset_t	base;
	uint32_t	rows;
	uint32_t	khz[OSM_TABLE_ROWS];
	uint32_t	row;			/* the row PERF_STATE holds */
	uint32_t	top;			/* the row it was in when it was opened: the governor never goes above it */
	bool		open;
};

static struct osm_domain osm[2] = {
	{ .name = "little", .phys = OSM_LITTLE_PHYS },
	{ .name = "big", .phys = OSM_BIG_PHYS },
};

static void osm_note(struct osm_domain * d);	/* (the read-only sysctl views of a domain, see below) */

static uint32_t
osm_rows(vm_offset_t base, uint32_t khz[OSM_TABLE_ROWS])
{
	uint32_t rows = 0;

	for (uint32_t row = 0; row < OSM_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + OSM_LUT + OSM_LUT_STRIDE * row);
		uint32_t value;

		if (entry == 0) {
			break;
		}
		value = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;
		/* (the table ends by repeating its last row: the same frequency is not another step) */
		if (rows > 0 && value <= khz[rows - 1]) {
			break;
		}
		khz[rows++] = value;
	}
	return rows;
}

/* What the table holds and what row the hardware is in. PERF_STATE keeps its value across a kernel run. */
static bool
osm_open(struct osm_domain * d, uint32_t cores)
{
	uint32_t state;

	d->base = ml_io_map((vm_offset_t) d->phys, OSM_DOMAIN_SIZE);
	if (d->base == 0) {
		tell("cpufreq: the OSM's %s domain at %#llx cannot be mapped; left alone\n", d->name, d->phys);
		return false;
	}
	d->rows = osm_rows(d->base, d->khz);
	state = *(volatile uint32_t *)(d->base + OSM_PERF_STATE);
	if (d->rows == 0) {
		tell("cpufreq: the OSM's %s domain at %#llx has %#x in ENABLE and no rows of frequencies; left alone\n",
		    d->name, d->phys, *(volatile uint32_t *)(d->base + OSM_ENABLE));
		return false;
	}
	d->row = state < d->rows ? state : 0;
	d->top = d->row;
	d->open = true;
	tell("cpufreq: the OSM's %s domain (%u of our cores): %u rows, %u to %u MHz; in row %u, %u MHz\n", d->name,
	    cores, d->rows, d->khz[0] / 1000, d->khz[d->rows - 1] / 1000, state,
	    state < d->rows ? d->khz[state] / 1000 : 0);
	osm_note(d);
	return true;
}

/* The governor's one write: the row asked for, and what the hardware made of it two milliseconds later. */
static void
osm_set(struct osm_domain * d, uint32_t row)
{
	uint32_t now;

	if (!d->open) {
		return;
	}
	if (row >= d->rows) {
		row = d->rows - 1;
	}
	*(volatile uint32_t *)(d->base + OSM_PERF_STATE) = row;
	__asm__ volatile ("dsb sy" ::: "memory");
	d->row = row;
	delay_for_interval(2, NSEC_PER_MSEC);
	now = *(volatile uint32_t *)(d->base + OSM_PERF_STATE);
	tell("cpufreq: the OSM's %s domain: row %u asked for (%u MHz); PERF_STATE now %u, %u MHz\n", d->name, row,
	    d->khz[row] / 1000, now, now < d->rows ? d->khz[now] / 1000 : 0);
	osm_note(d);
}

/*
 * One domain put at the row it is asked for: -1 leaves it where the boot loader left it (and that row is the
 * ceiling), -2 asks for the top row, anything else for that row. The row written becomes the ceiling the
 * governor may go back up to.
 */
static void
osm_apply(struct osm_domain * d, int want)
{
	if (!d->open) {
		return;
	}
	if (want == -1 || d->rows == 0) {
		d->top = d->row;
		return;
	}
	if (want == -2 || (uint32_t) want >= d->rows) {
		want = (int) d->rows - 1;
	}
	d->top = (uint32_t) want;
	osm_set(d, (uint32_t) want);
}

/*
 * The frequency the two domains are put at, from the boot-args:
 *   cpufreq_observe=1        leave both as they are (read only)
 *   cpufreq_row=N            both domains to row N
 *   cpufreq_big_row=N        the big cores only; cpufreq_little_row=N the little ones
 * and, without any of them, the top row of each: the boot loader leaves a modest one and there is no governor
 * to raise it later. With cpufreq_gov= (see below) the big domain starts at the top and the governor brings it
 * down as the phone warms, so the same default is right there too.
 */
static void
osm_cpufreq(uint32_t cores_little, uint32_t cores_big)
{
	int little = -2, big = -2;              /* (row -2: the top one, unless it is said otherwise below) */
	uint32_t row = 0, observe = 0;

	if (PE_parse_boot_argn("cpufreq_observe", &observe, sizeof(observe)) && observe != 0) {
		little = big = -1;              /* (read only: what the boot loader left, for a run that wants to know) */
	}
	if (PE_parse_boot_argn("cpufreq_row", &row, sizeof(row))) {
		little = big = (int) row;
	}
	if (PE_parse_boot_argn("cpufreq_big_row", &row, sizeof(row))) {
		big = (int) row;
	}
	if (PE_parse_boot_argn("cpufreq_little_row", &row, sizeof(row))) {
		little = (int) row;
	}
	if (osm_open(&osm[0], cores_little)) {
		osm_apply(&osm[0], little);
	}
	if (osm_open(&osm[1], cores_big)) {
		osm_apply(&osm[1], big);
	}
}

/* ---------------------------------------------------------------------------------------------------------
 * The governor: the big cores' row from the SoC's own temperature, with hysteresis, as the Mi 8 does it
 * (dipper-xnu, patches/xnu-mi8.patch: a target in degrees, two strikes to go down, four degrees of headroom to
 * come back up, and the row it was in as the ceiling). The sensors are drivers/power/hakios.thermal.c's
 * (boot-arg thermal=1): this reads the hottest of them every cpufreq_gov_ms milliseconds and says what it does.
 * None of it writes a thermal block: the policy is ours, the hardware's thresholds are untouched.
 *
 *   cpufreq_gov=<C>      hold the hottest CPU sensor near C degrees (55 to 110; without it no governor)
 *   cpufreq_gov_ms=<ms>  how often it is looked at (500 to 60000; 2000)
 *
 * Only rows below the one the domain was opened in are asked for, and never below cpufreq_gov_floor MHz (1600,
 * as the Mi 8): the phone stays usable rather than cool. Ten readings in a row that make no sense (the thermal
 * driver not started yet, sensors gone) put the big domain back at the top of the range.
 */
/*
 * The CPU cores' own temperatures, read here rather than through drivers/power/hakios.thermal.c: this policy is
 * the CPU governor's and it wants the cores, not the hottest thing on the SoC (a charging PMIC or the GPU would
 * otherwise hold the cores down). SDM845's TSENS blocks are the same two the thermal driver reads: the readings
 * at 0xc263000 and 0xc265000, one status word a sensor (0xa0 + 4*n), bits [11:0] tenths of a degree, signed, bit
 * 21 set when there is a reading. The numbers are the MIX 2S's own device tree's (android-polaris.dtb,
 * /soc/tsens@c222000: cpu0-silver 1 ... cpu3-silver 4, cpu0-gold 7 ... cpu3-gold 10).
 */
#define TSENS_TM0_PHYS		0xc263000UL
#define TSENS_TM_SIZE		0x200
#define TSENS_STATUS(sensor)	(0xa0 + 4 * (sensor))
#define TSENS_VALID		(1u << 21)

static vm_offset_t	tsens_tm;
static const uint32_t	cpu_sensors[] = { 1, 2, 3, 4, 7, 8, 9, 10 };	/* (the little cores, then the big) */

/* The hottest of our cores, in millidegrees; -273000 when no sensor has a reading. */
static int
cpu_temperature_mc(void)
{
	int32_t hottest = INT32_MIN;

	if (tsens_tm == 0) {
		return -273000;
	}
	for (unsigned i = 0; i < sizeof(cpu_sensors) / sizeof(cpu_sensors[0]); i++) {
		uint32_t status = *(volatile uint32_t *)(tsens_tm + TSENS_STATUS(cpu_sensors[i]));
		int32_t tenth = (int32_t) (status << 20) >> 20;

		if (!(status & TSENS_VALID) || tenth < -400 || tenth > 1600) {
			continue;
		}
		if (tenth > hottest) {
			hottest = tenth;
		}
	}
	return hottest == INT32_MIN ? -273000 : (int) hottest * 100;
}

static bool		govOn;
static uint32_t		govTarget = 85, govLittle = 93;	/* (the temperature ceilings, degrees: the top of what a row may be) */
static uint32_t		govMs = 100;			/* how often the load is looked at (cpufreq_gov_ms) */
static uint32_t		govFloor = 1600000;		/* the big cores never go below this for load alone */
static uint64_t		govIdleAt, govIdleMs = 10000;	/* (a quiet spell this long lets even them go to the bottom) */
static uint32_t		govCap, govLow;			/* the temperature ceiling in rows (big), and the lowest row it goes to */
static uint32_t		govCapLittle;			/* the same for the little cores */
static unsigned		govHot, govCool, govInvalid;	/* (the big cores' temperature strikes, and unreadable samples) */
static unsigned		govLittleHot, govLittleCool;
static unsigned		govIdleBig, govIdleLittle;	/* (samples the load has been below the row it is at) */
static unsigned		govUtilBig, govUtilLittle;	/* (percent, for the log) */
static unsigned		govCoreUtil[8];	/* (percent each core was busy, by core number: for the log) */
static unsigned		govPrevBig, govPrevLittle;	/* (the sample before: a jump is what boost is for) */
static uint32_t		govBoost = 1, govBoostStep = 25;
static uint64_t		govSamples, govDown, govUp;
static uint64_t		govLastAt, govLastIdle[MAX_CPUS];
static thread_call_t	govCall;
static int		cpu_gov_row_value = -1;	/* (the row it allows, for kern.hakios.cpu_gov_row) */

static void govTick(thread_call_param_t p0, thread_call_param_t p1);

/* The lowest row whose frequency is at least the floor: what the governor will go down to. */
static uint32_t
osm_floor_row(struct osm_domain * d, uint32_t khz)
{
	for (uint32_t row = 0; row < d->rows; row++) {
		if (d->khz[row] >= khz) {
			return row;
		}
	}
	return d->rows - 1;
}

static void
govApplyBig(uint32_t row, const char *reason)
{
	if (!osm[1].open || row > osm[1].top || row == osm[1].row) {
		return;
	}
	cpu_gov_row_value = (int) row;
	tell("cpufreq: gov: big %u -> %u (%u MHz), load %u%%, reason %s\n", osm[1].row, row, osm[1].khz[row] / 1000,
	    govUtilBig, reason);
	osm_set(&osm[1], row);
}

static void
govApplyLittle(uint32_t row, const char *reason)
{
	if (!osm[0].open || row > osm[0].top || row == osm[0].row) {
		return;
	}
	tell("cpufreq: gov: little %u -> %u (%u MHz), load %u%%, reason %s\n", osm[0].row, row,
	    osm[0].khz[row] / 1000, govUtilLittle, reason);
	osm_set(&osm[0], row);
}

/*
 * The load of the two clusters, in percent, from the time the cores were not idle since the sample before (the
 * same idle accounting schedutil uses, ml_cpu_idle_time(): the cpus are numbered as the topology has them, and the
 * low byte of phys_id is the core number of the SoC -- 0 to 3 the little cores on SDM845, 4 to 7 the big ones).
 */
static void
govUtil(unsigned * big, unsigned * little)
{
	uint64_t now = mach_absolute_time();
	uint64_t ns = now - govLastAt;                  /* (ml_cpu_idle_time() counts in the same units) */
	uint64_t busyBig = 0, busyLittle = 0;
	unsigned nBig = 0, nLittle = 0;
	const ml_topology_info_t * topology = ml_get_topology_info();

	govLastAt = now;
	if (ns == 0 || topology == NULL) {
		*big = *little = 0;
		return;
	}
	for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		uint64_t idle = 0, waited;

		if (!ml_cpu_idle_time((int) i, &idle)) {
			continue;
		}
		waited = idle - govLastIdle[i];
		govLastIdle[i] = idle;
		if (waited > ns) {
			waited = ns;
		}
		{
			unsigned core = (unsigned) ((topology->cpus[i].phys_id >> 8) & 0xff);

			if (core < sizeof(govCoreUtil) / sizeof(govCoreUtil[0])) {
				govCoreUtil[core] = (unsigned) (((ns - waited) * 100) / ns);
			}
		}
		if (((topology->cpus[i].phys_id >> 8) & 0xff) < 4) {
			busyLittle += ns - waited;
			nLittle++;
		} else {
			busyBig += ns - waited;
			nBig++;
		}
	}
	*big = nBig != 0 ? (unsigned) (busyBig * 100 / ((uint64_t) nBig * ns)) : 0;
	*little = nLittle != 0 ? (unsigned) (busyLittle * 100 / ((uint64_t) nLittle * ns)) : 0;
}

/*
 * The row a load asks for: the load, with a quarter of headroom (schedutil's 1.25), over the table's own range.
 * The top row is what the boot asked for; the floor of the big cores is applied by the caller.
 */
static uint32_t
govRowForUtil(struct osm_domain * d, unsigned util)
{
	uint64_t want;

	if (util > 100) {
		util = 100;
	}
	want = (uint64_t) util * 125 / 100;
	if (want > 100) {
		want = 100;
	}
	want = want * d->top / 100;
	if (want > d->top) {
		want = d->top;
	}
	return (uint32_t) want;
}

/*
 * The governor: the load decides the row, the temperature decides the highest row it may be. (The request: the
 * clocks follow what is running, as schedutil's do, and the temperature is the ceiling rather than the thing that
 * is chased -- 60 C as a target held the big cores at their lowest row while the phone was still at 70 C.)
 *
 *   cpufreq_gov=<C>          the big cores' temperature ceiling (85)
 *   cpufreq_gov_little=<C>   the little cores' (93: they are the last to be held back)
 *   cpufreq_gov_ms=<ms>      how often the load is looked at (100)
 *   cpufreq_gov_floor=<MHz>  the lowest the big cores are taken for load alone (1600)
 *
 * Up goes at once (a load that appears is served now); down takes GOV_DOWN_STRIKES samples of the load under the
 * row it is at, a row at a time, so a burst does not cost a frequency step. The temperature ceiling comes down
 * five samples after it is reached and goes back two samples after it has fallen four degrees, a row at a time.
 */
#define GOV_DOWN_STRIKES	5
#define GOV_UP_STRIKES		2

static void
govSample(void)
{
	unsigned big = 0, little = 0;
	uint32_t wantBig, wantLittle;
	int mc;

	govSamples++;
	govUtil(&big, &little);
	govUtilBig = big;
	govUtilLittle = little;

	/* The temperature: a ceiling, never the thing that is chased. */
	mc = cpu_temperature_mc();
	if (mc > -273000 + 1) {
		govInvalid = 0;
		if (mc >= (int) (govTarget * 1000)) {
			govCool = 0;
			if (govCap > govLow && ++govHot >= GOV_DOWN_STRIKES) {
				govHot = 0;
				govCap--;
			}
		} else if (mc <= (int) ((govTarget - 4) * 1000)) {
			govHot = 0;
			if (govCap < osm[1].top && ++govCool >= GOV_UP_STRIKES) {
				govCool = 0;
				govCap++;
			}
		} else {
			govHot = govCool = 0;
		}
		if (osm[0].open) {
			if (mc >= (int) (govLittle * 1000)) {
				govLittleCool = 0;
				if (govCapLittle > 0 && ++govLittleHot >= GOV_DOWN_STRIKES) {
					govLittleHot = 0;
					govCapLittle--;
				}
			} else if (mc <= (int) ((govLittle - 4) * 1000)) {
				govLittleHot = 0;
				if (govCapLittle < osm[0].top && ++govLittleCool >= GOV_UP_STRIKES) {
					govLittleCool = 0;
					govCapLittle++;
				}
			} else {
				govLittleHot = govLittleCool = 0;
			}
		}
	} else if (++govInvalid >= 10) {
		/* (no sensor reading for ten samples: no ceiling, the load alone) */
		govInvalid = 0;
		govCap = osm[1].open ? osm[1].top : 0;
		govCapLittle = osm[0].open ? osm[0].top : 0;
	}

	/*
	 * The big cores: the load's row, under the temperature's ceiling and over the floor -- except after a long
	 * quiet spell (cpufreq_gov_idle_ms, ten seconds), when even they are let down to the bottom of their table.
	 * The floor is back the sample the load returns.
	 */
	if (osm[1].open) {
		uint32_t floorRow = govLow;

		/*
		 * A jump in the load (a quarter of the cores at once, cpufreq_gov_boost_step): the top row now, without
		 * waiting for the row the load maps to or for the temperature ceiling to be consulted -- AOSP's boost is
		 * the same idea. cpufreq_gov_boost=0 turns it off.
		 */
		if (govBoost && big >= 25 && big >= govPrevBig + govBoostStep) {
			govPrevBig = big;
			govIdleBig = 0;
			govIdleAt = 0;
			govUp++;
			govApplyBig(osm[1].top, "boost");
			goto little_cores;
		}
		govPrevBig = big;
		if (big <= 10) {
			uint64_t now = mach_absolute_time(), quiet = 0;

			if (govIdleAt == 0) {
				govIdleAt = now;
			}
			absolutetime_to_nanoseconds(now - govIdleAt, &quiet);
			if (quiet >= govIdleMs * NSEC_PER_MSEC) {
				floorRow = 0;
			}
		} else {
			govIdleAt = 0;
		}
		wantBig = govRowForUtil(&osm[1], big);
		if (wantBig < floorRow) {
			wantBig = floorRow;
		}
		if (wantBig > govCap) {
			wantBig = govCap;
		}
		if (wantBig > osm[1].row) {
			govIdleBig = 0;
			govUp++;
			govApplyBig(wantBig, "load up");
		} else if (wantBig < osm[1].row) {
			if (++govIdleBig >= GOV_DOWN_STRIKES) {
				govIdleBig = 0;
				govDown++;
				govApplyBig(osm[1].row - 1, "load down");
			}
		} else {
			govIdleBig = 0;
		}
	}

little_cores:
	/* The little cores: the same, no floor of their own, and their own ceiling. */
	if (osm[0].open) {
		if (govBoost && little >= 25 && little >= govPrevLittle + govBoostStep) {
			govPrevLittle = little;
			govIdleLittle = 0;
			govApplyLittle(osm[0].top, "boost");
			goto gov_done;
		}
		govPrevLittle = little;
		wantLittle = govRowForUtil(&osm[0], little);
		if (wantLittle > govCapLittle) {
			wantLittle = govCapLittle;
		}
		if (wantLittle > osm[0].row) {
			govIdleLittle = 0;
			govApplyLittle(wantLittle, "load up");
		} else if (wantLittle < osm[0].row) {
			if (++govIdleLittle >= GOV_DOWN_STRIKES) {
				govIdleLittle = 0;
				govApplyLittle(osm[0].row - 1, "load down");
			}
		} else {
			govIdleLittle = 0;
		}
	}

gov_done:
	/* Every thirty samples, where it stands (about three seconds at the default interval). */
	if (govSamples % 30 == 0) {
		tell("cpufreq: gov: load big %u%%, little %u%%; rows %u/%u (%u/%u MHz), ceiling %u/%u, %d mC\n", big,
		    little, osm[1].row, osm[0].row, osm[1].open ? osm[1].khz[osm[1].row] / 1000 : 0,
		    osm[0].open ? osm[0].khz[osm[0].row] / 1000 : 0, govCap, govCapLittle, mc);
		tell("cpufreq: cores %u %u %u %u %u %u %u %u (busy %%, core 0 to 7)\n", govCoreUtil[0],
		    govCoreUtil[1], govCoreUtil[2], govCoreUtil[3], govCoreUtil[4], govCoreUtil[5], govCoreUtil[6],
		    govCoreUtil[7]);
	}
}

static void
govTick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	govSample();
	if (!govOn) {
		return;
	}
	clock_interval_to_deadline(govMs, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(govCall, deadline);
}

/* Boot-args cpufreq_gov=<C> and cpufreq_gov_ms=<ms>: the big domain held near that temperature. */
static void
govStart(void)
{
	uint32_t target = 0, ms = 0;
	uint64_t deadline;
	int mc;

	if (!PE_parse_boot_argn("cpufreq_gov", &target, sizeof(target)) || target == 0) {
		return;
	}
	if (target < 55 || target > 110) {
		tell("cpufreq: gov: %u degrees is outside 55 to 110; not started\n", target);
		return;
	}
	if (!osm[1].open || osm[1].rows < 3) {
		tell("cpufreq: gov: the big domain was not opened; not started\n");
		return;
	}
	tsens_tm = ml_io_map((vm_offset_t) TSENS_TM0_PHYS, TSENS_TM_SIZE);
	if (tsens_tm == 0) {
		tell("cpufreq: gov: the CPU sensors at %#lx cannot be mapped; not started\n", (unsigned long) TSENS_TM0_PHYS);
		return;
	}
	if (PE_parse_boot_argn("cpufreq_gov_ms", &ms, sizeof(ms)) && ms >= 20 && ms <= 2000) {
		govMs = ms;
	}
	if (PE_parse_boot_argn("cpufreq_gov_floor", &ms, sizeof(ms)) && ms >= 300 && ms <= 2800) {
		govFloor = ms * 1000;
	}
	if (PE_parse_boot_argn("cpufreq_gov_idle_ms", &ms, sizeof(ms)) && ms >= 1000 && ms <= 120000) {
		govIdleMs = ms;
	}
	if (PE_parse_boot_argn("cpufreq_gov_boost", &ms, sizeof(ms)) && ms <= 1) {
		govBoost = ms;
	}
	if (PE_parse_boot_argn("cpufreq_gov_boost_step", &ms, sizeof(ms)) && ms >= 5 && ms <= 100) {
		govBoostStep = ms;
	}
	govTarget = target;
	/*
	 * The little cores are the last to give: cpufreq_gov_little=<C>, eight degrees above the big ones' target
	 * without it (and never above 110). They go down to the bottom of their own table, a row at a time.
	 */
	govLittle = target + 8;
	if (PE_parse_boot_argn("cpufreq_gov_little", &ms, sizeof(ms)) && ms > target && ms <= 110) {
		govLittle = ms;
	}
	if (govLittle > 110) {
		govLittle = 110;
	}
	govLow = osm_floor_row(&osm[1], govFloor);
	if (govLow >= osm[1].top) {
		govLow = osm[1].top >= 2 ? osm[1].top - 2 : 0;   /* (no row at or above the floor: two below the top) */
	}
	govCap = osm[1].top;
	govCapLittle = osm[0].open ? osm[0].top : 0;
	cpu_gov_row_value = (int) govCap;
	/* (the load is a difference: one reading of each counter now is the baseline) */
	govLastAt = mach_absolute_time();
	{
		const ml_topology_info_t * topology = ml_get_topology_info();

		for (unsigned i = 0; topology != NULL && i < topology->num_cpus && i < MAX_CPUS; i++) {
			(void) ml_cpu_idle_time((int) i, &govLastIdle[i]);
		}
	}
	govCall = thread_call_allocate(govTick, NULL);
	if (govCall == NULL) {
		tell("cpufreq: gov: no thread call; not started\n");
		return;
	}
	govOn = true;
	mc = cpu_temperature_mc();
	clock_interval_to_deadline(govMs, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(govCall, deadline);
	tell("cpufreq: gov: the load picks the row (big %u to %u MHz, little %u to %u MHz); the temperature is the "
	    "ceiling (%u C big, %u C little), looked at every %u ms, down five samples of load and up at once; %s\n",
	    osm[1].khz[govLow] / 1000, osm[1].khz[osm[1].top] / 1000, osm[0].open ? osm[0].khz[0] / 1000 : 0,
	    osm[0].open ? osm[0].khz[osm[0].top] / 1000 : 0, govTarget, govLittle, govMs,
	    mc > -273000 + 1 ? "the CPU sensors are being read" : "no CPU sensor has a reading yet");
}

/*
 * What the two domains are at, for user space: kern.hakios.cpu_big_mhz, cpu_little_mhz, cpu_big_row,
 * cpu_little_row and cpu_gov_row (-1: no governor). The rows are the boot-arg/govenor's writing, so these are
 * read-only views; osm_set() and govApply() keep them up to date.
 */
static int	cpu_big_mhz_value, cpu_little_mhz_value;
static int	cpu_big_row_value, cpu_little_row_value;

static void
osm_note(struct osm_domain * d)
{
	int mhz = d->open && d->row < d->rows ? (int) (d->khz[d->row] / 1000) : 0;

	if (d == &osm[0]) {
		cpu_little_mhz_value = mhz;
		cpu_little_row_value = (int) d->row;
	} else {
		cpu_big_mhz_value = mhz;
		cpu_big_row_value = (int) d->row;
	}
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, cpu_big_mhz, CTLFLAG_RD | CTLFLAG_LOCKED, &cpu_big_mhz_value, 0,
    "the clock the big cores' domain is asked for");
SYSCTL_INT(_kern_hakios, OID_AUTO, cpu_little_mhz, CTLFLAG_RD | CTLFLAG_LOCKED, &cpu_little_mhz_value, 0,
    "the clock the little cores' domain is asked for");
SYSCTL_INT(_kern_hakios, OID_AUTO, cpu_big_row, CTLFLAG_RD | CTLFLAG_LOCKED, &cpu_big_row_value, 0,
    "the row the big cores' domain is in (the last one is the fastest)");
SYSCTL_INT(_kern_hakios, OID_AUTO, cpu_little_row, CTLFLAG_RD | CTLFLAG_LOCKED, &cpu_little_row_value, 0,
    "the row the little cores' domain is in");
SYSCTL_INT(_kern_hakios, OID_AUTO, cpu_gov_row, CTLFLAG_RD | CTLFLAG_LOCKED, &cpu_gov_row_value, 0,
    "the row the temperature governor allows the big cores (-1: no governor)");

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, membench = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	/*
	 * The EPSS block below belongs to the other SoC this tree is built for (SM7325, redwood). The MIX 2S's SDM845
	 * has nothing of the kind at that address -- its cores' clock is the OSM's, which this file does not drive --
	 * and 0x18591000 is some other block there. Refuse instead of writing rows into it.
	 */
	if (pe_arm_get_soc_base_phys() == 0x00100000UL) {
		/*
		 * The MIX 2S's SDM845 has nothing at the EPSS address: its cores are the OSM's, two domains of them
		 * (see osm_cpufreq()). How fast a loop runs before and after says what the row asked for is worth.
		 */
		uint32_t little = 0, big = 0;

		for (unsigned int i = 0; i < topology->num_cpus; i++) {
			if (((topology->cpus[i].phys_id >> 8) & 0xff) < 4) {
				little++;
			} else {
				big++;
			}
		}
		here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
		before = loop_speed();
		osm_cpufreq(little, big);
		printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
		govStart();
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	/* membench has its own variable: when the boot-arg is absent, the cpufreq value (2 means on) must not start the benchmark */
	PE_parse_boot_argn("membench", &membench, sizeof(membench));
	if (membench == 1) {
		memory_bench();
	} else if (membench == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
