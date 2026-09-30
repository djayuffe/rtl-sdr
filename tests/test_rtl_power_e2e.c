/*
 * End-to-end test of the rtl_power scan path: a tone at a known baseband
 * frequency goes through librtlsdr, the scanner, the FFT and the CSV writer,
 * and the frequency reported for the strongest bin is compared with the tone.
 * This checks bin ordering, the sign convention and the CSV frequency axis.
 */
#define main rtl_power_main
#include "../src/rtl_power.c"
#undef main
#include "fake_libusb.h"

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

struct result {
	double low, high, step, samples;
	int nvals;
	double peak_hz, peak_db, median_db;
};

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* one hop scan of `spec` with a tone at rf_hz; fills r */
static int run_scan(const char *spec, double crop, double rf_hz, struct result *r)
{
	static char text[1 << 20];
	static double vals[70000], sorted[70000];
	char arg[128];
	char *p, *end;
	int i, n = 0, best = 0, length;
	size_t got;

	memset(r, 0, sizeof(*r));
	fake_reset();
	strncpy(arg, spec, sizeof(arg) - 1);
	arg[sizeof(arg) - 1] = 0;
	tune_count = 0;
	frequency_range(arg, crop);
	if (tune_count != 1)
		return -1;

	/* the tone is given in RF; the dongle is tuned to tunes[0].freq */
	fake_set_signal(1, rf_hz - tunes[0].freq, 0, 0, 0, 100);	/* strong: clipped the old int16 path */

	if (rtlsdr_open(&dev, 0) != 0)
		return -2;
	rtlsdr_set_tuner_gain_mode(dev, 0);
	rtlsdr_reset_buffer(dev);
	rtlsdr_set_sample_rate(dev, (uint32_t)tunes[0].rate);

	free(Sinewave); free(power_table);
	sine_table(tunes[0].bin_e);
	free(fft_buf); free(window_coefs);
	fft_buf = malloc(tunes[0].buf_len * sizeof(int16_t));
	length = 1 << tunes[0].bin_e;
	window_coefs = malloc(length * sizeof(int));
	for (i = 0; i < length; i++)
		window_coefs[i] = (int)(256 * rectangle(i, length));

	file = tmpfile();
	for (i = 0; i < 4; i++)
		scanner();
	csv_dbm(&tunes[0]);
	fflush(file);
	rewind(file);
	got = fread(text, 1, sizeof(text) - 1, file);
	text[got] = 0;
	fclose(file);
	rtlsdr_close(dev);

	/* "low, high, step, samples, v, v, ..." */
	p = text;
	r->low = strtod(p, &end); p = end + 1;
	r->high = strtod(p, &end); p = end + 1;
	r->step = strtod(p, &end); p = end + 1;
	r->samples = strtod(p, &end); p = end + 1;
	while (*p && n < 70000) {
		double v = strtod(p, &end);
		if (end == p)
			break;
		vals[n++] = v;
		p = end;
		while (*p == ',' || *p == ' ' || *p == '\n')
			p++;
	}
	r->nvals = n;
	if (n < 4)
		return -3;
	for (i = 0; i < n; i++) {
		sorted[i] = vals[i];
		if (vals[i] > vals[best])
			best = i;
	}
	qsort(sorted, n, sizeof(double), cmp_d);
	r->peak_db = vals[best];
	r->median_db = sorted[n / 2];
	/* the CSV convention used by heatmap.py: bin j is at low + j * step */
	r->peak_hz = r->low + best * r->step;
	return 0;
}

static void expect_peak(const char *spec, double crop, double rf_hz)
{
	struct result r;
	int rc = run_scan(spec, crop, rf_hz, &r);

	if (rc) {
		fprintf(stderr, "FAIL scan '%s' tone %.0f: rc %d\n", spec, rf_hz, rc);
		failures++;
		return;
	}
	printf("  %-16s crop %.1f tone %.3f MHz -> peak %.3f MHz (err %+.0f Hz, step %.0f Hz), %.1f dB over median, %d values\n",
	       spec, crop, rf_hz / 1e6, r.peak_hz / 1e6, r.peak_hz - rf_hz, r.step, r.peak_db - r.median_db, r.nvals);
	CHECK(fabs(r.peak_hz - rf_hz) <= r.step);
	CHECK(r.peak_db - r.median_db > 25.0);
	CHECK(r.high > r.low);
}

int main(void)
{
	printf("rtl_power end-to-end frequency axis:\n");
	expect_peak("99M:101M:10k", 0.0, 100300000.0);	/* above the centre */
	expect_peak("99M:101M:10k", 0.0, 99550000.0);	/* below the centre */
	expect_peak("99M:101M:10k", 0.2, 100300000.0);	/* cropped */
	expect_peak("99M:101M:10k", 0.2, 99550000.0);
	expect_peak("100M:100.4M:1k", 0.0, 100120000.0);	/* decimated (boxcar) path */
	expect_peak("100M:100.4M:1k", 0.0, 100050000.0);	/* -150 kHz from the centre */
	expect_peak("100M:100.4M:1k", 0.1, 100050000.0);	/* even decimation factor */
	expect_peak("100M:100.4M:1k", 0.2, 100350000.0);
	expect_peak("100M:100.4M:1k", 0.3, 100050000.0);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_rtl_power_e2e: all checks passed\n");
	return 0;
}
