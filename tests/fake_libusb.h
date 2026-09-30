/*
 * Minimal libusb replacement that emulates just enough of an RTL2832U with an
 * R820T / E4000 tuner and a 24C02 EEPROM to exercise librtlsdr without
 * hardware. Test-only code.
 */
#ifndef FAKE_LIBUSB_H
#define FAKE_LIBUSB_H

#include <stdint.h>

/* knobs */
extern int fake_num_devices;		/* number of RTL dongles on the bus (default 1) */
extern int fake_list_fail;		/* libusb_get_device_list() fails */
extern int fake_tuner_addr;		/* 0x34 = R820T (default), 0xc8 = E4000, 0 = none */
extern int fake_fail_eeprom;		/* EEPROM reads fail with LIBUSB_ERROR_IO */
extern int fake_fail_tuner_reads;	/* tuner register reads fail */
extern int fake_fail_submit_after;	/* libusb_submit_transfer() fails from the Nth call on (-1 = never) */
extern int fake_events_error_at;	/* Nth handle_events call fails once (-1 = never) */
extern uint8_t fake_eeprom[256];

/* observation */
int fake_pending_transfers(void);
int fake_open_handles(void);
unsigned fake_gpo_writes(void);		/* number of writes to the GPO register */
uint8_t fake_last_gpo(void);
void fake_reset(void);
uint8_t fake_tuner_reg(int reg);	/* last value written to a tuner register */

#endif
