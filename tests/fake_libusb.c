#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libusb.h>

#include "fake_libusb.h"

struct libusb_context { int dummy; };
struct libusb_device { uint16_t vid, pid; };
struct libusb_device_handle { struct libusb_device *dev; };

int fake_num_devices = 1;
int fake_list_fail = 0;
int fake_tuner_addr = 0x34;
int fake_fail_eeprom = 0;
int fake_fail_tuner_reads = 0;
int fake_fail_submit_after = -1;
int fake_events_error_at = -1;
uint8_t fake_eeprom[256];

static int open_handles;
static int submit_calls;
static int event_calls;
/* the failure knobs count calls from the moment they are armed */
static int submit_base = -1;
static int event_base = -1;
static unsigned gpo_writes;
static uint8_t last_gpo;

#define MAX_PENDING 64
static struct libusb_transfer *pending[MAX_PENDING];
static int cancel_req[MAX_PENDING];

static uint8_t ee_ptr;
static uint8_t tuner_ptr;
static uint8_t tuner_regs[256];
static int testmode;		/* demod reg 0x19 == 0x03: device streams a byte counter */
static uint8_t counter;

static void fill_stream(unsigned char *buf, int len)
{
	int i;
	if (testmode) {
		for (i = 0; i < len; i++)
			buf[i] = counter++;
	} else {
		/* ~N(127.5, 1.5) noise from a small LCG + CLT */
		static uint32_t lcg = 1;
		for (i = 0; i < len; i++) {
			int s = 0, k;
			for (k = 0; k < 4; k++) {
				lcg = lcg * 1664525u + 1013904223u;
				s += (int)((lcg >> 24) & 3) - 1;	/* -1..2, mean 0.5 */
			}
			buf[i] = (uint8_t)(126 + s);
		}
	}
}

uint8_t fake_tuner_reg(int reg) { return tuner_regs[reg & 0xff]; }

void fake_reset(void)
{
	int i;
	fake_num_devices = 1;
	fake_list_fail = 0;
	fake_tuner_addr = 0x34;
	fake_fail_eeprom = 0;
	fake_fail_tuner_reads = 0;
	fake_fail_submit_after = -1;
	fake_events_error_at = -1;
	submit_calls = 0;
	event_calls = 0;
	submit_base = event_base = -1;
	testmode = 0;
	counter = 0;
	gpo_writes = 0;
	last_gpo = 0;
	for (i = 0; i < MAX_PENDING; i++) {
		pending[i] = NULL;
		cancel_req[i] = 0;
	}
	memset(fake_eeprom, 0xff, sizeof(fake_eeprom));
	/* valid RTL2838 EEPROM header: 28 32, vid/pid, serial flag, ... */
	fake_eeprom[0] = 0x28; fake_eeprom[1] = 0x32;
	fake_eeprom[2] = 0xda; fake_eeprom[3] = 0x0b;
	fake_eeprom[4] = 0x38; fake_eeprom[5] = 0x28;
	fake_eeprom[6] = 0xa5; fake_eeprom[7] = 0x16;
}

int fake_pending_transfers(void)
{
	int i, n = 0;
	for (i = 0; i < MAX_PENDING; i++)
		if (pending[i])
			n++;
	return n;
}
int fake_open_handles(void) { return open_handles; }
unsigned fake_gpo_writes(void) { return gpo_writes; }
uint8_t fake_last_gpo(void) { return last_gpo; }

int libusb_init(libusb_context **ctx)
{
	*ctx = calloc(1, sizeof(**ctx));
	return *ctx ? 0 : LIBUSB_ERROR_NO_MEM;
}
void libusb_exit(libusb_context *ctx) { free(ctx); }

/* devices are reference counted in real libusb and outlive the list */
static struct libusb_device pool[16];

ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
	int n = fake_num_devices + 1, i;
	libusb_device **l;

	if (fake_list_fail)
		return LIBUSB_ERROR_OTHER;	/* *list stays untouched, like libusb */

	if (n > 16)
		n = 16;
	l = calloc(n + 1, sizeof(*l));
	for (i = 0; i < n; i++) {
		l[i] = &pool[i];
		if (i == 0) {			/* an unrelated USB device first */
			l[i]->vid = 0x1234; l[i]->pid = 0x5678;
		} else {
			l[i]->vid = 0x0bda; l[i]->pid = 0x2838;
		}
	}
	*list = l;
	return n;
}
void libusb_free_device_list(libusb_device **list, int unref)
{
	free(list);
}
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *dd)
{
	memset(dd, 0, sizeof(*dd));
	dd->idVendor = dev->vid;
	dd->idProduct = dev->pid;
	dd->iManufacturer = 1;
	dd->iProduct = 2;
	dd->iSerialNumber = 3;
	return 0;
}
int libusb_open(libusb_device *dev, libusb_device_handle **h)
{
	*h = calloc(1, sizeof(**h));
	(*h)->dev = dev;
	open_handles++;
	return 0;
}
void libusb_close(libusb_device_handle *h) { open_handles--; free(h); }
libusb_device *libusb_get_device(libusb_device_handle *h) { return h->dev; }
int libusb_kernel_driver_active(libusb_device_handle *h, int i) { return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { return 0; }
int libusb_reset_device(libusb_device_handle *h) { return 0; }
int libusb_dev_mem_free(libusb_device_handle *h, unsigned char *b, size_t l) { return 0; }

int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx,
				       unsigned char *data, int length)
{
	const char *s = idx == 1 ? "Realtek" : idx == 2 ? "RTL2838UHIDIR" : "00000001";
	strncpy((char *)data, s, length - 1);
	data[length - 1] = 0;
	return (int)strlen((char *)data);
}

static uint8_t tuner_reg_value(int addr, int reg)
{
	if (addr == 0x34 || addr == 0x74) {
		if (reg == 0x00)
			return 0x69;		/* R82XX_CHECK_VAL, also read back bit-reversed */
		return 0xff;			/* PLL locked, fine tune etc. */
	}
	if (addr == 0xc8) {
		if (reg == 0x02)
			return 0x40;		/* E4K_CHECK_VAL */
		return 0xff;
	}
	if (addr == 0xc6) {			/* FC0012 (0xa1) / FC0013 (0xa3) */
		if (reg == 0x00)
			return 0xa1;
		return 0x20;			/* VCO calibration result in range */
	}
	return 0;
}

int libusb_control_transfer(libusb_device_handle *h, uint8_t bmRequestType,
			    uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
			    unsigned char *data, uint16_t wLength, unsigned int timeout)
{
	int in = (bmRequestType & 0x80) != 0;
	int block = wIndex >> 8;
	int i;

	if ((wValue & 0xff) == 0x20 && block == 0) {
		/* demod register access */
		if (!in && (wValue >> 8) == 0x19 && (wIndex & 0x0f) == 0 && wLength >= 1)
			testmode = (data[0] == 0x03);
		if (in)
			memset(data, 0, wLength);
		return wLength;
	}

	if (block == 6) {			/* I2C block */
		int addr = wValue;
		if (!in) {
			if (addr == 0xa0) {
				if (wLength == 1) {
					ee_ptr = data[0];
				} else if (wLength == 2) {
					fake_eeprom[data[0]] = data[1];
					ee_ptr = data[0];
				}
			} else if (wLength >= 1) {
				tuner_ptr = data[0];
				if (addr == fake_tuner_addr)
					for (i = 1; i < wLength; i++)
						tuner_regs[(uint8_t)(data[0] + i - 1)] = data[i];
			}
			return wLength;
		}
		if (addr == 0xa0) {
			if (fake_fail_eeprom)
				return LIBUSB_ERROR_IO;
			for (i = 0; i < wLength; i++)
				data[i] = fake_eeprom[(uint8_t)(ee_ptr + i)];
			ee_ptr += wLength;
			return wLength;
		}
		if (fake_fail_tuner_reads && addr == fake_tuner_addr)
			return LIBUSB_ERROR_IO;
		for (i = 0; i < wLength; i++)
			data[i] = (addr == fake_tuner_addr) ?
				tuner_reg_value(addr, tuner_ptr + i) : 0;
		return wLength;
	}

	if (block == 2 && !in && wValue == 0x3001 && wLength >= 1) {
		gpo_writes++;
		last_gpo = data[0];
	}
	if (in)
		memset(data, 0, wLength);
	return wLength;
}

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep,
			 unsigned char *data, int length, int *transferred,
			 unsigned int timeout)
{
	fill_stream(data, length);
	if (transferred)
		*transferred = length;
	return 0;
}

struct libusb_transfer *libusb_alloc_transfer(int iso)
{
	return calloc(1, sizeof(struct libusb_transfer));
}
void libusb_free_transfer(struct libusb_transfer *t) { free(t); }

int libusb_submit_transfer(struct libusb_transfer *t)
{
	int i;
	submit_calls++;
	if (fake_fail_submit_after < 0) {
		submit_base = -1;
	} else {
		if (submit_base < 0)
			submit_base = submit_calls - 1;
		if (submit_calls - submit_base > fake_fail_submit_after)
			return LIBUSB_ERROR_BUSY;
	}
	for (i = 0; i < MAX_PENDING; i++) {
		if (!pending[i]) {
			pending[i] = t;
			cancel_req[i] = 0;
			return 0;
		}
	}
	return LIBUSB_ERROR_NO_MEM;
}

int libusb_cancel_transfer(struct libusb_transfer *t)
{
	int i;
	for (i = 0; i < MAX_PENDING; i++) {
		if (pending[i] == t) {
			if (cancel_req[i])
				return LIBUSB_ERROR_NOT_FOUND;
			cancel_req[i] = 1;
			return 0;
		}
	}
	return LIBUSB_ERROR_NOT_FOUND;
}

int libusb_handle_events_timeout_completed(libusb_context *ctx, struct timeval *tv, int *completed)
{
	struct libusb_transfer *snap[MAX_PENDING];
	int snapc[MAX_PENDING];
	int i;

	event_calls++;
	if (fake_events_error_at < 0) {
		event_base = -1;
	} else {
		if (event_base < 0)
			event_base = event_calls - 1;
		if (event_calls - event_base == fake_events_error_at)
			return LIBUSB_ERROR_IO;
	}

	/* snapshot: callbacks may resubmit */
	for (i = 0; i < MAX_PENDING; i++) {
		snap[i] = pending[i];
		snapc[i] = cancel_req[i];
		pending[i] = NULL;
		cancel_req[i] = 0;
	}
	for (i = 0; i < MAX_PENDING; i++) {
		struct libusb_transfer *t = snap[i];
		if (!t)
			continue;
		if (snapc[i]) {
			t->status = LIBUSB_TRANSFER_CANCELLED;
			t->actual_length = 0;
		} else {
			fill_stream(t->buffer, t->length);
			t->status = LIBUSB_TRANSFER_COMPLETED;
			t->actual_length = t->length;
		}
		snap[i] = NULL;
		t->callback(t);
		if (completed && *completed)
			break;
	}
	/* anything not processed because of an early break stays pending */
	for (i = 0; i < MAX_PENDING; i++) {
		if (snap[i]) {
			int j;
			for (j = 0; j < MAX_PENDING; j++) {
				if (!pending[j]) {
					pending[j] = snap[i];
					cancel_req[j] = snapc[i];
					break;
				}
			}
		}
	}
	return 0;
}
