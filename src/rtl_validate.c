/*
 * rtl_validate, an on-device validation and benchmark tool for RTL2832U
 * based receivers.
 *
 * It exercises the library the way the other tools do and checks the results
 * against what the hardware and the API are supposed to deliver:
 *
 *   identity / EEPROM, API argument contract, sample rate accuracy, gain
 *   table, tuner range sweep with retune latency, PLL lock stress, ADC / IQ
 *   sanity, and streaming (throughput, sample clock error, lost data using
 *   the RTL2832 test-mode byte counter, cancel latency).
 *
 * Every check ends in PASS / WARN / FAIL / SKIP. The exit code is 0 when
 * nothing failed, 1 when at least one check failed, 2 on usage / open errors.
 *
 * Run it with the antenna port terminated (50 ohm) for the ADC noise checks.
 * The bias tee is never switched on by this tool.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <time.h>
#else
#include <windows.h>
#include "getopt/getopt.h"
#endif

#include "rtl-sdr.h"
#include "convenience/convenience.h"

enum status { ST_PASS, ST_WARN, ST_FAIL, ST_SKIP };
static const char *status_name[] = { "PASS", "WARN", "FAIL", "SKIP" };

static int counts[4];
static FILE *csv;
static int verbose;

static rtlsdr_dev_t *dev;
static double stream_seconds = 3.0;
static int quick;
static int no_timing;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static double now_s(void)
{
#ifdef _WIN32
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)f.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static void report(enum status st, const char *name, const char *fmt, ...)
{
	char detail[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);

	counts[st]++;
	printf("[%s] %-28s %s\n", status_name[st], name, detail);
	if (csv)
		fprintf(csv, "%s,%s,\"%s\"\n", name, status_name[st], detail);
	fflush(stdout);
}

static const char *tuner_name(enum rtlsdr_tuner t)
{
	switch (t) {
	case RTLSDR_TUNER_E4000:  return "Elonics E4000";
	case RTLSDR_TUNER_FC0012: return "Fitipower FC0012";
	case RTLSDR_TUNER_FC0013: return "Fitipower FC0013";
	case RTLSDR_TUNER_FC2580: return "FCI FC2580";
	case RTLSDR_TUNER_R820T:  return "Rafael Micro R820T";
	case RTLSDR_TUNER_R828D:  return "Rafael Micro R828D";
	default:                  return "unknown";
	}
}

/* nominal ranges every unit of that tuner must cover (Hz) */
struct range { uint32_t lo, hi; };

static int nominal_ranges(enum rtlsdr_tuner t, struct range *r)
{
	switch (t) {
	case RTLSDR_TUNER_R820T:
	case RTLSDR_TUNER_R828D:
		r[0].lo = 25000000;  r[0].hi = 1700000000; return 1;
	case RTLSDR_TUNER_E4000:
		r[0].lo = 60000000;  r[0].hi = 1000000000;
		r[1].lo = 1300000000; r[1].hi = 1850000000; return 2;
	case RTLSDR_TUNER_FC0012:
		r[0].lo = 25000000;  r[0].hi = 900000000; return 1;
	case RTLSDR_TUNER_FC0013:
		r[0].lo = 25000000;  r[0].hi = 1000000000; return 1;
	default:
		return 0;
	}
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* stream byte-counter checker (RTL2832 test mode: every byte is previous + 1) */
struct counter_state {
	int init;
	uint8_t next;
	uint64_t bytes;
	uint64_t lost;
};

static void counter_feed(struct counter_state *c, const unsigned char *buf, uint32_t len)
{
	uint32_t i;

	if (!len)
		return;
	if (!c->init) {
		c->next = buf[0];
		c->init = 1;
	}
	for (i = 0; i < len; i++) {
		if (buf[i] != c->next) {
			int d = (int)buf[i] - (int)c->next;
			c->lost += (uint64_t)(d < 0 ? d + 256 : d);
			c->next = buf[i];
		}
		c->next++;
	}
	c->bytes += len;
}

/* ------------------------------------------------------------------ */
/* checks                                                             */
/* ------------------------------------------------------------------ */

static void check_identity(uint32_t index)
{
	char m[256], p[256], s[256];
	uint8_t ee[256];
	int r;

	m[0] = p[0] = s[0] = 0;
	r = rtlsdr_get_device_usb_strings(index, m, p, s);
	if (r == 0)
		report(ST_PASS, "identity", "%s / %s / SN %s, tuner %s",
		       m, p, s, tuner_name(rtlsdr_get_tuner_type(dev)));
	else
		report(ST_FAIL, "identity", "cannot read USB strings (%d)", r);

	if (rtlsdr_get_tuner_type(dev) == RTLSDR_TUNER_UNKNOWN)
		report(ST_FAIL, "tuner", "no supported tuner found");

	memset(ee, 0, sizeof(ee));
	r = rtlsdr_read_eeprom(dev, ee, 0, 256);
	if (r == -3)
		report(ST_SKIP, "eeprom", "no EEPROM present");
	else if (r != 0)
		report(ST_FAIL, "eeprom", "read failed (%d, documented: 0 on success)", r);
	else if (ee[0] != 0x28 || ee[1] != 0x32)
		report(ST_WARN, "eeprom", "readable but header is %02x %02x (expected 28 32)", ee[0], ee[1]);
	else
		report(ST_PASS, "eeprom", "256 bytes read, header ok, VID:PID %02x%02x:%02x%02x",
		       ee[3], ee[2], ee[5], ee[4]);
}

static void check_contract(void)
{
	uint8_t b[4];
	int bad = 0;

	/* every one of these is documented / required to fail */
	if (rtlsdr_set_sample_rate(dev, 100000) >= 0) bad++;
	if (rtlsdr_set_sample_rate(dev, 500000) >= 0) bad++;
	if (rtlsdr_set_sample_rate(dev, 3300000) >= 0) bad++;
	if (rtlsdr_set_freq_correction(dev, 1000000) >= 0) bad++;
	if (rtlsdr_set_freq_correction(dev, -1000000) >= 0) bad++;
	if (rtlsdr_set_direct_sampling(dev, 3) >= 0) bad++;
	if (rtlsdr_set_bias_tee_gpio(dev, 9, 0) >= 0) bad++;
	if (rtlsdr_read_eeprom(dev, b, 253, 4) != -2) bad++;

	if (bad)
		report(ST_FAIL, "api-contract", "%d invalid-argument call(s) were accepted", bad);
	else
		report(ST_PASS, "api-contract", "invalid rates / ppm / modes / pins / eeprom ranges rejected");

	/* leave the device in a sane state */
	rtlsdr_set_freq_correction(dev, 0);
	rtlsdr_set_sample_rate(dev, 2048000);
}

static void check_sample_rates(void)
{
	static const uint32_t rates[] = { 250000, 300000, 960000, 1024000, 1440000,
					  1800000, 1920000, 2048000, 2400000, 2560000,
					  2880000, 3200000 };
	unsigned i;
	int fail = 0, warn = 0;
	double worst = 0;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		double err_ppm;
		uint32_t got;

		if (rtlsdr_set_sample_rate(dev, rates[i]) != 0) {
			report(ST_FAIL, "sample-rate", "%u S/s was rejected", rates[i]);
			fail++;
			continue;
		}
		got = rtlsdr_get_sample_rate(dev);
		err_ppm = fabs((double)got - (double)rates[i]) / rates[i] * 1e6;
		if (err_ppm > worst)
			worst = err_ppm;
		if (verbose)
			printf("       %u -> %u (%.3f ppm)\n", rates[i], got, err_ppm);
		if (err_ppm > 50.0) {
			report(ST_FAIL, "sample-rate", "%u S/s reads back %u (%.1f ppm off)", rates[i], got, err_ppm);
			fail++;
		} else if (err_ppm > 5.0) {
			warn++;
		}
	}
	if (!fail)
		report(warn ? ST_WARN : ST_PASS, "sample-rate",
		       "%u rates programmed, worst quantisation error %.3f ppm",
		       (unsigned)(sizeof(rates) / sizeof(rates[0])), worst);
	rtlsdr_set_sample_rate(dev, 2048000);
}

static void check_gains(void)
{
	int gains[128], n, i, bad = 0;
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);

	n = rtlsdr_get_tuner_gains(dev, NULL);
	if (n <= 0 || n > 128) {
		report(ST_FAIL, "gain-table", "bad gain count %d", n);
		return;
	}
	rtlsdr_get_tuner_gains(dev, gains);
	for (i = 1; i < n; i++)
		if (gains[i] < gains[i - 1])
			bad++;
	if (t == RTLSDR_TUNER_FC2580 || t == RTLSDR_TUNER_UNKNOWN) {
		report(ST_SKIP, "gain-table", "tuner has no software gain control");
		return;
	}
	if (bad) {
		report(ST_FAIL, "gain-table", "%d entries are not monotonic", bad);
		return;
	}

	rtlsdr_set_tuner_gain_mode(dev, 1);
	bad = 0;
	for (i = 0; i < n; i++) {
		if (rtlsdr_set_tuner_gain(dev, gains[i]) != 0 ||
		    rtlsdr_get_tuner_gain(dev) != gains[i]) {
			if (verbose)
				printf("       gain %.1f dB not accepted\n", gains[i] / 10.0);
			bad++;
		}
	}
	rtlsdr_set_tuner_gain_mode(dev, 0);
	if (bad)
		report(ST_FAIL, "gain-table", "%d of %d gains rejected / not read back", bad, n);
	else
		report(ST_PASS, "gain-table", "%d gains %.1f..%.1f dB, monotonic, all accepted",
		       n, gains[0] / 10.0, gains[n - 1] / 10.0);
}

static void check_tuning(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	struct range nom[2];
	int nn = nominal_ranges(t, nom), i, holes = 0, missed = 0, total = 0;
	uint32_t f, step = quick ? 25000000u : 5000000u;
	double lat[2048], t0;
	int nlat = 0;
	uint32_t first_ok = 0, last_ok = 0, gap_start = 0;
	int in_gap = 0;

	rtlsdr_set_sample_rate(dev, 2048000);
	for (f = 20000000; f <= 2300000000u; f += step) {
		int ok, expected = 0;

		t0 = now_s();
		ok = (rtlsdr_set_center_freq(dev, f) == 0);
		if (nlat < 2048 && ok)
			lat[nlat++] = (now_s() - t0) * 1e3;
		if (ok && rtlsdr_get_center_freq(dev) != f)
			ok = 0;	/* claims success but does not read back */
		total++;
		if (ok) {
			if (!first_ok)
				first_ok = f;
			last_ok = f;
			if (in_gap) {
				in_gap = 0;
				if (verbose)
					printf("       gap %.0f..%.0f MHz\n", gap_start / 1e6, (f - step) / 1e6);
				holes++;
			}
		} else if (first_ok && !in_gap) {
			in_gap = 1;
			gap_start = f;
		}
		for (i = 0; i < nn; i++)
			if (f >= nom[i].lo && f <= nom[i].hi)
				expected = 1;
		if (expected && !ok)
			missed++;
	}

	if (!first_ok) {
		report(ST_FAIL, "tuning-range", "no frequency could be set");
		return;
	}
	if (nn == 0)
		report(ST_SKIP, "tuning-range", "tuned %.0f..%.0f MHz, no nominal range known for this tuner",
		       first_ok / 1e6, last_ok / 1e6);
	else if (missed)
		report(ST_FAIL, "tuning-range", "%d of %d nominal test points failed (tuned %.0f..%.0f MHz, %d gap(s))",
		       missed, total, first_ok / 1e6, last_ok / 1e6, holes);
	else
		report(ST_PASS, "tuning-range", "all nominal points tuned; range %.0f..%.0f MHz, %d gap(s) outside nominal",
		       first_ok / 1e6, last_ok / 1e6, holes);

	if (nlat > 4) {
		qsort(lat, nlat, sizeof(double), cmp_double);
		if (no_timing)
			report(ST_SKIP, "retune-latency", "timing checks disabled");
		else
			report(lat[nlat / 2] > 50.0 ? ST_WARN : ST_PASS, "retune-latency",
			       "median %.2f ms, p95 %.2f ms, max %.2f ms over %d retunes",
			       lat[nlat / 2], lat[(nlat * 95) / 100], lat[nlat - 1], nlat);
	}
}

static void check_pll_stress(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	struct range nom[2];
	int nn = nominal_ranges(t, nom), i, fails = 0, n = quick ? 100 : 500;
	uint32_t seed = 12345;

	if (nn == 0) {
		report(ST_SKIP, "pll-stress", "no nominal range for this tuner");
		return;
	}
	for (i = 0; i < n; i++) {
		struct range *r = &nom[i % nn];
		uint32_t f;

		seed = seed * 1664525u + 1013904223u;
		f = r->lo + (uint32_t)(((uint64_t)seed * (r->hi - r->lo)) >> 32);
		if (rtlsdr_set_center_freq(dev, f) != 0 || rtlsdr_get_center_freq(dev) != f) {
			if (verbose)
				printf("       retune to %u Hz failed\n", f);
			fails++;
		}
	}
	report(fails ? ST_FAIL : ST_PASS, "pll-stress", "%d random retunes, %d failed", n, fails);
}

static void check_adc(void)
{
	static unsigned char buf[262144];
	int n = 0, r;
	double sum[2] = { 0, 0 }, sq[2] = { 0, 0 };
	uint32_t hist[256];
	uint64_t clip = 0, cnt = 0;
	unsigned i, distinct = 0;
	double dc[2], sd[2], ratio;

	rtlsdr_set_sample_rate(dev, 2048000);
	rtlsdr_set_center_freq(dev, 433920000);
	rtlsdr_set_tuner_gain_mode(dev, 0);
	rtlsdr_reset_buffer(dev);
	memset(hist, 0, sizeof(hist));

	for (i = 0; i < 4; i++) {	/* first blocks may contain start-up junk */
		r = rtlsdr_read_sync(dev, buf, sizeof(buf), &n);
		if (r < 0 || n <= 0) {
			report(ST_FAIL, "adc", "sync read failed (%d)", r);
			return;
		}
	}
	for (i = 0; i < (unsigned)n; i++) {
		int ch = i & 1;
		double v = buf[i];

		sum[ch] += v;
		sq[ch] += v * v;
		hist[buf[i]]++;
		if (buf[i] == 0 || buf[i] == 255)
			clip++;
		cnt++;
	}
	for (i = 0; i < 256; i++)
		if (hist[i])
			distinct++;
	for (i = 0; i < 2; i++) {
		double m = sum[i] / (cnt / 2.0);

		dc[i] = m;
		sd[i] = sqrt(sq[i] / (cnt / 2.0) - m * m);
	}
	ratio = sd[1] > 0 ? sd[0] / sd[1] : 0;

	if (sd[0] < 0.4 || sd[1] < 0.4)
		report(ST_FAIL, "adc-noise", "I/Q sigma %.2f / %.2f LSB: ADC output is flat", sd[0], sd[1]);
	else
		report(sd[0] > 40 || sd[1] > 40 ? ST_WARN : ST_PASS, "adc-noise",
		       "sigma I %.2f Q %.2f LSB, %u distinct codes", sd[0], sd[1], distinct);
	report(fabs(dc[0] - 127.5) > 8 || fabs(dc[1] - 127.5) > 8 ? ST_WARN : ST_PASS, "adc-dc-offset",
	       "mean I %.2f Q %.2f (ideal 127.5)", dc[0], dc[1]);
	report(ratio < 0.85 || ratio > 1.15 ? ST_WARN : ST_PASS, "adc-iq-balance",
	       "sigma I/Q = %.3f", ratio);
	report(clip * 100 > cnt ? ST_WARN : ST_PASS, "adc-clipping",
	       "%.3f %% of samples at 0/255 (antenna port terminated?)", 100.0 * clip / cnt);
}

/* --- streaming ---------------------------------------------------- */

struct stream {
	struct counter_state c;
	double t_first, t_last, t_stop, cancel_at;
	uint64_t bytes_after_first;
	int blocks;
};

static void stream_cb(unsigned char *buf, uint32_t len, void *ctx)
{
	struct stream *s = ctx;
	double t = now_s();

	if (s->t_stop > 0)
		return;
	counter_feed(&s->c, buf, len);
	if (s->blocks == 0)
		s->t_first = t;
	else
		s->bytes_after_first += len;
	s->t_last = t;
	s->blocks++;
	if (t - s->t_first >= s->cancel_at) {
		s->t_stop = t;
		rtlsdr_cancel_async(dev);
	}
}

static void check_stream_at(uint32_t rate, int strict)
{
	struct stream s;
	double t_start, t_end, span, mb, ppm, msps;
	char name[32];
	int r;

	snprintf(name, sizeof(name), "stream-%uk", rate / 1000);
	memset(&s, 0, sizeof(s));
	s.cancel_at = stream_seconds;

	if (rtlsdr_set_sample_rate(dev, rate) != 0) {
		report(ST_FAIL, name, "rate rejected");
		return;
	}
	rtlsdr_set_center_freq(dev, 100000000);
	rtlsdr_set_testmode(dev, 1);
	rtlsdr_reset_buffer(dev);

	t_start = now_s();
	r = rtlsdr_read_async(dev, stream_cb, &s, 0, 0);
	t_end = now_s();
	rtlsdr_set_testmode(dev, 0);

	if (r != 0 || s.blocks < 2) {
		report(ST_FAIL, name, "async read returned %d after %d block(s)", r, s.blocks);
		return;
	}
	span = s.t_last - s.t_first;
	mb = s.c.bytes / 1e6;
	msps = (s.bytes_after_first / 2.0) / span / 1e6;
	ppm = ((s.bytes_after_first / 2.0) / span / rate - 1.0) * 1e6;

	if (s.c.lost) {
		report(strict ? ST_FAIL : ST_WARN, name,
		       "%.1f MB, %.3f MS/s, LOST %llu bytes (%.4f %%): USB bandwidth / host too slow",
		       mb, msps, (unsigned long long)s.c.lost, 100.0 * s.c.lost / s.c.bytes);
	} else if (no_timing) {
		report(ST_PASS, name, "%.1f MB received, no lost bytes (timing disabled)", mb);
	} else {
		report(fabs(ppm) > 1000 ? ST_WARN : ST_PASS, name,
		       "%.1f MB, %.4f MS/s (%+.0f ppm vs host clock), no lost bytes", mb, msps, ppm);
	}
	(void)t_start;
	(void)t_end;
}

static void check_streaming(void)
{
	check_stream_at(1024000, 1);
	check_stream_at(2048000, 1);
	if (!quick) {
		check_stream_at(2400000, 1);
		check_stream_at(3200000, 0);	/* often too much for shared USB2 hubs */
	}
	rtlsdr_set_sample_rate(dev, 2048000);
}

static void cancel_cb(unsigned char *buf, uint32_t len, void *ctx)
{
	double *t = ctx;

	if (*t == 0)
		*t = now_s();
	if (*t > 0 && now_s() - *t > 0.2)
		rtlsdr_cancel_async(dev);
}

static void check_cancel(void)
{
	double t_cb = 0, t0, dt;
	int r;

	rtlsdr_set_sample_rate(dev, 2048000);
	rtlsdr_reset_buffer(dev);
	r = rtlsdr_read_async(dev, cancel_cb, &t_cb, 0, 0);
	t0 = now_s();
	dt = (t0 - t_cb - 0.2) * 1e3;
	if (r != 0)
		report(ST_FAIL, "async-cancel", "read_async returned %d", r);
	else if (no_timing)
		report(ST_PASS, "async-cancel", "stream started and stopped cleanly");
	else
		report(dt > 500 ? ST_WARN : ST_PASS, "async-cancel", "cancel took %.1f ms", dt < 0 ? 0 : dt);
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
	fprintf(stderr,
		"rtl_validate, on-device validation and benchmark for RTL2832U receivers\n\n"
		"Usage:\trtl_validate [-d device_index_or_serial] [options]\n"
		"\t[-t seconds per streaming test (default: 3)]\n"
		"\t[-q quick mode: coarser sweeps, fewer stream rates]\n"
		"\t[-T skip host-clock timing checks (for emulators / busy hosts)]\n"
		"\t[-c file  write results as CSV]\n"
		"\t[-v verbose]\n\n"
		"Terminate the antenna input for the ADC checks. The bias tee is never enabled.\n"
		"Exit status: 0 no failure, 1 failed check, 2 usage or device error.\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int opt, dev_index = 0, dev_given = 0, r;
	const char *csv_name = NULL;

	while ((opt = getopt(argc, argv, "d:t:qTc:vh")) != -1) {
		switch (opt) {
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 't':
			stream_seconds = atof(optarg);
			if (stream_seconds < 0.05 || stream_seconds > 600)
				usage();
			break;
		case 'q':
			quick = 1;
			break;
		case 'T':
			no_timing = 1;
			break;
		case 'c':
			csv_name = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}
	if (!dev_given)
		dev_index = verbose_device_search("0");
	if (dev_index < 0)
		return 2;

	if (csv_name) {
		csv = fopen(csv_name, "w");
		if (!csv) {
			fprintf(stderr, "Cannot write %s\n", csv_name);
			return 2;
		}
		fprintf(csv, "check,status,detail\n");
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		return 2;
	}

	printf("rtl_validate %s%s\n\n", quick ? "(quick) " : "", no_timing ? "(no timing) " : "");
	check_identity((uint32_t)dev_index);
	check_contract();
	check_sample_rates();
	check_gains();
	check_tuning();
	check_pll_stress();
	check_adc();
	check_streaming();
	check_cancel();

	rtlsdr_close(dev);

	printf("\n%d passed, %d warnings, %d failed, %d skipped\n",
	       counts[ST_PASS], counts[ST_WARN], counts[ST_FAIL], counts[ST_SKIP]);
	if (csv)
		fclose(csv);
	return counts[ST_FAIL] ? 1 : 0;
}
