/*
 * Offline benchmark + accuracy check of the rtl_fm DSP kernels.
 * No hardware needed.   usage: bench_fm [-q]
 *
 * Reports throughput in mega-samples/s (complex IQ samples for the IQ
 * stages) and the real-time factor against a 2.4 MS/s stream, and the
 * accuracy of the three FM discriminators against libm atan2().
 */
#define main rtl_fm_main
#include "../src/rtl_fm.c"
#undef main
#include "bench_common.h"

#define N 65536		/* complex samples per block */

static int16_t iq[2 * N], work[2 * N];

static void row(const char *name, double spc, double nsamp)
{
	double msps = nsamp / spc / 1e6;
	printf("  %-30s %9.1f MS/s   x%-7.1f real time @2.4 MS/s\n", name, msps, msps / 2.4);
}

static void make_fm(double amp)
{
	int n;
	double ph = 0;
	for (n = 0; n < N; n++) {
		/* 75 kHz deviation at fs = 1.0 MS/s, 1 kHz tone */
		double f = 75000.0 * sin(2 * M_PI * 1000.0 * n / 1e6);
		ph += 2 * M_PI * f / 1e6;
		iq[2*n]   = (int16_t)(amp * cos(ph));
		iq[2*n+1] = (int16_t)(amp * sin(ph));
	}
}

static void accuracy(const char *name, int (*disc)(int, int, int, int), double amp)
{
	int n, cnt = 0;
	double se = 0, mx = 0;

	for (n = 1; n < N; n++) {
		int got = disc(iq[2*n], iq[2*n+1], iq[2*n-2], iq[2*n-1]);
		double ref = atan2((double)iq[2*n+1] * iq[2*n-2] - (double)iq[2*n] * iq[2*n-1],
				   (double)iq[2*n] * iq[2*n-2] + (double)iq[2*n+1] * iq[2*n-1]) / M_PI * (1 << 14);
		double e = fabs(got - ref);
		se += e * e;
		if (e > mx)
			mx = e;
		cnt++;
	}
	printf("  %-30s amplitude %5.0f: rms error %7.2f, max %7.2f (units: pi = 16384, i.e. %.3f%% / %.3f%% of full scale)\n",
	       name, amp, sqrt(se / cnt), mx, 100 * sqrt(se / cnt) / 16384, 100 * mx / 16384);
	if (strstr(name, "libm"))
		BENCH_EXPECT(mx < 2.0, "%s max error %.1f", name, mx);
	else if (strstr(name, "table"))
		BENCH_EXPECT(mx < 64.0, "%s max error %.1f (was ~16370 = pi for small angles)", name, mx);
	else
		BENCH_EXPECT(mx < 450.0, "%s max error %.1f", name, mx);
}

int main(int argc, char **argv)
{
	double spc;
	struct demod_state *d = calloc(1, sizeof(*d));
	int16_t hi[6] = {0}, hq[6] = {0}, fh[9] = {0};
	int i, k;
	double amps[] = { 100, 3000, 32000 };

	bench_args(argc, argv);
	atan_lut_init();

	printf("rtl_fm DSP kernels, block = %d complex samples%s\n\n", N, bench_quick ? " (quick)" : "");
	printf("Discriminator accuracy vs atan2 (FM, 75 kHz deviation, fs = 1 MS/s):\n");
	for (k = 0; k < 3; k++) {
		make_fm(amps[k]);
		accuracy("polar_discriminant (libm)", polar_discriminant, amps[k]);
		accuracy("polar_disc_fast (poly)", polar_disc_fast, amps[k]);
		accuracy("polar_disc_lut (table)", polar_disc_lut, amps[k]);
	}

	make_fm(3000);
	printf("\nThroughput:\n");
	BENCH_LOOP(BENCH_MIN_S, { for (i = 1; i < N; i++) work[i] = (int16_t)polar_discriminant(iq[2*i], iq[2*i+1], iq[2*i-2], iq[2*i-1]); }, spc);
	row("FM discriminator: libm atan2", spc, N);
	BENCH_LOOP(BENCH_MIN_S, { for (i = 1; i < N; i++) work[i] = (int16_t)polar_disc_fast(iq[2*i], iq[2*i+1], iq[2*i-2], iq[2*i-1]); }, spc);
	row("FM discriminator: fast poly", spc, N);
	BENCH_LOOP(BENCH_MIN_S, { for (i = 1; i < N; i++) work[i] = (int16_t)polar_disc_lut(iq[2*i], iq[2*i+1], iq[2*i-2], iq[2*i-1]); }, spc);
	row("FM discriminator: lookup table", spc, N);

	d->downsample = 8;
	BENCH_LOOP(BENCH_MIN_S, { memcpy(d->lowpassed, iq, sizeof(iq)); d->lp_len = 2*N; d->prev_index = 0; low_pass(d); }, spc);
	row("boxcar decimate x8", spc, N);
	BENCH_LOOP(BENCH_MIN_S, { memcpy(work, iq, sizeof(iq)); fifth_order(work, 2*N, hi); fifth_order(work+1, 2*N-1, hq); }, spc);
	row("5th order decimate x2 (I+Q)", spc, N);
	BENCH_LOOP(BENCH_MIN_S, { memcpy(work, iq, sizeof(iq)); generic_fir(work, 2*N, cic_9_tables[3], fh); generic_fir(work+1, 2*N-1, cic_9_tables[3], fh); }, spc);
	row("9-tap droop FIR (I+Q)", spc, N);
	memcpy(d->lowpassed, iq, sizeof(iq));
	d->lp_len = 2*N; d->output_scale = 2;
	BENCH_LOOP(BENCH_MIN_S, { am_demod(d); }, spc);
	row("AM envelope", spc, N);
	d->result_len = N;
	BENCH_LOOP(BENCH_MIN_S, { dc_block_filter(d); }, spc);
	row("DC block", spc, N);
	d->deemph_a = 10; d->result_len = N;
	BENCH_LOOP(BENCH_MIN_S, { deemph_filter(d); }, spc);
	row("de-emphasis IIR", spc, N);
	d->rate_out = 170000; d->rate_out2 = 32000;
	BENCH_LOOP(BENCH_MIN_S, { d->result_len = N; d->prev_lpr_index = 0; d->now_lpr = d->now_lpr_n = 0; low_pass_real(d); }, spc);
	row("resampler 170k -> 32k", spc, N);
	BENCH_LOOP(BENCH_MIN_S, { work[0] = (int16_t)rms(iq, 2*N, 1); }, spc);
	row("rms (squelch)", spc, N);

	free(d);
	if (bench_failed) {
		printf("\n%d check(s) failed\n", bench_failed);
		return 1;
	}
	return 0;
}
