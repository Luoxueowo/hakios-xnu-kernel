/*
 * The buttons of the Xiaomi MIX 2S (polaris) -- power, volume up, volume down -- read from the
 * PM8998 over the Qualcomm SPMI arbiter, as a source of button records for /dev/hakios-touch
 * (sys/hakios.input.h). Runs with buttons=1, or with touchscreen=1 when buttons= is absent; the older
 * polarisbuttons=1 spelling remains accepted for existing run scripts.
 *
 * This is the bring-up that worked on this phone (hakios-polaris-old,
 * bsd/dev/polaris_buttons.c), ported to devices/polaris/drivers/input/hakios.buttons.polaris.c.
 * The common driver's hakios.buttons.c is the POCO X5 Pro's PMK8350/PM7325 map and cannot serve here.
 * The loader describes the board
 * in /arm-io/polaris-buttons, so no PM8998 address is hard-coded in the read path; a loader that
 * predates that node is recognised by /arm-io's device_type and served the same map from here.
 *
 * Power is PM8998 PON KPDPWR_N, volume down is PON RESIN_N, and volume up is PM8998 GPIO6
 * (active low). THIS DRIVER ONLY READS: nothing is written to a PMIC register or a pin's
 * configuration, and a key whose input is not configured the way the firmware leaves it is left
 * alone. Records go out through hakios_touch_post(), the same queue the touch screen uses, so
 * backboardd's HID side needs nothing new.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

#define POLARIS_BUTTONS_PATH       "/arm-io/polaris-buttons"
#define ARB_CHANNELS              512
#define ARB_VERSION                0x000
#define ARB_FEATURES               0x004
#define ARB_CHANNEL(n)             (0x900 + 4 * (n))
#define ARB_OWNER(n)               (0x700 + 4 * (n))
#define OBS_COMMAND                0x00
#define OBS_STATUS                 0x08
#define OBS_DATA0                  0x18
#define OBS_DATA1                  0x1c
#define OBS_DONE                   0x1
#define OBS_FAILURE                0x2
#define OBS_DENIED                 0x4
#define OBS_DROPPED                0x8
#define OBS_READ                   (1u << 27)
#define REG_TYPE                   0x04
#define REG_SUBTYPE                0x05
#define REG_STATUS                 0x10
#define REG_GPIO_MODE              0x40
#define REG_GPIO_PULL              0x42
#define REG_GPIO_ENABLE            0x46
#define PON_POWER                  0x01
#define PON_RESIN                  0x02
#define GPIO_LEVEL                 0x01
#define POLL_MS                    16

static vm_offset_t arb_core, arb_config, arb_observer;
static uint32_t pon_id, gpio_id, gpio_number, active_low, debounce_ms;
static uint16_t pon_channel, gpio_channel;
static bool have_pon, have_gpio;
static uint8_t last_pon_status = 0xff, last_gpio_status = 0xff;

/* Share the SPMI transaction lock with the existing Redwood torch/key readers. */
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

static void
button_log(const char *format, ...) __printflike(1, 2);

static void
button_log(const char *format, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	for (const char *p = line; *p; p++) {
		loader_log_putc(*p);
	}
	printf("%s", line);
}

static bool
dt_u32(DTEntry node, const char *name, uint32_t *value)
{
	const void *property = NULL;
	unsigned int size = 0;
	if (SecureDTGetProperty(node, name, &property, &size) != kSuccess || property == NULL || size != sizeof(*value)) {
		return false;
	}
	*value = *(const uint32_t *)property;
	return true;
}

static bool
button_read(const DTEntry node, const char *property, uint32_t *value)
{
	return dt_u32(node, property, value);
}

static bool
load_board_description(void)
{
	DTEntry node = NULL;
	uint32_t core_pa, config_pa, observer_pa, pon_sid, pon_periph, gpio_sid, gpio_periph;

	if (SecureDTLookupEntry(NULL, POLARIS_BUTTONS_PATH, &node) == kSuccess && node != NULL) {
		const void *compatible = NULL;
		unsigned int compatible_size = 0;
		if (SecureDTGetProperty(node, "compatible", &compatible, &compatible_size) != kSuccess || compatible == NULL ||
		    compatible_size != sizeof("xiaomi,polaris-pm8998-buttons") ||
		    strcmp((const char *)compatible, "xiaomi,polaris-pm8998-buttons") != 0) {
			button_log("polaris-buttons: unexpected compatible string; disabled\n");
			return false;
		}
		if (!button_read(node, "arbiter-core", &core_pa) || !button_read(node, "arbiter-config", &config_pa) ||
		    !button_read(node, "arbiter-observer", &observer_pa) || !button_read(node, "pon-sid", &pon_sid) ||
		    !button_read(node, "pon-peripheral", &pon_periph) || !button_read(node, "gpio-sid", &gpio_sid) ||
		    !button_read(node, "gpio-peripheral", &gpio_periph) || !button_read(node, "gpio-number", &gpio_number) ||
		    !button_read(node, "gpio-active-low", &active_low) || !button_read(node, "debounce-ms", &debounce_ms)) {
			button_log("polaris-buttons: incomplete PM8998 device-tree node; disabled\n");
			return false;
		}
	} else {
		/* Transition support for an older loader that predates /arm-io/polaris-buttons. */
		const void *board = NULL;
		unsigned int board_size = 0;
		if (SecureDTLookupEntry(NULL, "/arm-io", &node) != kSuccess || node == NULL ||
		    SecureDTGetProperty(node, "device_type", &board, &board_size) != kSuccess || board == NULL ||
		    board_size != sizeof("polaris") || strcmp((const char *)board, "polaris") != 0) {
			button_log("polaris-buttons: no Polaris PM8998 device-tree description; disabled\n");
			return false;
		}
		button_log("polaris-buttons: using legacy-loader PM8998 defaults (device_type=polaris)\n");
		core_pa = 0x0c440000;
		config_pa = 0x0c40a000;
		observer_pa = 0x0e600000;
		pon_sid = 0;
		pon_periph = 0x08;
		gpio_sid = 0;
		gpio_periph = 0xc5;
		gpio_number = 6;
		active_low = 1;
		debounce_ms = 15;
	}
	pon_id = (pon_sid << 8) | pon_periph;
	gpio_id = (gpio_sid << 8) | gpio_periph;
	arb_core = ml_io_map(core_pa, 0x1100);
	arb_config = ml_io_map(config_pa, 0x1000);
	arb_observer = ml_io_map(observer_pa, 0x10000);
	return arb_core != 0 && arb_config != 0 && arb_observer != 0;
}

static uint32_t
mmio_read(vm_offset_t base, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(base + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
mmio_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
find_channel(uint32_t id, uint16_t *channel)
{
	uint32_t count = mmio_read(arb_core, ARB_FEATURES) & 0x7ff;
	if (count > ARB_CHANNELS) {
		count = ARB_CHANNELS;
	}
	bool found = false, selected_hlos = false;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t map = mmio_read(arb_core, ARB_CHANNEL(i));
		if (map != 0 && ((map >> 8) & 0xfff) == id) {
			uint32_t owner = mmio_read(arb_config, ARB_OWNER(i)) & 7;
			button_log("polaris-buttons: SPMI channel %u maps %03x, owner %u%s\n", i, id, owner,
			    owner == 0 ? " (HLOS)" : "");
			/* If the arbiter exposes several channels, use one owned by HLOS. */
			if (!found || (!selected_hlos && owner == 0)) {
				*channel = (uint16_t)i;
				found = true;
				selected_hlos = owner == 0;
			}
		}
	}
	if (found) {
		return true;
	}
	button_log("polaris-buttons: no SPMI channel for SID/peripheral %#x\n", id);
	return false;
}

static bool
spmi_read(uint16_t channel_id, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x80UL * channel_id;
	uint64_t start = mach_absolute_time(), timeout, now;
	uint32_t status, word0, word1 = 0;
	nanoseconds_to_absolutetime(2000ULL * NSEC_PER_USEC, &timeout);
	hakios_spmi_lock();
	mmio_write(channel, OBS_COMMAND, OBS_READ | ((uint32_t)reg << 4) | (count - 1));
	do {
		status = mmio_read(channel, OBS_STATUS);
		now = mach_absolute_time();
	} while (!(status & OBS_DONE) && now - start < timeout);
	word0 = mmio_read(channel, OBS_DATA0);
	if (count > 4) {
		word1 = mmio_read(channel, OBS_DATA1);
	}
	hakios_spmi_unlock();
	if (!(status & OBS_DONE) || (status & (OBS_FAILURE | OBS_DENIED | OBS_DROPPED))) {
		button_log("polaris-buttons: SPMI read sid/periph=%#x reg=%#x failed status=%#x\n",
		    channel_id, reg, status);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)((i < 4 ? word0 : word1) >> (8 * (i & 3)));
	}
	return true;
}

struct polaris_button {
	const char *name;
	uint32_t event_code;
	bool usable, initialized, previous, down, armed;
	uint8_t stable_samples;
};

static struct polaris_button power_key = { "power", HAKIOS_BUTTON_LOCK };
static struct polaris_button volume_up_key = { "volume-up", HAKIOS_BUTTON_VOLUME_UP };
static struct polaris_button volume_down_key = { "volume-down", HAKIOS_BUTTON_VOLUME_DOWN };

static void
report_key(struct polaris_button *key, bool pressed)
{
	if (!key->usable) {
		return;
	}
	if (!key->initialized) {
		key->previous = pressed;
		key->initialized = true;
		key->armed = !pressed;
		return;
	}
	if (pressed != key->previous) {
		key->previous = pressed;
		key->stable_samples = 0;
		return;
	}
	if (key->stable_samples < MAX(1U, debounce_ms / POLL_MS)) {
		key->stable_samples++;
		return;
	}
	if (!key->armed) {
		if (!pressed) {
			key->armed = true;
		}
		return;
	}
	if (pressed == key->down) {
		return;
	}
	key->down = pressed;
	button_log("polaris-buttons: %s %s\n", key->name, pressed ? "PRESSED" : "released");
	hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, key->event_code, 0, 0);
}

static void
poll_buttons(void)
{
	uint8_t status;
	if (have_pon && spmi_read(pon_channel, REG_STATUS, &status, 1)) {
		if (status != last_pon_status) {
			button_log("polaris-buttons: PON RT status=%#x (power=%u resin=%u)\n",
			    status, !!(status & PON_POWER), !!(status & PON_RESIN));
			last_pon_status = status;
		}
		report_key(&power_key, (status & PON_POWER) != 0);
		report_key(&volume_down_key, (status & PON_RESIN) != 0);
	}
	if (have_gpio && spmi_read(gpio_channel, REG_STATUS, &status, 1)) {
		if (status != last_gpio_status) {
			button_log("polaris-buttons: GPIO%u RT status=%#x level=%u\n",
			    gpio_number, status, !!(status & GPIO_LEVEL));
			last_gpio_status = status;
		}
		bool high = (status & GPIO_LEVEL) != 0;
		report_key(&volume_up_key, active_low ? !high : high);
	}
}

static void
buttons_thread(void *arg, __unused wait_result_t wait)
{
	uint32_t ticks = 0;
	(void)arg;
	for (;;) {
		poll_buttons();
		if (++ticks % (60 * 1000 / POLL_MS) == 0) {
			button_log("polaris-buttons: polling power=%u volume-up=%u volume-down=%u\n",
			    power_key.usable, volume_up_key.usable, volume_down_key.usable);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}

__startup_func
static void
buttons_polaris_init(void)
{
	uint32_t enabled = 0, touch = 0;
	uint8_t type[2], setup[8], level;
	thread_t thread;

	/* An explicit buttons= value wins, then the old alias, then touchscreen's default for phone buttons. */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		if (!PE_parse_boot_argn("polarisbuttons", &enabled, sizeof(enabled))) {
			enabled = PE_parse_boot_argn("touchscreen", &touch, sizeof(touch)) && touch != 0;
		}
	}
	if (enabled != 1) {
		return;
	}
	if (!load_board_description()) {
		button_log("polaris-buttons: cannot map PMIC arbiter; disabled\n");
		return;
	}
	button_log("polaris-buttons: PM8998 arbiter version %#x, PON=%#x GPIO%u=%#x debounce=%ums\n",
	    mmio_read(arb_core, ARB_VERSION), pon_id, gpio_number, gpio_id, debounce_ms);
	if (gpio_number != 6 || debounce_ms > 100) {
		button_log("polaris-buttons: unexpected board key description; disabled\n");
		return;
	}
	have_pon = find_channel(pon_id, &pon_channel);
	have_gpio = find_channel(gpio_id, &gpio_channel);
	if (have_pon && spmi_read(pon_channel, REG_TYPE, type, 2)) {
		button_log("polaris-buttons: PON type=%#x subtype=%#x (expected PM8998 PON)\n", type[0], type[1]);
		/* Keep the keys gated until the status read confirms a working peripheral. */
		if (type[0] == 0x01) {
			power_key.usable = volume_down_key.usable = true;
		}
	}
	if (have_gpio && spmi_read(gpio_channel, REG_TYPE, type, 2) && type[0] == 0x10 &&
	    spmi_read(gpio_channel, REG_GPIO_MODE, setup, 8) && spmi_read(gpio_channel, REG_STATUS, &level, 1)) {
		unsigned mode = setup[0] & 3;
		unsigned pull = setup[REG_GPIO_PULL - REG_GPIO_MODE] & 7;
		bool enabled_pin = (setup[REG_GPIO_ENABLE - REG_GPIO_MODE] & 0x80) != 0;
		button_log("polaris-buttons: GPIO6 type=%#x subtype=%#x mode=%u pull=%u enabled=%u level=%s\n",
		    type[0], type[1], mode, pull, enabled_pin, (level & GPIO_LEVEL) ? "high" : "low");
		/* Only consume a firmware-configured input with a pull-up; never reprogram the PMIC. */
		if (mode == 0 && pull < 4 && enabled_pin) {
			volume_up_key.usable = true;
		}
	} else if (have_gpio) {
		button_log("polaris-buttons: unable to identify or inspect PM8998 GPIO6\n");
	}
	if (!power_key.usable && !volume_up_key.usable && !volume_down_key.usable) {
		button_log("polaris-buttons: no verified key inputs; disabled\n");
		return;
	}
	button_log("polaris-buttons: starting read-only key polling\n");
	if (kernel_thread_start(buttons_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		button_log("polaris-buttons: could not start polling thread\n");
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, buttons_polaris_init);
