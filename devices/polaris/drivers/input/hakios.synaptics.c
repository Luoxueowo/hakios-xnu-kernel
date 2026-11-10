/*
 * The touch screen of the Xiaomi MIX 2S (polaris, SDM845): a Synaptics S3330, incell, RMI4 over I2C.
 *
 * The checked-in Android device tree puts it at /soc/i2c@a98000/synaptics_dsi_i2c@20 (compatible
 * synaptics,dsx-i2c-force, address 0x20), attention on GPIO125 (active low), reset on GPIO99. The
 * legacy Polaris bring-up (old-project commit 2e2c898) reports that the Qualcomm GENI I2C controller
 * was not usable there and bit-bangs SDA GPIO33 / SCL GPIO34 on the TLMM north tile instead. Those
 * bus pins are not described by the checked-in tree; this preserves that previously used path, but
 * it still needs validation from the main repository on the phone.
 *
 * RMI4 as the boot loader and the vendor driver left it:
 *   F01 at PDT 0x00e3 (device status, interrupt status, control), F12 at PDT 0x00dd (2D touch).
 *   F12's query base is 0x49 (its Control base 0x12, its Data base 0x08). Query5 says how many
 *   slots there are (Control 23) and how long a report is (Control 28); this phone's controller
 *   answers 10 slots of 8 bytes, which is the "vendor DSX" object layout: status, x (LE), y (LE),
 *   z, wx, wy. The old bring-up proved that on hardware; this driver keeps its decoding and adds
 *   the two things it lacked.
 *
 * What is new here, compared with that bring-up:
 *   - the slots and the report size come from Control 23 and 28 instead of being guessed;
 *   - a frame that fails to read is retried, and a controller that keeps failing is re-probed
 *     instead of the driver stopping for good;
 *   - the coordinate scale is a boot argument (synaptics_max_x/y) rather than a constant;
 *   - the log goes to the loader's (hakios_tl), so a run's lines survive the kernel's own ring.
 *
 * F12 is read by polling the data base, as the bring-up did. An earlier revision waited for F12's
 * bit in F01's interrupt status instead; on this phone that bit never came (F01 Control 0, the
 * interrupt enable, reads back clear), so the driver logged "no interrupt for 400 frames" about a
 * second after boot and stopped -- every boot, with the data base perfectly readable throughout.
 *
 * Boot arguments (all optional):
 *   touchscreen=1            run at all (the same switch as the other touch drivers)
 *   synaptics_reset=0        leave RESET99 alone (by default, release it if the loader left it asserted)
 *   synaptics_fingers=N      ignore Control 23 and use N slots
 *   synaptics_poll_us=N      polling interval in microseconds (500..1000000; default 20000)
 *   synaptics_max_x=N        the panel's X coordinate range (default 1080)
 *   synaptics_max_y=N        the panel's Y coordinate range (default 2160)
 *   synaptics_trace=1        a line for every frame (bring-up)
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t thread);   /* not in the exported headers */

#define SYNAPTICS_TLMM_NORTH    0x03900000u     /* TLMM base 0x03400000 + north tile 0x00500000 */
#define SYNAPTICS_GPIO_WINDOW   0x23000u        /* enough for GPIO33/34 of the north tile */
#define SYNAPTICS_SDA           33u
#define SYNAPTICS_SCL           34u
#define SYNAPTICS_RESET         99u
#define SYNAPTICS_GPIO_STRIDE   0x1000u
#define SYNAPTICS_GPIO_IO       0x4u            /* the input/output register, after the config one */

/* TLMM configuration words the old bring-up proved on this phone: GPIO function, 2 mA drive, and
 * either "input, pull-up" (a released line) or "output, latched low" (a line being pulled down). */
#define SYNAPTICS_CFG_MASK      0x3ffu
#define SYNAPTICS_IN_PULL_UP    (3u | (2u << 6))
#define SYNAPTICS_OUT_LOW       ((2u << 6) | 0x200u)

#define SYNAPTICS_I2C_ADDR      0x20u

#define SYNAPTICS_PDT_F01       0x00e3u
#define SYNAPTICS_PDT_F12       0x00ddu
#define SYNAPTICS_MAX_READ      128u
#define SYNAPTICS_MAX_SLOTS     10u
#define SYNAPTICS_SLOT_BYTES    8u
#define SYNAPTICS_REPORT_BYTES  8u              /* Control 28 must say this */

/* A read that fails three times in a row is a broken transfer, not a busy controller. */
#define SYNAPTICS_BAD_FRAMES    3

static vm_offset_t syn_gpio;                    /* north tile, GPIO33/34 */
static vm_offset_t syn_reset_gpio;              /* GPIO99, its own page */
static uint32_t    syn_saved[4];                /* SDA cfg, SDA io, SCL cfg, SCL io */
static bool        syn_gpio_ready;

static uint32_t syn_reset_arg = 1;              /* the loader leaves RESET99 asserted: release it unless told not to */
static uint32_t syn_fingers_arg;
static uint32_t syn_poll_us = 20000;            /* the bring-up polled every 20 ms; the bus is bit-banged */
static uint32_t syn_max_x = 1080;
static uint32_t syn_max_y = 2160;
static uint32_t syn_trace;

/* Probe results. */
static uint8_t syn_f01_status;                  /* PDT byte 2: the status register's address */
static uint8_t syn_f01_irq;                     /* PDT byte 3: the interrupt status register's address */
static uint8_t syn_f01_ctrl;                    /* PDT byte 1: the control register's address */
static uint8_t syn_f12_data;                    /* PDT byte 3: where reports are read */
static unsigned syn_slots;

static struct {
	bool down;
	uint32_t x, y;
} syn_contact[SYNAPTICS_MAX_SLOTS];

static unsigned syn_bad;                        /* consecutive bad transfers */
static uint32_t syn_bad_reads;                  /* transfers that failed, for the report at the end */
static uint32_t syn_frames, syn_reports;

static uint32_t
syn_gpio_read(vm_offset_t base, uint32_t off)
{
	uint32_t value = *(volatile uint32_t *)(base + off);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return value;
}

static void
syn_gpio_write(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static uint32_t
syn_cfg_off(uint32_t pin)
{
	return pin * SYNAPTICS_GPIO_STRIDE;
}

/*
 * A line of the bus: pulled low by latching a zero before the pin becomes an output, released by
 * becoming an input with the pull-up on. Never driven high: the other side may be driving.
 */
static void
syn_line(uint32_t pin, bool release)
{
	uint32_t cfg = syn_cfg_off(pin);
	uint32_t old = syn_gpio_read(syn_gpio, cfg);

	if (!release) {
		syn_gpio_write(syn_gpio, cfg + SYNAPTICS_GPIO_IO,
		    syn_gpio_read(syn_gpio, cfg + SYNAPTICS_GPIO_IO) & ~2u);
	}
	syn_gpio_write(syn_gpio, cfg, (old & ~SYNAPTICS_CFG_MASK) |
	    (release ? SYNAPTICS_IN_PULL_UP : SYNAPTICS_OUT_LOW));
}

static bool
syn_high(uint32_t pin)
{
	return (syn_gpio_read(syn_gpio, syn_cfg_off(pin) + SYNAPTICS_GPIO_IO) & 1u) != 0;
}

/* Release SCL and wait for it to rise: at most 100 us of clock stretching, never assume it did. */
static bool
syn_clock(void)
{
	syn_line(SYNAPTICS_SCL, true);
	for (unsigned i = 0; i < 20; ++i) {
		if (syn_high(SYNAPTICS_SCL)) {
			delay(5);
			return true;
		}
	}
	return false;
}

static void
syn_stop(void)
{
	syn_line(SYNAPTICS_SDA, false);
	delay(5);
	(void)syn_clock();
	syn_line(SYNAPTICS_SDA, true);
	delay(5);
}

static bool
syn_idle(void)
{
	syn_line(SYNAPTICS_SDA, true);
	syn_line(SYNAPTICS_SCL, true);
	delay(20);
	return syn_high(SYNAPTICS_SDA) && syn_high(SYNAPTICS_SCL);
}

static bool
syn_start(bool repeated)
{
	syn_line(SYNAPTICS_SDA, true);
	if (!repeated) {
		if (!syn_idle()) {
			return false;           /* a stuck bus: do not report a made-up ACK */
		}
	} else if (!syn_clock()) {
		return false;
	}
	if (!syn_high(SYNAPTICS_SDA) || !syn_high(SYNAPTICS_SCL)) {
		return false;
	}
	syn_line(SYNAPTICS_SDA, false);
	delay(5);
	syn_line(SYNAPTICS_SCL, false);
	delay(5);
	return true;
}

/* 1: the byte could not be shifted out, 2: nobody acknowledged it. */
static unsigned syn_write_failure;

static bool
syn_write(uint8_t byte)
{
	syn_write_failure = 0;
	for (int i = 7; i >= 0; --i) {
		syn_line(SYNAPTICS_SDA, (byte & (1u << i)) != 0);
		delay(3);
		if (!syn_clock()) {
			syn_write_failure = 1;
			return false;
		}
		syn_line(SYNAPTICS_SCL, false);
		delay(3);
	}
	syn_line(SYNAPTICS_SDA, true);
	delay(5);
	if (!syn_clock()) {
		syn_write_failure = 1;
		return false;
	}
	bool ack = !syn_high(SYNAPTICS_SDA);
	syn_line(SYNAPTICS_SCL, false);
	delay(5);
	if (!ack) {
		syn_write_failure = 2;
	}
	return ack;
}

static bool
syn_read_byte(uint8_t *out, bool ack)
{
	uint8_t byte = 0;

	syn_line(SYNAPTICS_SDA, true);
	for (unsigned i = 0; i < 8; ++i) {
		if (!syn_clock()) {
			return false;
		}
		byte = (uint8_t)((byte << 1) | (syn_high(SYNAPTICS_SDA) ? 1u : 0u));
		syn_line(SYNAPTICS_SCL, false);
		delay(3);
	}
	syn_line(SYNAPTICS_SDA, !ack);
	delay(3);
	if (!syn_clock()) {
		return false;
	}
	syn_line(SYNAPTICS_SCL, false);
	syn_line(SYNAPTICS_SDA, true);
	*out = byte;
	return true;
}

/*
 * An RMI4 register read as the controller expects it: write 0xff (the page) and the register's high
 * byte, STOP; then address the one-byte register and read from it with a repeated START.
 */
static unsigned syn_read_stage;

static bool
syn_read(uint16_t reg, uint8_t *buf, unsigned len)
{
	bool ok = false;

	syn_read_stage = 1;
	if (len == 0 || len > SYNAPTICS_MAX_READ) {
		goto out;
	}
	if (!syn_start(false)) {
		goto out;
	}
	syn_read_stage = 2;
	if (!syn_write(SYNAPTICS_I2C_ADDR << 1)) {
		goto out;
	}
	syn_read_stage = 3;
	if (!syn_write(0xff)) {
		goto out;
	}
	syn_read_stage = 4;
	if (!syn_write((uint8_t)(reg >> 8))) {
		goto out;
	}
	syn_stop();
	syn_read_stage = 5;
	if (!syn_start(false)) {
		goto out;
	}
	syn_read_stage = 6;
	if (!syn_write(SYNAPTICS_I2C_ADDR << 1)) {
		goto out;
	}
	syn_read_stage = 7;
	if (!syn_write((uint8_t)reg)) {
		goto out;
	}
	syn_read_stage = 8;
	if (!syn_start(true)) {
		goto out;
	}
	syn_read_stage = 9;
	if (!syn_write((SYNAPTICS_I2C_ADDR << 1) | 1u)) {
		goto out;
	}
	for (unsigned i = 0; i < len; ++i) {
		syn_read_stage = 10 + i;
		if (!syn_read_byte(&buf[i], i + 1 < len)) {
			goto out;
		}
	}
	ok = true;
out:
	syn_stop();
	syn_read_stage = ok ? 30 : syn_read_stage;
	if (!syn_idle()) {
		syn_read_stage = 31;
		return false;
	}
	if (!ok) {
		syn_bad_reads++;
	}
	return ok;
}

/* Short waits in a worker thread of the boot: keep them local, as the bring-up did. */
static void
syn_wait(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
syn_wait_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
syn_release(void)
{
	for (unsigned i = 0; i < SYNAPTICS_MAX_SLOTS; ++i) {
		if (syn_contact[i].down) {
			hakios_touch_post(HAKIOS_TOUCH_UP, i, syn_contact[i].x, syn_contact[i].y);
			syn_contact[i].down = false;
		}
	}
}

/* Put the two I2C pins back exactly as they were: no other pin was touched. */
static void
syn_restore(void)
{
	if (!syn_gpio_ready) {
		return;
	}
	syn_line(SYNAPTICS_SDA, true);
	syn_line(SYNAPTICS_SCL, true);
	syn_gpio_write(syn_gpio, syn_cfg_off(SYNAPTICS_SDA) + SYNAPTICS_GPIO_IO, syn_saved[1]);
	syn_gpio_write(syn_gpio, syn_cfg_off(SYNAPTICS_SCL) + SYNAPTICS_GPIO_IO, syn_saved[3]);
	syn_gpio_write(syn_gpio, syn_cfg_off(SYNAPTICS_SDA), syn_saved[0]);
	syn_gpio_write(syn_gpio, syn_cfg_off(SYNAPTICS_SCL), syn_saved[2]);
	syn_gpio_ready = false;
}

/*
 * Release RESET99 if the loader left it asserted, and nothing else: no low pulse, no PMIC, no VIO23.
 * The bring-up found it as "output, latched low" (cfg 0x201) on this phone; anything else means
 * somebody else owns the pin and this driver must not fight it.
 */
static bool
syn_reset_release(void)
{
	uint32_t cfg = syn_gpio_read(syn_reset_gpio, 0);
	uint32_t io = syn_gpio_read(syn_reset_gpio, SYNAPTICS_GPIO_IO);

	hakios_tl("synaptics: RESET99 cfg=0x%x io=0x%x\n", cfg, io);
	if ((cfg & SYNAPTICS_CFG_MASK) != 0x201u || (io & 2u) != 0) {
		hakios_tl("synaptics: RESET99 is not asserted the way this phone leaves it; not touching it\n");
		return true;            /* already released, by the loader or an earlier run */
	}
	syn_gpio_write(syn_reset_gpio, SYNAPTICS_GPIO_IO, io | 2u);
	syn_wait(200);
	if ((syn_gpio_read(syn_reset_gpio, SYNAPTICS_GPIO_IO) & 1u) == 0) {
		hakios_tl("synaptics: RESET99 did not rise\n");
		return false;
	}
	hakios_tl("synaptics: RESET99 released\n");
	return true;
}

/* Control 23 says how many slots there are; Control 28 must say an 8-byte report for the layout above. */
static bool
syn_slots_from_controls(uint8_t query_base, uint8_t ctrl_base, unsigned *slots)
{
	uint8_t q[32];
	uint8_t head[2];
	uint8_t size = 0;
	unsigned offset = 0;
	uint8_t report = 0;

	if (!syn_read((uint16_t)(query_base + 4u), &size, 1) || size < 5 || size > sizeof(q)) {
		return false;
	}
	for (unsigned n = 0; n < size; n += SYNAPTICS_MAX_READ) {
		unsigned chunk = size - n < SYNAPTICS_MAX_READ ? size - n : SYNAPTICS_MAX_READ;

		if (!syn_read((uint16_t)(query_base + 5u + n), q + n, chunk)) {
			return false;
		}
	}
	if (!(q[1 + 23 / 8] & (1u << (23 % 8)))) {
		return false;
	}
	for (unsigned n = 0; n < 23; ++n) {
		offset += (q[1 + n / 8] >> (n % 8)) & 1u;
	}
	if (offset > 0xffu - ctrl_base || !syn_read((uint16_t)(ctrl_base + offset), head, sizeof(head))) {
		return false;
	}
	hakios_tl("synaptics: Ctrl23 = 0x%02x 0x%02x\n", head[0], head[1]);
	if ((head[0] & 1u) == 0 || head[1] < 1 || head[1] > SYNAPTICS_MAX_SLOTS) {
		return false;
	}
	*slots = head[1];
	if (q[1 + 28 / 8] & (1u << (28 % 8))) {
		unsigned offset28 = 0;

		for (unsigned n = 0; n < 28; ++n) {
			offset28 += (q[1 + n / 8] >> (n % 8)) & 1u;
		}
		if (offset28 > 0xffu - ctrl_base || !syn_read((uint16_t)(ctrl_base + offset28), &report, 1)) {
			return false;
		}
		hakios_tl("synaptics: Ctrl28 = 0x%02x\n", report);
		if (report != SYNAPTICS_REPORT_BYTES) {
			return false;   /* the report layout below is not this controller's */
		}
	}
	return true;
}

/* Read the tables and decide how to talk to this controller. A re-probe goes through here again. */
static bool
syn_probe(void)
{
	uint8_t f01[6];
	uint8_t f12[6];
	uint8_t query[16];
	unsigned slots = 1;

	syn_release();
	if (!syn_idle()) {
		hakios_tl("synaptics: the bus is stuck (SDA %u, SCL %u)\n", syn_high(SYNAPTICS_SDA), syn_high(SYNAPTICS_SCL));
		return false;
	}
	if (!syn_read(SYNAPTICS_PDT_F01, f01, sizeof(f01))) {
		hakios_tl("synaptics: F01 PDT read failed (stage %u, write failure %u)\n", syn_read_stage, syn_write_failure);
		return false;
	}
	hakios_tl("synaptics: F01 PDT %02x %02x %02x %02x %02x %02x\n", f01[0], f01[1], f01[2], f01[3], f01[4], f01[5]);
	if (f01[0] != 0x2c || f01[1] != 0x23 || f01[2] != 0x0c || f01[3] != 0x06 || f01[5] != 0x01) {
		hakios_tl("synaptics: not an S3330 (F01 identity differs); touch not started\n");
		return false;
	}
	if (!syn_read(SYNAPTICS_PDT_F12, f12, sizeof(f12))) {
		hakios_tl("synaptics: F12 PDT read failed (stage %u)\n", syn_read_stage);
		return false;
	}
	hakios_tl("synaptics: F12 PDT %02x %02x %02x %02x %02x %02x\n", f12[0], f12[1], f12[2], f12[3], f12[4], f12[5]);
	if (f12[0] != 0x49 || f12[1] != 0x00 || f12[2] != 0x12 || f12[3] != 0x08 || f12[5] != 0x12) {
		hakios_tl("synaptics: F12 identity differs (query 0x%02x, base 0x%02x); touch not started\n", f12[0], f12[3]);
		return false;
	}
	if (!syn_read(f12[0], query, sizeof(query))) {
		hakios_tl("synaptics: F12 query read failed (stage %u)\n", syn_read_stage);
		return false;
	}
	hakios_tl("synaptics: F12 query %02x %02x %02x %02x %02x %02x %02x %02x (Query8 length %u)\n",
	    query[0], query[1], query[2], query[3], query[4], query[5], query[6], query[7], query[7]);
	if (syn_fingers_arg != 0) {
		slots = syn_fingers_arg > SYNAPTICS_MAX_SLOTS ? SYNAPTICS_MAX_SLOTS : syn_fingers_arg;
		hakios_tl("synaptics: %u slots, from synaptics_fingers\n", slots);
	} else if (syn_slots_from_controls(f12[0], f12[2], &slots)) {
		hakios_tl("synaptics: %u slots of %u bytes, from Control 23 and 28\n", slots, SYNAPTICS_REPORT_BYTES);
	} else {
		hakios_tl("synaptics: Control 23/28 unread, one slot of %u bytes\n", SYNAPTICS_REPORT_BYTES);
	}
	syn_f01_status = f01[3];
	syn_f01_irq = f01[4];
	syn_f01_ctrl = f01[2];
	syn_f12_data = f12[3];
	syn_slots = slots;
	hakios_tl("synaptics: F01 status 0x%02x, interrupt 0x%02x, control 0x%02x; F12 data 0x%02x\n",
	    syn_f01_status, syn_f01_irq, syn_f01_ctrl, syn_f12_data);
	return true;
}

/*
 * One report of `slots` objects, decoded as the vendor's DSX layout: the low nibble of the first byte
 * is the object's status (0 present-but-unused, 1 finger, anything else is not ours), then x and y.
 */
static bool
syn_decode(const uint8_t *raw, unsigned slot, uint32_t *x, uint32_t *y, bool *present)
{
	const uint8_t *object = raw + slot * SYNAPTICS_SLOT_BYTES;
	uint32_t px = (uint32_t)object[1] | ((uint32_t)object[2] << 8);
	uint32_t py = (uint32_t)object[3] | ((uint32_t)object[4] << 8);

	*present = false;
	if ((object[0] & 0x0fu) == 0) {
		return true;                    /* the slot is not in use */
	}
	if ((object[0] & 0x0fu) != 1 || px >= syn_max_x || py >= syn_max_y) {
		return false;
	}
	*x = px * 65535u / (syn_max_x - 1u);
	*y = py * 65535u / (syn_max_y - 1u);
	*present = true;
	return true;
}

static void
syn_report(void)
{
	uint8_t raw[SYNAPTICS_MAX_SLOTS * SYNAPTICS_SLOT_BYTES];
	uint32_t x[SYNAPTICS_MAX_SLOTS];
	uint32_t y[SYNAPTICS_MAX_SLOTS];
	bool present[SYNAPTICS_MAX_SLOTS];
	bool valid = true;

	if (!syn_read(syn_f12_data, raw, syn_slots * SYNAPTICS_SLOT_BYTES)) {
		if (++syn_bad < SYNAPTICS_BAD_FRAMES) {
			return;                 /* a noisy frame is not a lifted finger */
		}
		syn_release();
		return;
	}
	for (unsigned slot = 0; slot < syn_slots; ++slot) {
		if (!syn_decode(raw, slot, &x[slot], &y[slot], &present[slot])) {
			valid = false;
			break;
		}
	}
	if (!valid) {
		if (++syn_bad < SYNAPTICS_BAD_FRAMES) {
			return;
		}
		syn_release();
		return;
	}
	syn_bad = 0;
	syn_reports++;
	for (unsigned slot = 0; slot < syn_slots; ++slot) {
		if (!present[slot]) {
			if (syn_contact[slot].down) {
				hakios_touch_post(HAKIOS_TOUCH_UP, slot, syn_contact[slot].x, syn_contact[slot].y);
				syn_contact[slot].down = false;
			}
			continue;
		}
		if (!syn_contact[slot].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, slot, x[slot], y[slot]);
			syn_contact[slot].down = true;
		} else if (x[slot] != syn_contact[slot].x || y[slot] != syn_contact[slot].y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, slot, x[slot], y[slot]);
		} else {
			continue;
		}
		syn_contact[slot].x = x[slot];
		syn_contact[slot].y = y[slot];
		if (syn_trace) {
			hakios_tl("synaptics: slot %u at %u,%u\n", slot, x[slot], y[slot]);
		}
	}
}

static void
syn_thread(void *arg __unused, wait_result_t result __unused)
{
	syn_gpio = ml_io_map(SYNAPTICS_TLMM_NORTH, SYNAPTICS_GPIO_WINDOW);
	syn_reset_gpio = ml_io_map(SYNAPTICS_TLMM_NORTH + syn_cfg_off(SYNAPTICS_RESET), SYNAPTICS_GPIO_STRIDE);
	if (syn_gpio == 0 || syn_reset_gpio == 0) {
		hakios_tl("synaptics: cannot map the TLMM's north tile\n");
		goto done;
	}
	syn_saved[0] = syn_gpio_read(syn_gpio, syn_cfg_off(SYNAPTICS_SDA));
	syn_saved[1] = syn_gpio_read(syn_gpio, syn_cfg_off(SYNAPTICS_SDA) + SYNAPTICS_GPIO_IO);
	syn_saved[2] = syn_gpio_read(syn_gpio, syn_cfg_off(SYNAPTICS_SCL));
	syn_saved[3] = syn_gpio_read(syn_gpio, syn_cfg_off(SYNAPTICS_SCL) + SYNAPTICS_GPIO_IO);
	syn_gpio_ready = true;
	hakios_tl("synaptics: GPIO33 cfg=0x%x io=0x%x, GPIO34 cfg=0x%x io=0x%x\n",
	    syn_saved[0], syn_saved[1], syn_saved[2], syn_saved[3]);
	if (syn_reset_arg != 0 && !syn_reset_release()) {
		goto restore;
	}
	if (syn_reset_arg == 0) {
		hakios_tl("synaptics: leaving RESET99 unchanged by request\n");
	}
	for (unsigned attempt = 0; ; ++attempt) {
		if (!syn_probe()) {
			if (attempt + 1 < 3) {
				syn_wait(500);
				continue;
			}
			hakios_tl("synaptics: the controller did not answer three times; touch not started\n");
			break;
		}
		syn_bad = 0;
		/*
		 * The F01 interrupt status is not a gate. Its interrupt enable bit (F01 Control 0, bit 0)
		 * is whatever the vendor driver left in the controller, and on this phone it is read back
		 * clear: polling it reported "no interrupt" for every frame, forever, while the data base
		 * kept answering. The bring-up that first worked here read the F12 data base directly, so
		 * this does the same and only stops when the controller itself stops answering.
		 */
		for (;;) {
			syn_frames++;
			syn_report();
			if (syn_bad >= SYNAPTICS_BAD_FRAMES) {
				hakios_tl("synaptics: the controller stopped answering after %u reports\n", syn_reports);
				break;
			}
			syn_wait_us(syn_poll_us);
		}
		/* The bus or the controller got lost: read the tables again, a few times, then give up. */
		syn_release();
		syn_wait(200);
	}
	hakios_tl("synaptics: stopped after %u frames and %u reports (%u bad reads)\n",
	    syn_frames, syn_reports, syn_bad_reads);
restore:
	syn_release();
	syn_restore();
done:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

__startup_func
static void
synaptics_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	(void)PE_parse_boot_argn("synaptics_reset", &syn_reset_arg, sizeof(syn_reset_arg));
	(void)PE_parse_boot_argn("synaptics_fingers", &syn_fingers_arg, sizeof(syn_fingers_arg));
	(void)PE_parse_boot_argn("synaptics_poll_us", &syn_poll_us, sizeof(syn_poll_us));
	(void)PE_parse_boot_argn("synaptics_max_x", &syn_max_x, sizeof(syn_max_x));
	(void)PE_parse_boot_argn("synaptics_max_y", &syn_max_y, sizeof(syn_max_y));
	(void)PE_parse_boot_argn("synaptics_trace", &syn_trace, sizeof(syn_trace));
	if (syn_poll_us < 500) {
		syn_poll_us = 500;
	} else if (syn_poll_us > 1000000) {
		syn_poll_us = 1000000;
	}
	if (syn_max_x < 2) {
		syn_max_x = 1080;
	}
	if (syn_max_y < 2) {
		syn_max_y = 2160;
	}
	hakios_tl("synaptics: init reset=%u fingers=%u poll=%uus panel=%ux%u\n",
	    syn_reset_arg, syn_fingers_arg, syn_poll_us, syn_max_x, syn_max_y);
	if (kernel_thread_start(syn_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		hakios_tl("synaptics: cannot start the worker thread\n");
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, synaptics_init);
