/*
 * rtl_validate, an on-device validation and benchmark tool for RTL2832U
 * based receivers.
 *
 * Exercises every public function of librtlsdr, its argument contract and
 * the hardware behind it, and reports PASS / WARN / FAIL / SKIP per check.
 * Exit status: 0 no failure, 1 at least one failure, 2 usage / device error.
 *
 *   api      enumeration, xtal, ppm, bandwidth, gain modes, IF gain, test mode,
 *            AGC, direct sampling, offset tuning, sync / async parameters,
 *            re-entrancy, open/close cycles, exclusive open, EEPROM
 *   rf       sample rates, gain table, gain vs noise, tuning range, retune
 *            latency, PLL stress, ADC statistics, spur scan
 *   stream   throughput, lost bytes (RTL2832 test-mode counter), sample-clock
 *            error, cancel latency
 *   ref      with a reference carrier (-r): frequency error in ppm, spectrum
 *            sign, effect and direction of the ppm correction
 *   opt      opt-in tests that change state: bias tee (-B), EEPROM write (-E)
 *   self     self-test of the measurement code (FFT peak estimator)
 *
 * Run with the antenna port terminated (50 ohm) for the noise checks, or with
 * a known carrier connected and -r <Hz> for the reference tests.
 * The bias tee is never switched on unless -B is given.
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum status { ST_PASS, ST_WARN, ST_FAIL, ST_SKIP };
static const char *status_name[] = { "PASS", "WARN", "FAIL", "SKIP" };

static int counts[4];
static FILE *csv;
static int verbose;

static rtlsdr_dev_t *dev;
static uint32_t dev_index;
static double stream_seconds = 3.0;
static int quick;
static int no_timing;
static double ref_hz;		/* -r: reference carrier */
static int opt_biast, opt_eeprom_write;

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
	printf("[%s] %-26s %s\n", status_name[st], name, detail);
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

static int tuner_is_r82xx(enum rtlsdr_tuner t)
{
	return t == RTLSDR_TUNER_R820T || t == RTLSDR_TUNER_R828D;
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

/* --- FFT / spectral estimation ------------------------------------ */

static void fft(double *re, double *im, int n)
{
	int i, j, k, m;

	for (i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			double t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (m = 2; m <= n; m <<= 1) {
		double ang = -2 * M_PI / m;
		double wr0 = cos(ang), wi0 = sin(ang);
		for (k = 0; k < n; k += m) {
			double wr = 1, wi = 0;
			for (i = 0; i < m / 2; i++) {
				double xr = re[k+i+m/2] * wr - im[k+i+m/2] * wi;
				double xi = re[k+i+m/2] * wi + im[k+i+m/2] * wr;
				re[k+i+m/2] = re[k+i] - xr; im[k+i+m/2] = im[k+i] - xi;
				re[k+i] += xr; im[k+i] += xi;
				{
					double t = wr * wr0 - wi * wi0;
					wi = wr * wi0 + wi * wr0;
					wr = t;
				}
			}
		}
	}
}

#define NFFT 65536

static double spec_re[NFFT], spec_im[NFFT], spec_p[NFFT];

/* power spectrum of n = NFFT complex samples (Hann window), bins in FFT order */
static void power_spectrum(const double *i_, const double *q_)
{
	int k;

	for (k = 0; k < NFFT; k++) {
		double w = 0.5 - 0.5 * cos(2 * M_PI * k / (NFFT - 1));
		spec_re[k] = i_[k] * w;
		spec_im[k] = q_[k] * w;
	}
	fft(spec_re, spec_im, NFFT);
	for (k = 0; k < NFFT; k++)
		spec_p[k] = spec_re[k] * spec_re[k] + spec_im[k] * spec_im[k];
}

/* strongest peak within |f| <= maxhz (bins excluded around DC: guard), parabolic interpolation.
 * Returns the frequency in Hz relative to the tuned centre. */
static double peak_freq(double fs, double maxhz, int dc_guard, double *peak_db, double *median_db)
{
	int k, best = -1;
	double bestp = -1, a, b, c, delta, tmp[NFFT];
	int kmax = (int)(maxhz / fs * NFFT);

	for (k = -kmax; k <= kmax; k++) {
		int idx = (k + NFFT) % NFFT;

		if (abs(k) <= dc_guard)
			continue;
		if (spec_p[idx] > bestp) {
			bestp = spec_p[idx];
			best = k;
		}
	}
	if (best == -999999)
		return 0;
	a = log(spec_p[(best - 1 + NFFT) % NFFT] + 1e-9);
	b = log(spec_p[(best + NFFT) % NFFT] + 1e-9);
	c = log(spec_p[(best + 1 + NFFT) % NFFT] + 1e-9);
	delta = 0.5 * (a - c) / (a - 2 * b + c);
	if (!(fabs(delta) < 1.0))
		delta = 0;
	if (peak_db)
		*peak_db = 10 * log10(bestp + 1e-9);
	if (median_db) {
		memcpy(tmp, spec_p, sizeof(tmp));
		qsort(tmp, NFFT, sizeof(double), cmp_double);
		*median_db = 10 * log10(tmp[NFFT / 2] + 1e-9);
	}
	return (best + delta) * fs / NFFT;
}

/* read NFFT complex samples: 8-bit unsigned -> centred doubles */
static int capture(double *i_, double *q_)
{
	static unsigned char buf[2 * NFFT];
	int n = 0, r, got = 0, blocks = 0;

	rtlsdr_reset_buffer(dev);
	for (blocks = 0; blocks < 3 && got < 2 * NFFT + 8192 * 0; blocks++) {
		/* flush start-up junk */
		r = rtlsdr_read_sync(dev, buf, 16384, &n);
		if (r < 0)
			return r;
	}
	got = 0;
	while (got < 2 * NFFT) {
		r = rtlsdr_read_sync(dev, buf + got, (2 * NFFT - got) > 16384 ? 16384 : (2 * NFFT - got), &n);
		if (r < 0 || n <= 0)
			return r < 0 ? r : -1;
		got += n;
	}
	for (n = 0; n < NFFT; n++) {
		i_[n] = buf[2 * n] - 127.5;
		q_[n] = buf[2 * n + 1] - 127.5;
	}
	return 0;
}

static double cap_i[NFFT], cap_q[NFFT];

/* ------------------------------------------------------------------ */
/* api checks                                                         */
/* ------------------------------------------------------------------ */

static void check_identity(void)
{
	char m[256], p[256], s[256];
	int r;

	m[0] = p[0] = s[0] = 0;
	r = rtlsdr_get_device_usb_strings(dev_index, m, p, s);
	if (r == 0)
		report(ST_PASS, "identity", "%s / %s / SN %s, tuner %s",
		       m, p, s, tuner_name(rtlsdr_get_tuner_type(dev)));
	else
		report(ST_FAIL, "identity", "cannot read USB strings (%d)", r);

	if (rtlsdr_get_tuner_type(dev) == RTLSDR_TUNER_UNKNOWN)
		report(ST_FAIL, "tuner", "no supported tuner found");
}

static void check_enumeration(void)
{
	uint32_t n = rtlsdr_get_device_count(), i;
	char m[256], p[256], s[256], s2[256];
	int bad = 0, dup = 0, empty_serial = 0;

	if (n < 1 || dev_index >= n) {
		report(ST_FAIL, "api-enumeration", "device count %u, but device %u is open", n, dev_index);
		return;
	}
	for (i = 0; i < n; i++) {
		m[0] = p[0] = s[0] = 0;
		if (rtlsdr_get_device_usb_strings(i, m, p, s) != 0 || !strlen(rtlsdr_get_device_name(i)))
			bad++;
		if (!s[0])
			empty_serial++;
		else if (rtlsdr_get_index_by_serial(s) != (int)i) {
			/* another dongle with the same serial comes first */
			if (rtlsdr_get_index_by_serial(s) >= 0)
				dup++;
			else
				bad++;
		}
	}
	/* an index that does not exist must not look like a real device */
	m[0] = 0;
	if (rtlsdr_get_device_usb_strings(n + 5, m, p, s2) == 0) bad++;
	if (strlen(rtlsdr_get_device_name(n + 5)) != 0) bad++;
	if (rtlsdr_get_index_by_serial(NULL) >= 0) bad++;
	if (rtlsdr_get_index_by_serial("no-such-serial-\x01") >= 0) bad++;

	if (bad)
		report(ST_FAIL, "api-enumeration", "%u device(s), %d inconsistent result(s)", n, bad);
	else if (dup || empty_serial)
		report(ST_WARN, "api-enumeration", "%u device(s); %d duplicate and %d empty serial number(s): "
		       "selecting by serial is ambiguous (set unique ones with rtl_eeprom -s)", n, dup, empty_serial);
	else
		report(ST_PASS, "api-enumeration", "%u device(s), names / USB strings / serial lookup consistent", n);
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
	if (rtlsdr_set_direct_sampling(dev, -1) >= 0) bad++;
	if (rtlsdr_set_bias_tee_gpio(dev, 9, 0) >= 0) bad++;
	if (rtlsdr_set_bias_tee_gpio(dev, -1, 0) >= 0) bad++;
	if (rtlsdr_read_eeprom(dev, b, 253, 4) != -2) bad++;
	if (rtlsdr_read_eeprom(dev, NULL, 0, 4) != -1) bad++;
	if (rtlsdr_set_center_freq(NULL, 100000000) >= 0) bad++;
	if (rtlsdr_get_center_freq(NULL) != 0) bad++;
	if (rtlsdr_get_sample_rate(NULL) != 0) bad++;
	if (rtlsdr_read_async(NULL, NULL, NULL, 0, 0) >= 0) bad++;
	if (rtlsdr_cancel_async(NULL) >= 0) bad++;

	if (bad)
		report(ST_FAIL, "api-contract", "%d invalid-argument call(s) were accepted", bad);
	else
		report(ST_PASS, "api-contract", "invalid rates / ppm / modes / pins / ranges / NULL handles rejected");

	/* leave the device in a sane state */
	rtlsdr_set_freq_correction(dev, 0);
	rtlsdr_set_sample_rate(dev, 2048000);
}

static void check_eeprom(void)
{
	uint8_t ee[256], ee2[256];
	int r, i, diff = 0;

	memset(ee, 0, sizeof(ee));
	r = rtlsdr_read_eeprom(dev, ee, 0, 256);
	if (r == -3) {
		report(ST_SKIP, "eeprom", "no EEPROM present");
		return;
	}
	if (r != 0) {
		report(ST_FAIL, "eeprom", "read failed (%d, documented: 0 on success)", r);
		return;
	}
	if (ee[0] != 0x28 || ee[1] != 0x32)
		report(ST_WARN, "eeprom", "readable but header is %02x %02x (expected 28 32)", ee[0], ee[1]);
	else
		report(ST_PASS, "eeprom", "256 bytes read, header ok, VID:PID %02x%02x:%02x%02x",
		       ee[3], ee[2], ee[5], ee[4]);

	/* two reads must agree (catches a flaky I2C bus) */
	r = rtlsdr_read_eeprom(dev, ee2, 0, 256);
	for (i = 0; i < 256; i++)
		if (ee[i] != ee2[i])
			diff++;
	report(r == 0 && !diff ? ST_PASS : ST_FAIL, "eeprom-stable", "second read %s (%d byte(s) differ)",
	       r == 0 ? "done" : "failed", diff);

	/* partial reads agree with the full read */
	memset(ee2, 0, sizeof(ee2));
	r = rtlsdr_read_eeprom(dev, ee2, 16, 32);
	report(r == 0 && !memcmp(ee2, ee + 16, 32) ? ST_PASS : ST_FAIL, "eeprom-offset", "offset read of 32 bytes %s",
	       r == 0 && !memcmp(ee2, ee + 16, 32) ? "matches" : "differs");

	/* writing the same data changes nothing (the driver only writes differing bytes) */
	r = rtlsdr_write_eeprom(dev, ee, 0, 256);
	memset(ee2, 0, sizeof(ee2));
	rtlsdr_read_eeprom(dev, ee2, 0, 256);
	report(r == 0 && !memcmp(ee, ee2, 256) ? ST_PASS : ST_FAIL, "eeprom-rewrite",
	       "writing back the current contents returned %d, contents %s", r,
	       !memcmp(ee, ee2, 256) ? "unchanged" : "CHANGED");
}

static void check_xtal(void)
{
	uint32_t r0 = 0, t0 = 0, r1 = 0, t1 = 0;
	int bad = 0;

	if (rtlsdr_get_xtal_freq(dev, &r0, &t0) != 0) {
		report(ST_FAIL, "api-xtal", "rtlsdr_get_xtal_freq failed");
		return;
	}
	if (r0 < 28799000 || r0 > 28801000)
		bad++;
	if (t0 < 8000000 || t0 > 60000000)
		bad++;

	/* +-200 Hz on the RTL crystal is legal and must read back */
	if (rtlsdr_set_xtal_freq(dev, r0 + 200, 0) != 0) bad++;
	rtlsdr_get_xtal_freq(dev, &r1, &t1);
	if (r1 != r0 + 200) bad++;
	/* outside the +-1 kHz window / absurd tuner crystal must be refused and change nothing */
	if (rtlsdr_set_xtal_freq(dev, r0 + 5000, 0) >= 0) bad++;
	if (rtlsdr_set_xtal_freq(dev, 0, 1) >= 0) bad++;
	if (rtlsdr_set_xtal_freq(dev, 0, 200000000) >= 0) bad++;
	rtlsdr_get_xtal_freq(dev, &r1, NULL);
	if (r1 != r0 + 200) bad++;
	/* sample rate and tuning still work with the odd crystal */
	if (rtlsdr_set_sample_rate(dev, 2048000) != 0) bad++;
	if (rtlsdr_set_center_freq(dev, 100000000) != 0) bad++;
	/* restore */
	if (rtlsdr_set_xtal_freq(dev, r0, 0) != 0) bad++;
	rtlsdr_get_xtal_freq(dev, &r1, &t1);
	if (r1 != r0 || t1 != t0) bad++;

	report(bad ? ST_FAIL : ST_PASS, "api-xtal", "RTL %u Hz, tuner %u Hz; set/get, limits and restore %s",
	       r0, t0, bad ? "FAILED" : "ok");
}

static void check_freq_correction(void)
{
	int bad = 0, r;

	if (rtlsdr_get_freq_correction(dev) != 0)
		bad++;
	if (rtlsdr_set_freq_correction(dev, 25) != 0 || rtlsdr_get_freq_correction(dev) != 25) bad++;
	r = rtlsdr_set_freq_correction(dev, 25);	/* unchanged: -2 by convention, 0 also fine */
	if (r != 0 && r != -2) bad++;
	if (rtlsdr_set_freq_correction(dev, -25) != 0 || rtlsdr_get_freq_correction(dev) != -25) bad++;
	/* beyond the +-488 ppm sample-clock register: accepted, must not break streaming */
	if (rtlsdr_set_freq_correction(dev, 700) != 0 || rtlsdr_get_freq_correction(dev) != 700) bad++;
	if (rtlsdr_set_center_freq(dev, 100000000) != 0) bad++;
	if (rtlsdr_set_freq_correction(dev, 0) != 0 || rtlsdr_get_freq_correction(dev) != 0) bad++;
	report(bad ? ST_FAIL : ST_PASS, "api-freq-correction", "set/get +-25 ppm, 700 ppm, unchanged, reset: %s",
	       bad ? "FAILED" : "ok");
}

static void check_bandwidth(void)
{
	int bad = 0;
	static const uint32_t bw[] = { 0, 200000, 1000000, 1500000, 2400000, 6000000, 8000000 };
	unsigned i;

	for (i = 0; i < sizeof(bw) / sizeof(bw[0]); i++)
		if (rtlsdr_set_tuner_bandwidth(dev, bw[i]) != 0) {
			if (verbose)
				printf("       bandwidth %u rejected\n", bw[i]);
			bad++;
		}
	if (rtlsdr_set_tuner_bandwidth(dev, 0x80000000u) >= 0) bad++;
	rtlsdr_set_tuner_bandwidth(dev, 0);
	rtlsdr_set_sample_rate(dev, 2048000);
	report(bad ? ST_FAIL : ST_PASS, "api-bandwidth", "%u bandwidths incl. auto accepted, invalid refused: %s",
	       (unsigned)(sizeof(bw) / sizeof(bw[0])), bad ? "FAILED" : "ok");
}

static void check_gain_modes(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	int bad = 0, n, gains[128], i;

	if (rtlsdr_set_tuner_gain_mode(dev, 1) != 0) bad++;
	if (rtlsdr_set_tuner_gain_mode(dev, 0) != 0) bad++;
	if (rtlsdr_set_tuner_gain_mode(dev, 1) != 0) bad++;
	n = rtlsdr_get_tuner_gains(dev, gains);
	if (n > 0 && n <= 128 && t != RTLSDR_TUNER_FC2580) {
		/* out-of-table requests are snapped, never an error, never crash */
		if (rtlsdr_set_tuner_gain(dev, gains[0] - 100) < 0) bad++;
		if (rtlsdr_set_tuner_gain(dev, gains[n - 1] + 100) < 0) bad++;
		if (rtlsdr_set_tuner_gain(dev, gains[n / 2]) != 0 || rtlsdr_get_tuner_gain(dev) != gains[n / 2]) bad++;
	}
	if (t == RTLSDR_TUNER_E4000) {
		/* every IF stage with every legal value, dB * 10 */
		static const int v1[] = { -3, 6 }, v23[] = { 0, 3, 6, 9 }, v4[] = { 0, 1, 2 }, v56[] = { 3, 6, 9, 12, 15 };
		for (i = 0; i < 2; i++) if (rtlsdr_set_tuner_if_gain(dev, 1, v1[i] * 10) != 0) bad++;
		for (i = 0; i < 4; i++) if (rtlsdr_set_tuner_if_gain(dev, 2, v23[i] * 10) != 0 ||
					    rtlsdr_set_tuner_if_gain(dev, 3, v23[i] * 10) != 0) bad++;
		for (i = 0; i < 3; i++) if (rtlsdr_set_tuner_if_gain(dev, 4, v4[i] * 10) != 0) bad++;
		for (i = 0; i < 5; i++) if (rtlsdr_set_tuner_if_gain(dev, 5, v56[i] * 10) != 0 ||
					    rtlsdr_set_tuner_if_gain(dev, 6, v56[i] * 10) != 0) bad++;
		if (rtlsdr_set_tuner_if_gain(dev, 7, 0) == 0) bad++;	/* no such stage */
		if (rtlsdr_set_tuner_if_gain(dev, 1, 50) == 0) bad++;	/* no such value */
	} else {
		rtlsdr_set_tuner_if_gain(dev, 1, 0);		/* no IF gain stages: must simply not crash */
	}
	rtlsdr_set_tuner_gain_mode(dev, 0);
	report(bad ? ST_FAIL : ST_PASS, "api-gain-modes", "auto/manual switching, out-of-range snapping%s: %s",
	       t == RTLSDR_TUNER_E4000 ? ", all 6 IF stages" : "", bad ? "FAILED" : "ok");
}

static void check_testmode_agc(void)
{
	int bad = 0;

	if (rtlsdr_set_testmode(dev, 1) != 0) bad++;
	if (rtlsdr_set_testmode(dev, 0) != 0) bad++;
	if (rtlsdr_set_agc_mode(dev, 1) != 0) bad++;
	if (rtlsdr_set_agc_mode(dev, 0) != 0) bad++;
	if (rtlsdr_set_testmode(NULL, 1) >= 0) bad++;
	if (rtlsdr_set_agc_mode(NULL, 1) >= 0) bad++;
	report(bad ? ST_FAIL : ST_PASS, "api-testmode-agc", "test mode and digital AGC switch on/off: %s", bad ? "FAILED" : "ok");
}

static void check_direct_sampling(void)
{
	int mode, bad = 0;
	int n = 0;
	unsigned char buf[16384];

	for (mode = 0; mode <= 2; mode++) {
		if (rtlsdr_set_direct_sampling(dev, mode) != 0 || rtlsdr_get_direct_sampling(dev) != mode) {
			if (verbose)
				printf("       direct sampling mode %d failed\n", mode);
			bad++;
		}
		/* direct mode takes the ADC input as-is; the normal path needs a tunable frequency */
		if (rtlsdr_set_center_freq(dev, mode ? 7100000 : 100000000) != 0) bad++;
		rtlsdr_reset_buffer(dev);
		if (rtlsdr_read_sync(dev, buf, sizeof(buf), &n) < 0 || n != (int)sizeof(buf)) bad++;
	}
	/* back to normal: the tuner must be usable again */
	if (rtlsdr_set_direct_sampling(dev, 0) != 0 || rtlsdr_get_direct_sampling(dev) != 0) bad++;
	if (rtlsdr_set_center_freq(dev, 100000000) != 0) bad++;
	rtlsdr_reset_buffer(dev);
	if (rtlsdr_read_sync(dev, buf, sizeof(buf), &n) < 0 || n != (int)sizeof(buf)) bad++;
	report(bad ? ST_FAIL : ST_PASS, "api-direct-sampling", "modes 0/1/2 set, read back, stream, and tuner restored: %s",
	       bad ? "FAILED" : "ok");
}

static void check_offset_tuning(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	int r, bad = 0;

	r = rtlsdr_set_offset_tuning(dev, 1);
	if (tuner_is_r82xx(t)) {
		/* documented: not supported on R820T/R828D */
		if (r == 0) bad++;
		if (rtlsdr_get_offset_tuning(dev) != 0) bad++;
		report(bad ? ST_FAIL : ST_PASS, "api-offset-tuning", "R82xx has none and says so (%d)", r);
		return;
	}
	if (t == RTLSDR_TUNER_FC2580 || t == RTLSDR_TUNER_UNKNOWN) {
		report(ST_SKIP, "api-offset-tuning", "tuner has no offset tuning");
		return;
	}
	if (r != 0 || rtlsdr_get_offset_tuning(dev) != 1) bad++;
	/* with offset tuning the LO sits above the centre: tuning below it must fail, not wrap */
	if (rtlsdr_set_center_freq(dev, 1000000) == 0) bad++;
	if (rtlsdr_set_center_freq(dev, 100000000) != 0) bad++;
	/* direct sampling and offset tuning exclude each other */
	if (rtlsdr_set_direct_sampling(dev, 1) == 0 && rtlsdr_set_offset_tuning(dev, 1) == 0) bad++;
	rtlsdr_set_direct_sampling(dev, 0);
	if (rtlsdr_set_offset_tuning(dev, 0) != 0 || rtlsdr_get_offset_tuning(dev) != 0) bad++;
	rtlsdr_set_center_freq(dev, 100000000);
	report(bad ? ST_FAIL : ST_PASS, "api-offset-tuning", "on/off, read back, no wrap-around, exclusive with direct sampling: %s",
	       bad ? "FAILED" : "ok");
}

static void check_read_sync(void)
{
	static unsigned char buf[262144];
	static const int sizes[] = { 512, 4096, 16384, 65536, 262144 };
	unsigned i;
	int bad = 0, short_reads = 0, n = 0;
	struct counter_state c;

	rtlsdr_set_sample_rate(dev, 2048000);
	rtlsdr_set_testmode(dev, 1);
	rtlsdr_reset_buffer(dev);
	memset(&c, 0, sizeof(c));
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (rtlsdr_read_sync(dev, buf, sizes[i], &n) != 0) {
			bad++;
			continue;
		}
		if (n != sizes[i])
			short_reads++;
		counter_feed(&c, buf, n > 0 ? n : 0);
	}
	rtlsdr_set_testmode(dev, 0);
	if (bad)
		report(ST_FAIL, "api-read-sync", "%d read(s) failed", bad);
	else if (c.lost)
		report(ST_FAIL, "api-read-sync", "%llu byte(s) lost across %u reads", (unsigned long long)c.lost,
		       (unsigned)(sizeof(sizes) / sizeof(sizes[0])));
	else
		report(short_reads ? ST_WARN : ST_PASS, "api-read-sync", "sizes 512 B .. 256 KiB: %d short read(s), no lost bytes",
		       short_reads);
}

/* --- async parameter / re-entrancy --------------------------------- */

struct acb {
	struct counter_state c;
	uint32_t want_len;
	int blocks, len_mismatch, limit;
	int reentry_rc, cancel_before_rc;
	int do_reentry;
};

static void acb(unsigned char *buf, uint32_t len, void *ctx)
{
	struct acb *a = ctx;

	counter_feed(&a->c, buf, len);
	if (a->want_len && len != a->want_len)
		a->len_mismatch++;
	if (a->do_reentry && a->blocks == 0)
		a->reentry_rc = rtlsdr_read_async(dev, acb, a, 0, 0);	/* already running: must refuse */
	a->blocks++;
	if (a->blocks >= a->limit)
		rtlsdr_cancel_async(dev);
}

static void check_async_params(void)
{
	static const struct { uint32_t num, len; } cfg[] = {
		{ 0, 0 }, { 4, 16384 }, { 1, 512 }, { 8, 65536 }, { 15, 262144 }, { 3, 1000 }
	};
	unsigned i;
	int bad = 0, r;

	rtlsdr_set_sample_rate(dev, 2048000);
	for (i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
		struct acb a;

		memset(&a, 0, sizeof(a));
		a.limit = 24;
		/* a length that is not a multiple of 512 falls back to the default: no fixed expectation */
		a.want_len = (cfg[i].len && cfg[i].len % 512 == 0) ? cfg[i].len : 0;
		rtlsdr_set_testmode(dev, 1);
		rtlsdr_reset_buffer(dev);
		r = rtlsdr_read_async(dev, acb, &a, cfg[i].num, cfg[i].len);
		rtlsdr_set_testmode(dev, 0);
		if (r != 0 || a.blocks < a.limit || a.c.lost || a.len_mismatch) {
			if (verbose)
				printf("       bufs %u len %u: rc %d, blocks %d, lost %llu, wrong-length %d\n",
				       cfg[i].num, cfg[i].len, r, a.blocks, (unsigned long long)a.c.lost, a.len_mismatch);
			bad++;
		}
	}
	report(bad ? ST_FAIL : ST_PASS, "api-async-params", "%u buffer count / length combinations stream cleanly, lengths honoured: %s",
	       (unsigned)(sizeof(cfg) / sizeof(cfg[0])), bad ? "FAILED" : "ok");
}

static void check_async_reentrancy(void)
{
	struct acb a;
	int bad = 0, r;

	/* cancel with nothing running */
	if (rtlsdr_cancel_async(dev) == 0) bad++;

	/* starting a second async read from inside the callback must be refused */
	memset(&a, 0, sizeof(a));
	a.limit = 4;
	a.do_reentry = 1;
	a.reentry_rc = 12345;
	rtlsdr_reset_buffer(dev);
	r = rtlsdr_read_async(dev, acb, &a, 0, 0);
	if (r != 0 || a.reentry_rc >= 0) bad++;
	/* and cancel after it ended is again a no-op that says so */
	if (rtlsdr_cancel_async(dev) == 0) bad++;

	/* rtlsdr_wait_async is the same thing with default buffers */
	memset(&a, 0, sizeof(a));
	a.limit = 4;
	rtlsdr_reset_buffer(dev);
	if (rtlsdr_wait_async(dev, acb, &a) != 0 || a.blocks < 4) bad++;

	/* the stream can be restarted any number of times */
	for (r = 0; r < 5; r++) {
		memset(&a, 0, sizeof(a));
		a.limit = 3;
		rtlsdr_reset_buffer(dev);
		if (rtlsdr_read_async(dev, acb, &a, 0, 0) != 0 || a.blocks < 3) {
			bad++;
			break;
		}
	}
	report(bad ? ST_FAIL : ST_PASS, "api-async-reentrancy", "cancel when idle, refusal of nested start, wait_async, 5 restarts: %s",
	       bad ? "FAILED" : "ok");
}

static void check_open_close(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	rtlsdr_dev_t *second = NULL;
	int bad = 0, i, r;
	double t0, worst = 0;
	unsigned char buf[16384];
	int n = 0;

	/* the device is claimed by us: a second open has to fail */
	r = rtlsdr_open(&second, dev_index);
	if (r == 0) {
		report(ST_WARN, "api-double-open", "a second rtlsdr_open() of the same dongle succeeded "
		       "(two programs can fight over one device)");
		rtlsdr_close(second);
	} else {
		report(ST_PASS, "api-double-open", "second open refused (%d)", r);
	}

	for (i = 0; i < 10; i++) {
		t0 = now_s();
		rtlsdr_close(dev);
		dev = NULL;
		if (rtlsdr_open(&dev, dev_index) != 0) {
			bad++;
			break;
		}
		if (now_s() - t0 > worst)
			worst = now_s() - t0;
		if (rtlsdr_get_tuner_type(dev) != t) bad++;
		if (rtlsdr_set_sample_rate(dev, 2048000) != 0) bad++;
		if (rtlsdr_set_center_freq(dev, 100000000) != 0) bad++;
		rtlsdr_reset_buffer(dev);
		if (rtlsdr_read_sync(dev, buf, sizeof(buf), &n) < 0 || n != (int)sizeof(buf)) bad++;
	}
	if (!dev) {
		report(ST_FAIL, "api-open-close", "could not reopen the dongle, remaining checks need a device");
		exit(2);
	}
	report(bad ? ST_FAIL : (!no_timing && worst > 3.0 ? ST_WARN : ST_PASS), "api-open-close",
	       "10 close/open/tune/stream cycles%s: %s (slowest %.2f s)", bad ? "" : ", tuner identical each time",
	       bad ? "FAILED" : "ok", worst);
}

/* ------------------------------------------------------------------ */
/* rf checks                                                          */
/* ------------------------------------------------------------------ */

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
	/* a rejected request must not change the running rate */
	rtlsdr_set_sample_rate(dev, 1024000);
	if (rtlsdr_set_sample_rate(dev, 100000) >= 0 || rtlsdr_get_sample_rate(dev) != 1024000) {
		report(ST_FAIL, "sample-rate", "a rejected rate changed the current rate");
		fail++;
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

/* sigma of I and Q over a capture, DC removed */
static void adc_stats(double *sd_i, double *sd_q, double *dc_i, double *dc_q, double *clip_pct, unsigned *distinct)
{
	static unsigned char buf[262144];
	uint32_t hist[256];
	double s[2] = { 0, 0 }, q2[2] = { 0, 0 };
	uint64_t clip = 0, cnt = 0;
	int n = 0, r;
	unsigned i, k;

	memset(hist, 0, sizeof(hist));
	rtlsdr_reset_buffer(dev);
	for (k = 0; k < 4; k++) {	/* first blocks may contain start-up junk */
		r = rtlsdr_read_sync(dev, buf, sizeof(buf), &n);
		if (r < 0 || n <= 0) {
			*sd_i = *sd_q = 0;
			return;
		}
	}
	for (i = 0; i < (unsigned)n; i++) {
		int ch = i & 1;
		double v = buf[i];

		s[ch] += v;
		q2[ch] += v * v;
		hist[buf[i]]++;
		if (buf[i] == 0 || buf[i] == 255)
			clip++;
		cnt++;
	}
	for (i = 0, *distinct = 0; i < 256; i++)
		if (hist[i])
			(*distinct)++;
	*dc_i = s[0] / (cnt / 2.0);
	*dc_q = s[1] / (cnt / 2.0);
	*sd_i = sqrt(q2[0] / (cnt / 2.0) - *dc_i * *dc_i);
	*sd_q = sqrt(q2[1] / (cnt / 2.0) - *dc_q * *dc_q);
	*clip_pct = 100.0 * clip / cnt;
}

static void check_adc(void)
{
	static const uint32_t freqs[] = { 100000000, 433920000, 1090000000 };
	unsigned f, distinct;
	int dead = 0, dc_warn = 0, bal_warn = 0, clip_warn = 0, noisy = 0;
	double worst_dc = 0, worst_bal = 1, worst_clip = 0, sdmax = 0, sdmin = 1e9;
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	struct range nom[2];
	int nn = nominal_ranges(t, nom);

	rtlsdr_set_sample_rate(dev, 2048000);
	rtlsdr_set_tuner_gain_mode(dev, 0);
	for (f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
		double sdi, sdq, dci, dcq, clip, bal;
		int ok = (nn == 0);
		int i;

		for (i = 0; i < nn; i++)
			if (freqs[f] >= nom[i].lo && freqs[f] <= nom[i].hi)
				ok = 1;
		if (!ok)
			continue;
		if (rtlsdr_set_center_freq(dev, freqs[f]) != 0)
			continue;
		adc_stats(&sdi, &sdq, &dci, &dcq, &clip, &distinct);
		if (sdi < 0.4 || sdq < 0.4)
			dead++;
		if (sdi > 40 || sdq > 40)
			noisy++;
		if (fabs(dci - 127.5) > 8 || fabs(dcq - 127.5) > 8)
			dc_warn++;
		if (fabs(dci - 127.5) > worst_dc) worst_dc = fabs(dci - 127.5);
		if (fabs(dcq - 127.5) > worst_dc) worst_dc = fabs(dcq - 127.5);
		bal = sdq > 0 ? sdi / sdq : 0;
		if (bal < 0.85 || bal > 1.15)
			bal_warn++;
		if (fabs(log(bal + 1e-9)) > fabs(log(worst_bal))) worst_bal = bal;
		if (clip > 1.0)
			clip_warn++;
		if (clip > worst_clip) worst_clip = clip;
		if (sdi > sdmax) sdmax = sdi;
		if (sdi < sdmin) sdmin = sdi;
		if (verbose)
			printf("       %.3f MHz: sigma %.2f/%.2f, DC %.2f/%.2f, clip %.3f %%, %u codes\n",
			       freqs[f] / 1e6, sdi, sdq, dci, dcq, clip, distinct);
	}
	if (sdmin > 1e8) {
		report(ST_SKIP, "adc", "no test frequency is inside this tuner's range");
		return;
	}
	report(dead ? ST_FAIL : (noisy ? ST_WARN : ST_PASS), "adc-noise",
	       "sigma %.2f .. %.2f LSB over 3 bands%s", sdmin, sdmax,
	       dead ? ": ADC output is flat (dead input?)" : (noisy ? ": very noisy, strong signal present?" : ""));
	report(dc_warn ? ST_WARN : ST_PASS, "adc-dc-offset", "worst |mean - 127.5| = %.2f LSB", worst_dc);
	report(bal_warn ? ST_WARN : ST_PASS, "adc-iq-balance", "worst sigma I/Q = %.3f", worst_bal);
	report(clip_warn ? ST_WARN : ST_PASS, "adc-clipping", "worst %.3f %% of samples at 0/255 (antenna port terminated?)", worst_clip);
}

static void check_gain_noise(void)
{
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	int gains[128], n;
	double sd_lo, sd_hi, sd_mid, tmp, dci, dcq, clip;
	unsigned distinct;

	n = rtlsdr_get_tuner_gains(dev, gains);
	if (t == RTLSDR_TUNER_FC2580 || t == RTLSDR_TUNER_UNKNOWN || n < 3 || n > 128) {
		report(ST_SKIP, "gain-noise", "no gain control to test");
		return;
	}
	rtlsdr_set_sample_rate(dev, 2048000);
	if (rtlsdr_set_center_freq(dev, 433920000) != 0) {
		report(ST_SKIP, "gain-noise", "cannot tune the test frequency");
		return;
	}
	rtlsdr_set_tuner_gain_mode(dev, 1);
	rtlsdr_set_tuner_gain(dev, gains[0]);
	adc_stats(&sd_lo, &tmp, &dci, &dcq, &clip, &distinct);
	rtlsdr_set_tuner_gain(dev, gains[n / 2]);
	adc_stats(&sd_mid, &tmp, &dci, &dcq, &clip, &distinct);
	rtlsdr_set_tuner_gain(dev, gains[n - 1]);
	adc_stats(&sd_hi, &tmp, &dci, &dcq, &clip, &distinct);
	rtlsdr_set_tuner_gain_mode(dev, 0);

	if (sd_lo < 0.3)
		report(ST_FAIL, "gain-noise", "sigma %.2f LSB at minimum gain: dead ADC", sd_lo);
	else if (sd_hi < sd_lo * 1.15)
		report(ST_WARN, "gain-noise", "noise does not rise with gain: %.2f (%.1f dB) -> %.2f (%.1f dB) LSB: "
		       "gain control ineffective or input not terminated", sd_lo, gains[0] / 10.0, sd_hi, gains[n - 1] / 10.0);
	else
		report(sd_mid < sd_lo * 0.95 || sd_hi < sd_mid * 0.95 ? ST_WARN : ST_PASS, "gain-noise",
		       "sigma %.2f (%.1f dB) -> %.2f (%.1f dB) -> %.2f (%.1f dB) LSB, rises with gain",
		       sd_lo, gains[0] / 10.0, sd_mid, gains[n / 2] / 10.0, sd_hi, gains[n - 1] / 10.0);
}

static void check_spurs(void)
{
	static const uint32_t freqs[] = { 100000000, 433920000, 868000000 };
	enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);
	struct range nom[2];
	int nn = nominal_ranges(t, nom), i, f, worst_fail = 0, tested = 0;
	double worst_spur = -999, worst_dc = -999, worst_f = 0, pk, med;

	rtlsdr_set_sample_rate(dev, 2048000);
	rtlsdr_set_tuner_gain_mode(dev, 0);
	for (f = 0; f < 3; f++) {
		int ok = (nn == 0);

		for (i = 0; i < nn; i++)
			if (freqs[f] >= nom[i].lo && freqs[f] <= nom[i].hi)
				ok = 1;
		if (!ok || rtlsdr_set_center_freq(dev, freqs[f]) != 0)
			continue;
		if (capture(cap_i, cap_q) != 0) {
			worst_fail = 1;
			continue;
		}
		power_spectrum(cap_i, cap_q);
		{
			double fr = peak_freq(2048000.0, 900000.0, 6, &pk, &med);
			double dc = 10 * log10(spec_p[0] + spec_p[1] + spec_p[NFFT - 1] + 1e-9);

			if (pk - med > worst_spur) { worst_spur = pk - med; worst_f = freqs[f] + fr; }
			if (dc - med > worst_dc) worst_dc = dc - med;
			tested++;
		}
	}
	if (worst_fail || !tested) {
		report(tested ? ST_FAIL : ST_SKIP, "spur-scan", tested ? "capture failed" : "no test frequency inside the tuner range");
		return;
	}
	/* Hann window, 64k points: a clean noise floor peaks ~15 dB over the median */
	report(worst_spur > 30 ? ST_WARN : ST_PASS, "spur-scan",
	       "strongest line %.1f dB over the noise median at %.3f MHz, DC spike %.1f dB over median",
	       worst_spur, worst_f / 1e6, worst_dc);
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

/* ------------------------------------------------------------------ */
/* streaming                                                          */
/* ------------------------------------------------------------------ */

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
	double span, mb, ppm, msps;
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

	r = rtlsdr_read_async(dev, stream_cb, &s, 0, 0);
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
}

static void check_streaming(void)
{
	static const uint32_t all[] = { 250000, 960000, 1024000, 1440000, 1800000, 2048000, 2400000, 2560000, 2880000, 3200000 };
	unsigned i;

	if (quick) {
		check_stream_at(1024000, 1);
		check_stream_at(2048000, 1);
	} else {
		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
			check_stream_at(all[i], all[i] <= 2400000);	/* >2.4 MS/s often too much for shared USB2 hubs */
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
/* reference carrier (-r)                                             */
/* ------------------------------------------------------------------ */

/* tune to `center`, capture, return the carrier's offset from the centre in Hz */
static int measure_carrier(uint32_t center, double fs, double *offset, double *snr_db)
{
	double pk, med;

	if (rtlsdr_set_center_freq(dev, center) != 0)
		return -1;
	if (capture(cap_i, cap_q) != 0)
		return -2;
	power_spectrum(cap_i, cap_q);
	*offset = peak_freq(fs, fs * 0.45, 8, &pk, &med);
	*snr_db = pk - med;
	return 0;
}

static void check_reference(void)
{
	const double fs = 1024000.0;
	const double want = 100000.0;		/* carrier 100 kHz above the centre */
	double off0, off1, off2, snr, err_hz, ppm, shift, expect;
	uint32_t center = (uint32_t)(ref_hz - want);
	int bad = 0;

	if (ref_hz <= 0) {
		report(ST_SKIP, "ref-frequency", "no reference carrier given (-r <Hz>)");
		return;
	}
	rtlsdr_set_freq_correction(dev, 0);
	rtlsdr_set_sample_rate(dev, (uint32_t)fs);
	rtlsdr_set_tuner_gain_mode(dev, 0);

	if (measure_carrier(center, fs, &off0, &snr) != 0) {
		report(ST_FAIL, "ref-frequency", "cannot tune %.6f MHz / capture", center / 1e6);
		return;
	}
	if (snr < 25) {
		report(ST_FAIL, "ref-frequency", "no carrier found near %.6f MHz (strongest line only %.1f dB over noise)",
		       ref_hz / 1e6, snr);
		return;
	}
	err_hz = off0 - want;
	ppm = -err_hz / ref_hz * 1e6;	/* carrier too high in the baseband = LO too low = crystal slow */
	report(fabs(ppm) <= 25 ? ST_PASS : (fabs(ppm) <= 100 ? ST_WARN : ST_FAIL), "ref-frequency",
	       "carrier at %+.1f Hz from the expected place = %+.1f ppm, %.0f dB over noise; "
	       "suggested correction: -p %+.0f", err_hz, ppm, snr, -ppm);

	/* spectrum sign: moving the centre 50 kHz up must move the carrier 50 kHz down */
	if (measure_carrier(center + 50000, fs, &off1, &snr) != 0) {
		report(ST_FAIL, "ref-sign", "retune failed");
		return;
	}
	shift = off1 - off0;
	report(fabs(shift + 50000) < 500 ? ST_PASS : ST_FAIL, "ref-sign",
	       "centre +50 kHz moved the carrier by %+.1f Hz (expected -50000): %s", shift,
	       fabs(shift + 50000) < 500 ? "spectrum orientation correct" : "spectrum inverted or tuning step wrong");

	/* ppm correction: +p ppm must move the carrier up in the baseband by ref * p ppm */
	rtlsdr_set_freq_correction(dev, 20);
	if (measure_carrier(center, fs, &off2, &snr) != 0) {
		report(ST_FAIL, "ref-ppm-effect", "retune failed");
		rtlsdr_set_freq_correction(dev, 0);
		return;
	}
	rtlsdr_set_freq_correction(dev, 0);
	shift = off2 - off0;
	expect = ref_hz * 20e-6;
	if (shift <= 0 || shift < 0.7 * expect || shift > 1.3 * expect)
		bad = 1;
	report(bad ? ST_FAIL : ST_PASS, "ref-ppm-effect",
	       "+20 ppm correction moved the carrier by %+.1f Hz (expected %+.1f): %s", shift, expect,
	       bad ? (shift <= 0 ? "wrong direction or no effect" : "wrong size") : "direction and size correct");

	/* and the correction that the first measurement suggested should centre the carrier */
	{
		int p = (int)lround(-ppm);

		if (p != 0 && abs(p) < 400) {
			double off3;

			rtlsdr_set_freq_correction(dev, p);
			if (measure_carrier(center, fs, &off3, &snr) == 0) {
				double res = (off3 - want) / ref_hz * 1e6;

				report(fabs(res) <= 5 ? ST_PASS : ST_WARN, "ref-ppm-applied",
				       "after -p %d the carrier is %+.1f Hz (%+.1f ppm) from the expected place", p, off3 - want, res);
			}
			rtlsdr_set_freq_correction(dev, 0);
		}
	}
}

/* ------------------------------------------------------------------ */
/* opt-in tests that change state                                     */
/* ------------------------------------------------------------------ */

static void check_biast(void)
{
	int bad = 0;

	if (!opt_biast) {
		report(ST_SKIP, "biast-toggle", "not enabled (-B): would switch the antenna supply");
		return;
	}
	if (rtlsdr_set_bias_tee(dev, 1) != 0) bad++;
	if (rtlsdr_set_bias_tee(dev, 0) != 0) bad++;
	if (rtlsdr_set_bias_tee_gpio(dev, 0, 1) != 0) bad++;
	if (rtlsdr_set_bias_tee_gpio(dev, 0, 0) != 0) bad++;
	report(bad ? ST_FAIL : ST_PASS, "biast-toggle", "GPIO 0 switched on and off again (check with a meter): %s", bad ? "FAILED" : "ok");
}

static void check_eeprom_write(void)
{
	uint8_t save[16], pat[16], back[16];
	int i, bad = 0, r;

	if (!opt_eeprom_write) {
		report(ST_SKIP, "eeprom-write", "not enabled (-E): writes 16 unused bytes at offset 240 and restores them");
		return;
	}
	if (rtlsdr_read_eeprom(dev, save, 240, 16) != 0) {
		report(ST_SKIP, "eeprom-write", "no EEPROM");
		return;
	}
	for (i = 0; i < 16; i++)
		pat[i] = (uint8_t)(~save[i]);
	r = rtlsdr_write_eeprom(dev, pat, 240, 16);
	if (r != 0) bad++;
	if (rtlsdr_read_eeprom(dev, back, 240, 16) != 0 || memcmp(back, pat, 16)) bad++;
	r = rtlsdr_write_eeprom(dev, save, 240, 16);
	if (r != 0) bad++;
	if (rtlsdr_read_eeprom(dev, back, 240, 16) != 0 || memcmp(back, save, 16)) bad++;
	report(bad ? ST_FAIL : ST_PASS, "eeprom-write", "inverted 16 bytes written, read back, original restored: %s", bad ? "FAILED" : "ok");
}

/* ------------------------------------------------------------------ */
/* self-test of the measurement code                                  */
/* ------------------------------------------------------------------ */

static void check_selftest(void)
{
	static double a[NFFT], b[NFFT];
	const double fs = 2048000.0;
	double tests[] = { 123456.7, -98765.4, 250000.0, -400000.25 };
	unsigned t;
	int bad = 0;
	uint32_t seed = 7;

	for (t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
		int n;
		double off, pk, med;

		for (n = 0; n < NFFT; n++) {
			double ph = 2 * M_PI * tests[t] * n / fs;
			double nz1, nz2;

			seed = seed * 1664525u + 1013904223u;
			nz1 = ((seed >> 8) & 0xffff) / 65536.0 - 0.5;
			seed = seed * 1664525u + 1013904223u;
			nz2 = ((seed >> 8) & 0xffff) / 65536.0 - 0.5;
			a[n] = 50 * cos(ph) + nz1 * 4;
			b[n] = 50 * sin(ph) + nz2 * 4;
		}
		power_spectrum(a, b);
		off = peak_freq(fs, fs * 0.45, 8, &pk, &med);
		if (fabs(off - tests[t]) > 1.0)
			bad++;
		if (verbose)
			printf("       tone %.2f Hz -> %.2f Hz (%.1f dB over median)\n", tests[t], off, pk - med);
	}
	report(bad ? ST_FAIL : ST_PASS, "self-fft", "peak estimator within 1 Hz on %u synthetic tones: %s",
	       (unsigned)(sizeof(tests) / sizeof(tests[0])), bad ? "FAILED (measurement code is wrong)" : "ok");
}

/* ------------------------------------------------------------------ */

struct check {
	const char *group;
	const char *name;
	void (*fn)(void);
	int needs_device;
};

static const struct check checks[] = {
	{ "self",   "self-fft",          check_selftest,          0 },
	{ "api",    "identity",          check_identity,          1 },
	{ "api",    "api-enumeration",   check_enumeration,       1 },
	{ "api",    "api-contract",      check_contract,          1 },
	{ "api",    "eeprom",            check_eeprom,            1 },
	{ "api",    "api-xtal",          check_xtal,              1 },
	{ "api",    "api-freq-correction", check_freq_correction, 1 },
	{ "api",    "api-bandwidth",     check_bandwidth,         1 },
	{ "api",    "api-gain-modes",    check_gain_modes,        1 },
	{ "api",    "api-testmode-agc",  check_testmode_agc,      1 },
	{ "api",    "api-direct-sampling", check_direct_sampling, 1 },
	{ "api",    "api-offset-tuning", check_offset_tuning,     1 },
	{ "api",    "api-read-sync",     check_read_sync,         1 },
	{ "api",    "api-async-params",  check_async_params,      1 },
	{ "api",    "api-async-reentrancy", check_async_reentrancy, 1 },
	{ "api",    "api-open-close",    check_open_close,        1 },
	{ "rf",     "sample-rate",       check_sample_rates,      1 },
	{ "rf",     "gain-table",        check_gains,             1 },
	{ "rf",     "gain-noise",        check_gain_noise,        1 },
	{ "rf",     "tuning-range",      check_tuning,            1 },
	{ "rf",     "pll-stress",        check_pll_stress,        1 },
	{ "rf",     "adc",               check_adc,               1 },
	{ "rf",     "spur-scan",         check_spurs,             1 },
	{ "stream", "streaming",         check_streaming,         1 },
	{ "stream", "async-cancel",      check_cancel,            1 },
	{ "ref",    "ref-frequency",     check_reference,         1 },
	{ "opt",    "biast-toggle",      check_biast,             1 },
	{ "opt",    "eeprom-write",      check_eeprom_write,      1 },
};

static int selected(const struct check *c, const char *filter)
{
	char buf[256], *tok;

	if (!filter)
		return 1;
	strncpy(buf, filter, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
		if (!strncmp(c->group, tok, strlen(tok)) || !strncmp(c->name, tok, strlen(tok)))
			return 1;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"rtl_validate, on-device validation and benchmark for RTL2832U receivers\n\n"
		"Usage:\trtl_validate [-d device_index_or_serial] [options]\n"
		"\t[-k groups_or_checks  run only these, comma separated (groups: self api rf stream ref opt)]\n"
		"\t[-l list the checks and exit]\n"
		"\t[-t seconds per streaming test (default: 3)]\n"
		"\t[-q quick mode: coarser sweeps, fewer stream rates]\n"
		"\t[-T skip host-clock timing checks (for emulators / busy hosts)]\n"
		"\t[-r reference carrier in Hz: frequency error, spectrum sign, ppm correction checks]\n"
		"\t[-B also test switching the bias tee GPIO (antenna supply!)]\n"
		"\t[-E also test writing 16 unused EEPROM bytes (restored afterwards)]\n"
		"\t[-c file  write results as CSV]\n"
		"\t[-v verbose]\n\n"
		"Terminate the antenna input for the noise checks. The bias tee is never enabled without -B.\n"
		"Exit status: 0 no failure, 1 failed check, 2 usage or device error.\n");
	exit(2);
}

int main(int argc, char **argv)
{
	int opt, dev_given = 0, r, di = 0;
	const char *csv_name = NULL, *filter = NULL;
	unsigned i;
	int need_dev = 0, list = 0;

	while ((opt = getopt(argc, argv, "d:t:qTc:vhk:lr:BE")) != -1) {
		switch (opt) {
		case 'd':
			di = verbose_device_search(optarg);
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
		case 'k':
			filter = optarg;
			break;
		case 'l':
			list = 1;
			break;
		case 'r':
			ref_hz = atofs(optarg);
			break;
		case 'B':
			opt_biast = 1;
			break;
		case 'E':
			opt_eeprom_write = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}

	if (list) {
		for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
			printf("%-8s %s\n", checks[i].group, checks[i].name);
		return 0;
	}

	for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
		if (selected(&checks[i], filter) && checks[i].needs_device)
			need_dev = 1;

	if (need_dev) {
		if (!dev_given)
			di = verbose_device_search("0");
		if (di < 0)
			return 2;
		dev_index = (uint32_t)di;
	}

	if (csv_name) {
		csv = fopen(csv_name, "w");
		if (!csv) {
			fprintf(stderr, "Cannot write %s\n", csv_name);
			return 2;
		}
		fprintf(csv, "check,status,detail\n");
	}

	if (need_dev) {
		r = rtlsdr_open(&dev, dev_index);
		if (r < 0) {
			fprintf(stderr, "Failed to open rtlsdr device #%u.\n", dev_index);
			return 2;
		}
	}

	printf("rtl_validate %s%s%s\n\n", quick ? "(quick) " : "", no_timing ? "(no timing) " : "",
	       ref_hz > 0 ? "(reference carrier) " : "");
	for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
		if (selected(&checks[i], filter))
			checks[i].fn();

	if (dev)
		rtlsdr_close(dev);

	printf("\n%d passed, %d warnings, %d failed, %d skipped\n",
	       counts[ST_PASS], counts[ST_WARN], counts[ST_FAIL], counts[ST_SKIP]);
	if (csv)
		fclose(csv);
	return counts[ST_FAIL] ? 1 : 0;
}
