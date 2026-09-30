/* Unit tests for the rtl_fm DSP helpers (the tool source is included directly). */
#define main rtl_fm_main
#include "../src/rtl_fm.c"
#undef main

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int ref_angle(int y, int x)	/* pi == 1<<14 */
{
	return (int)(atan2((double)y, (double)x) / M_PI * (1<<14));
}

static void test_atan(void)
{
	long long k;
	int i;
	int ys[] = { 1, 5, 100, 1000, -3, -777, 4095, 32767 };
	int xs[] = { 100, -100, 3, -2000, 12345, -30000, 1, 7 };

	atan_lut_init();
	for (k = 1; k <= (1<<19); k <<= 3) {
		for (i = 0; i < 8; i++) {
			long long y = ys[i] * k, x = xs[i] * k;
			int yi, xi, want, got;
			/* products of int16 samples reach 2^30 */
			if (llabs(y) > (1<<30) || llabs(x) > (1<<30))
				continue;
			yi = (int)y; xi = (int)x;
			want = ref_angle(yi, xi);
			got = fast_atan2(yi, xi);
			/* the polynomial is scale invariant: with the 32 bit
			 * overflow it was way off for strong signals */
			CHECK(abs(got - want) < 450);
			got = polar_disc_lut(xi, yi, 1, 0);	/* angle of (x + jy) * conj(1) */
			CHECK(abs(got - want) < 450);
		}
	}
	CHECK(fast_atan2(0, 0) == 0);

	/* dense sweep of the whole circle for several magnitudes, incl. the tiny
	 * angles (|cj/cr| < 1/256) that used to return +pi from the table */
	{
		double mag, a;
		int worst = 0, n = 0;
		for (mag = 200; mag <= 30000; mag *= 3) {
			for (a = -M_PI + 0.001; a < M_PI; a += 0.0173) {
				int yi = (int)lrint(mag * sin(a)), xi = (int)lrint(mag * cos(a));
				int want, got;
				if (!xi && !yi)
					continue;
				want = ref_angle(yi, xi);
				/* discriminator of (x + jy) and 1 + 0j */
				got = polar_disc_lut(xi, yi, 32000, 0);
				if (abs(got - want) > worst && abs(abs(got - want) - 32768) > 100)
					worst = abs(got - want);	/* +-pi wrap is the same angle */
				n++;
			}
		}
		CHECK(n > 1000);
		CHECK(worst < 64);
	}
	/* explicit tiny positive / negative steps */
	CHECK(abs(polar_disc_lut(3000, 8, 3000, 0)) < 20);
	CHECK(abs(polar_disc_lut(3000, -8, 3000, 0)) < 20);
	CHECK(abs(polar_disc_lut(-3000, 8, 3000, 0)) > 16000);	/* near +-pi */
}

static void test_stats(void)
{
	int16_t flat[64], alt[64];
	int i;

	for (i = 0; i < 64; i++) {
		flat[i] = 100;
		alt[i] = (i & 1) ? 100 : -100;
	}
	CHECK(rms(flat, 64, 1) == 0);	/* DC is removed */
	CHECK(abs(rms(alt, 64, 1) - 100) <= 1);
	CHECK(abs(rms(alt, 64, 2) - 100) <= 1 || rms(alt, 64, 2) == 0);
	CHECK(rms(alt, 0, 1) == 0);
	CHECK(mad(flat, 64, 1) == 0);
	CHECK(abs(mad(alt, 64, 1) - 100) <= 1);
	CHECK(mad(alt, 0, 1) == 0);
	CHECK(mad(alt, 1, 4) == 0 || mad(alt, 1, 4) >= 0);	/* len < step: no division by zero */
}

static void test_helpers(void)
{
	unsigned char b[16];
	int16_t s[8];
	struct demod_state d;
	int i;

	/* rotate_90 must leave a trailing partial group alone */
	for (i = 0; i < 16; i++)
		b[i] = (unsigned char)(i * 3);
	rotate_90(b, 12);
	CHECK(b[8] == 24 && b[9] == 27 && b[10] == 30 && b[11] == 33);
	CHECK(b[12] == 36 && b[15] == 45);

	for (i = 0; i < 8; i++)
		s[i] = 1;
	CHECK(low_pass_simple(s, 2, 4) == 0);	/* len < step */

	/* AM: |z| of a full scale sample must saturate, not wrap */
	memset(&d, 0, sizeof(d));
	d.lowpassed[0] = 32767; d.lowpassed[1] = 32767;
	d.lp_len = 2;
	d.output_scale = 3;
	am_demod(&d);
	CHECK(d.result_len == 1 && d.result[0] == 32767);
	d.lowpassed[0] = 30000; d.lowpassed[1] = 30000;
	usb_demod(&d);
	CHECK(d.result[0] == 32767);
	d.lowpassed[0] = -30000; d.lowpassed[1] = 30000;
	lsb_demod(&d);
	CHECK(d.result[0] == -32768);

	/* empty blocks used to divide by zero / read lp[-2] */
	d.lp_len = 0;
	fm_demod(&d);
	CHECK(d.result_len == 0);
	d.result_len = 0;
	dc_block_filter(&d);
	d.deemph_a = 0;
	d.result_len = 4;
	deemph_filter(&d);
	CHECK(clip16(1 << 20) == 32767 && clip16(-(1 << 20)) == -32768);
}

static void test_filters(void)
{
	struct demod_state d;
	int16_t data[128], hist_i[6] = {0}, hist_q[6] = {0}, fh[9] = {0};
	int j, k, bad = 0;

	/* 170k -> 32k resampler: a constant must stay constant (was 1.0 .. 1.2x) */
	memset(&d, 0, sizeof(d));
	d.rate_out = 170000;
	d.rate_out2 = 32000;
	d.result_len = 1700;
	for (j = 0; j < 1700; j++)
		d.result[j] = 1000;
	low_pass_real(&d);
	CHECK(d.result_len > 300);
	for (j = 0; j < d.result_len; j++)
		if (d.result[j] != 1000)
			bad++;
	CHECK(bad == 0);

	/* 5th order decimator: a ramp must stay a ramp across block boundaries */
	for (k = 0; k < 2; k++) {
		for (j = 0; j < 32; j++) {
			data[2*j]   = (int16_t)(4 * (32*k + j));	/* I ramp */
			data[2*j+1] = 0;
		}
		fifth_order(data, 64, hist_i);
		fifth_order(data + 1, 63, hist_q);
		for (j = 0; j < 16; j++) {
			int g = 16*k + j;			/* global output index */
			if (g < 3)
				continue;			/* start-up from zero history */
			CHECK(data[2*j] == 8*(2*g + 1) - 20);
			CHECK(data[2*j+1] == 0);
		}
	}

	/* droop compensation with large samples: 32 bit products used to overflow */
	for (j = 0; j < 128; j += 2) {
		data[j] = 30000; data[j+1] = -30000;
	}
	generic_fir(data, 128, cic_9_tables[3], fh);
	CHECK(data[100] > 0);

	/* partial groups are ignored */
	{
		int16_t s[16];
		for (j = 0; j < 16; j++)
			s[j] = (int16_t)(j + 1);
		CHECK(low_pass_simple(s, 10, 4) == 2);
		CHECK(s[0] == 10 && s[1] == 26);
	}
}

static void test_frequency_range(void)
{
	struct controller_state c;
	char arg1[] = "100M:200M:25M";
	char arg2[] = "100M:105M";
	char arg3[] = "144.8M";

	memset(&c, 0, sizeof(c));
	frequency_range(&c, arg1);
	CHECK(c.freq_len == 5 && c.freqs[4] == 200000000);
	CHECK(c.freqs[0] == 100000000 && c.freqs[1] == 125000000);
	CHECK(strcmp(arg1, "100M:200M:25M") == 0);	/* argv restored */

	memset(&c, 0, sizeof(c));
	frequency_range(&c, arg2);
	CHECK(c.freq_len == 2 && c.freqs[1] == 105000000);
	CHECK(strcmp(arg2, "100M:105M") == 0);

	memset(&c, 0, sizeof(c));
	frequency_range(&c, arg3);
	CHECK(c.freq_len == 1 && c.freqs[0] == 144800000);

	/* array bounds: two more entries when only one slot is left used to overflow freqs[] */
	memset(&c, 0, sizeof(c));
	c.freq_len = FREQUENCIES_LIMIT - 1;
	frequency_range(&c, arg2);
	CHECK(c.freq_len <= FREQUENCIES_LIMIT);

	/* convenience suffix parsers with an empty string used to write s[-1] */
	{
		char empty[1] = "";
		char k[] = "100k";
		char m[] = "2.4M";
		CHECK(atofs(empty) == 0.0);
		CHECK(atoft(empty) == 0.0);
		CHECK(atofp(empty) == 0.0);
		CHECK(atofs(k) == 100000.0);
		CHECK(atofs(m) == 2400000.0);
	}
}

int main(void)
{
	test_atan();
	test_stats();
	test_helpers();
	test_filters();
	test_frequency_range();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_dsp_fm: all checks passed\n");
	return 0;
}
