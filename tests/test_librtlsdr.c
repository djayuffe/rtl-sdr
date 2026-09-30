/*
 * librtlsdr regression tests running against the fake libusb.
 * Build with -fsanitize=address,undefined for best effect.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl-sdr.h"
#include "fake_libusb.h"

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static rtlsdr_dev_t *open_dev(void)
{
	rtlsdr_dev_t *dev = NULL;
	fake_reset();
	CHECK(rtlsdr_open(&dev, 0) == 0);
	return dev;
}

static void test_enumeration(void)
{
	char m[256], p[256], s[256];
	rtlsdr_dev_t *dev = NULL;

	fake_reset();
	CHECK(rtlsdr_get_device_count() == 1);
	CHECK(strlen(rtlsdr_get_device_name(0)) > 0);
	/* used to return the name of the last known device */
	CHECK(strcmp(rtlsdr_get_device_name(5), "") == 0);
	CHECK(rtlsdr_get_device_usb_strings(0, m, p, s) == 0);
	CHECK(strcmp(s, "00000001") == 0);
	/* used to return 0 ("success") and leave the buffers untouched */
	CHECK(rtlsdr_get_device_usb_strings(3, m, p, s) != 0);
	CHECK(rtlsdr_get_index_by_serial("nonexistent") < 0);
	CHECK(rtlsdr_get_index_by_serial("00000001") == 0);

	/* libusb_get_device_list() failure must not crash any of them */
	fake_list_fail = 1;
	CHECK(rtlsdr_get_device_count() == 0);
	CHECK(strcmp(rtlsdr_get_device_name(0), "") == 0);
	CHECK(rtlsdr_get_device_usb_strings(0, m, p, s) != 0);
	CHECK(rtlsdr_open(&dev, 0) < 0);
	CHECK(fake_open_handles() == 0);

	/* an index that would match "no known device yet" must not open the
	 * unrelated USB device that is first on the bus */
	fake_reset();
	CHECK(rtlsdr_open(&dev, 0xffffffffu) < 0);
	CHECK(rtlsdr_open(&dev, 1) < 0);
	CHECK(fake_open_handles() == 0);
}

static void test_tuning(void)
{
	rtlsdr_dev_t *dev = open_dev();
	int gains[64];

	CHECK(dev != NULL);
	if (!dev)
		return;
	CHECK(rtlsdr_get_tuner_type(dev) == RTLSDR_TUNER_R820T);
	CHECK(rtlsdr_get_tuner_gains(dev, NULL) == 29);
	CHECK(rtlsdr_get_tuner_gains(dev, gains) == 29);

	/* before the first frequency is set nothing may be retuned to 0 Hz */
	CHECK(rtlsdr_set_sample_rate(dev, 2048000) == 0);
	CHECK(rtlsdr_get_center_freq(dev) == 0);
	CHECK(rtlsdr_set_sample_rate(dev, 100000) < 0);
	CHECK(rtlsdr_set_sample_rate(dev, 500000) < 0);

	CHECK(rtlsdr_set_center_freq(dev, 100000000) == 0);
	CHECK(rtlsdr_get_center_freq(dev) == 100000000);
	/* 1.7 GHz is in range */
	CHECK(rtlsdr_set_center_freq(dev, 1700000000) == 0);
	/* out of the PLL range: used to program a bogus divider and "succeed" */
	CHECK(rtlsdr_set_center_freq(dev, 10000000) < 0);
	CHECK(rtlsdr_get_center_freq(dev) == 0);
	CHECK(rtlsdr_set_center_freq(dev, 2500000000u) < 0);
	CHECK(rtlsdr_set_center_freq(dev, 98000000) == 0);

	/* ppm range */
	CHECK(rtlsdr_set_freq_correction(dev, -1000000) == -EINVAL);
	CHECK(rtlsdr_set_freq_correction(dev, 1000000) == -EINVAL);
	CHECK(rtlsdr_set_freq_correction(dev, 5000) == 0);	/* saturates the 14 bit register */
	CHECK(rtlsdr_get_freq_correction(dev) == 5000);
	CHECK(rtlsdr_set_freq_correction(dev, -5000) == 0);
	CHECK(rtlsdr_set_freq_correction(dev, 0) == 0);

	/* argument ranges */
	CHECK(rtlsdr_set_direct_sampling(dev, 3) == -EINVAL);
	CHECK(rtlsdr_set_direct_sampling(dev, -1) == -EINVAL);
	CHECK(rtlsdr_set_tuner_bandwidth(dev, 0x80000000u) == -EINVAL);

	CHECK(rtlsdr_close(dev) == 0);
	CHECK(fake_open_handles() == 0);
}

static void test_bias_tee(void)
{
	rtlsdr_dev_t *dev = open_dev();
	unsigned before;

	CHECK(dev != NULL);
	if (!dev)
		return;
	before = fake_gpo_writes();
	/* pin 256 was truncated to pin 0 (the bias tee) */
	CHECK(rtlsdr_set_bias_tee_gpio(dev, 256, 1) == -EINVAL);
	CHECK(rtlsdr_set_bias_tee_gpio(dev, -1, 1) == -EINVAL);
	CHECK(rtlsdr_set_bias_tee_gpio(dev, 8, 1) == -EINVAL);
	CHECK(fake_gpo_writes() == before);
	CHECK(rtlsdr_set_bias_tee_gpio(dev, 0, 1) == 0);
	CHECK(fake_gpo_writes() > before);
	CHECK(rtlsdr_close(dev) == 0);
}

static void test_eeprom(void)
{
	rtlsdr_dev_t *dev = open_dev();
	uint8_t buf[256], data[16];
	int i;

	CHECK(dev != NULL);
	if (!dev)
		return;

	memset(buf, 0, sizeof(buf));
	/* documented as 0 on success (used to return the last transfer size) */
	CHECK(rtlsdr_read_eeprom(dev, buf, 0, 256) == 0);
	CHECK(memcmp(buf, fake_eeprom, 256) == 0);
	CHECK(rtlsdr_read_eeprom(dev, buf, 0, 257) == -2);
	CHECK(rtlsdr_read_eeprom(dev, buf, 250, 10) == -2);
	CHECK(rtlsdr_read_eeprom(dev, NULL, 0, 4) == -1);
	CHECK(rtlsdr_write_eeprom(dev, NULL, 0, 4) == -1);
	CHECK(rtlsdr_write_eeprom(dev, data, 250, 10) == -2);

	for (i = 0; i < 16; i++)
		data[i] = (uint8_t)(0xa0 + i);
	CHECK(rtlsdr_write_eeprom(dev, data, 128, 16) == 0);
	CHECK(memcmp(fake_eeprom + 128, data, 16) == 0);

	fake_fail_eeprom = 1;
	CHECK(rtlsdr_read_eeprom(dev, buf, 0, 8) == -3);
	/* used to write on top of an uninitialised comparison byte */
	CHECK(rtlsdr_write_eeprom(dev, data, 0, 8) == -3);
	fake_fail_eeprom = 0;
	CHECK(rtlsdr_close(dev) == 0);
}

/* asynchronous reading ----------------------------------------------------*/
static int cb_count;
static int cb_limit;
static rtlsdr_dev_t *cb_dev;

static void count_cb(unsigned char *buf, uint32_t len, void *ctx)
{
	cb_count++;
	if (cb_count >= cb_limit)
		rtlsdr_cancel_async(cb_dev);
}

static void test_async(void)
{
	rtlsdr_dev_t *dev;
	int r;

	dev = open_dev();
	CHECK(dev != NULL);
	if (!dev)
		return;
	cb_dev = dev;

	/* normal cancel */
	cb_count = 0; cb_limit = 20;
	r = rtlsdr_read_async(dev, count_cb, NULL, 8, 16384);
	CHECK(r == 0);
	CHECK(cb_count >= 20);
	CHECK(fake_pending_transfers() == 0);

	/* the API must be reusable after a cancel */
	cb_count = 0; cb_limit = 5;
	r = rtlsdr_read_async(dev, count_cb, NULL, 4, 16384);
	CHECK(r == 0);
	CHECK(fake_pending_transfers() == 0);

	/* submit fails from the 3rd transfer on: used to report success */
	fake_fail_submit_after = 2;
	cb_count = 0; cb_limit = 1000000;
	r = rtlsdr_read_async(dev, count_cb, NULL, 8, 16384);
	CHECK(r < 0);
	CHECK(fake_pending_transfers() == 0);
	fake_fail_submit_after = -1;

	/* resubmit fails during streaming: the stream must end, not stall */
	fake_fail_submit_after = 8 + 4;
	cb_count = 0; cb_limit = 1000000;
	r = rtlsdr_read_async(dev, count_cb, NULL, 8, 16384);
	CHECK(r < 0);
	CHECK(fake_pending_transfers() == 0);
	fake_fail_submit_after = -1;

	CHECK(rtlsdr_close(dev) == 0);
	CHECK(fake_open_handles() == 0);

	/* libusb_handle_events() fails once while all transfers are in flight:
	 * buffers must not be freed before the transfers are reaped */
	dev = open_dev();
	CHECK(dev != NULL);
	if (!dev)
		return;
	cb_dev = dev;
	fake_events_error_at = 3;
	cb_count = 0; cb_limit = 1000000;
	r = rtlsdr_read_async(dev, count_cb, NULL, 8, 16384);
	CHECK(r < 0);
	CHECK(fake_pending_transfers() == 0);
	CHECK(rtlsdr_close(dev) == 0);
}

static void test_e4k(void)
{
	rtlsdr_dev_t *dev;

	fake_reset();
	fake_tuner_addr = 0xc8;
	dev = NULL;
	CHECK(rtlsdr_open(&dev, 0) == 0);
	CHECK(dev != NULL);
	if (!dev)
		return;
	CHECK(rtlsdr_get_tuner_type(dev) == RTLSDR_TUNER_E4000);
	CHECK(rtlsdr_set_sample_rate(dev, 2048000) == 0);
	CHECK(rtlsdr_set_center_freq(dev, 100000000) == 0);
	/* Z would not fit its 8 bit register: used to wrap silently */
	CHECK(rtlsdr_set_center_freq(dev, 4000000000u) < 0);
	CHECK(rtlsdr_set_center_freq(dev, 100000000) == 0);
	/* a failing register read used to count as "PLL locked" */
	fake_fail_tuner_reads = 1;
	CHECK(rtlsdr_set_center_freq(dev, 101000000) < 0);
	fake_fail_tuner_reads = 0;
	CHECK(rtlsdr_close(dev) == 0);
}

int main(void)
{
	test_enumeration();
	test_tuning();
	test_bias_tee();
	test_eeprom();
	test_async();
	test_e4k();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_librtlsdr: all checks passed\n");
	return 0;
}
