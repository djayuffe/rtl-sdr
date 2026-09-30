/*
 * Offline benchmark + sensitivity check of the rtl_adsb decoder.
 * usage: bench_adsb [-q]
 *
 * Synthesises DF17 frames at 2 MS/s (8-bit IQ), adds Gaussian noise for a
 * range of SNRs, and reports the exact-decode rate, the false-frame rate on
 * pure noise, and the decoder throughput.
 */
#define main rtl_adsb_main
#include "../src/rtl_adsb.c"
#undef main
#include "bench_common.h"
#include <unistd.h>

static int clip8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5); }

static void put_sample(uint8_t *iq, int n, double amp, double sigma)
{
	iq[2*n]   = (uint8_t)clip8(127.5 + amp + sigma * bench_gauss());
	iq[2*n+1] = (uint8_t)clip8(127.5 + sigma * bench_gauss());
}

/* returns number of samples written */
static int make_frame(uint8_t *iq, const uint8_t *frame, double amp, double sigma)
{
	static const int hi[] = { 0, 2, 7, 9 };
	int n = 0, i, j, bit;

	for (i = 0; i < 40; i++) put_sample(iq, n++, 0, sigma);
	for (i = 0; i < 16; i++) {
		int h = 0;
		for (j = 0; j < 4; j++) if (hi[j] == i) h = 1;
		put_sample(iq, n++, h ? amp : 0, sigma);
	}
	for (i = 0; i < 112; i++) {
		bit = (frame[i / 8] >> (7 - (i % 8))) & 1;
		put_sample(iq, n++, bit ? amp : 0, sigma);
		put_sample(iq, n++, bit ? 0 : amp, sigma);
	}
	for (i = 0; i < 60; i++) put_sample(iq, n++, 0, sigma);
	return n;
}

static int decode_count(uint8_t *iq, int nsamp, char *first, size_t firstlen)
{
	static uint16_t work[400000];
	char out[8192];
	size_t r;
	int len, lines = 0;
	char *p;

	memcpy(work, iq, 2 * nsamp);
	len = magnitute((uint8_t *)work, 2 * nsamp);
	manchester(work, len);
	messages(work, len);
	fflush(file);
	rewind(file);
	r = fread(out, 1, sizeof(out) - 1, file);
	out[r] = 0;
	rewind(file);
	if (ftruncate(fileno(file), 0)) { }
	for (p = out; (p = strchr(p, '*')) != NULL; p++) {
		if (!lines && first) {
			char *e = strchr(p, ';');
			size_t l = e ? (size_t)(e - p) : 0;
			if (l >= firstlen) l = firstlen - 1;
			memcpy(first, p, l); first[l] = 0;
		}
		lines++;
	}
	return lines;
}

int main(int argc, char **argv)
{
	uint8_t frame[14], iq[2 * 400], ref[64];
	int snr, t, trials, k, ok;
	double spc, amp = 60.0;
	static uint8_t big[262144];

	bench_args(argc, argv);
	trials = bench_quick ? 60 : 400;
	squares_precompute();
	file = tmpfile();

	printf("rtl_adsb decoder, quality %d, allowed errors %d%s\n\n", quality, allowed_errors, bench_quick ? " (quick)" : "");
	printf("  %-10s %-12s\n", "SNR [dB]", "decoded exactly");
	for (snr = 4; snr <= 24; snr += 4) {
		double sigma = amp / sqrt(2.0 * pow(10.0, snr / 10.0));
		ok = 0;
		for (t = 0; t < trials; t++) {
			char hex[64], got[64];
			int n;
			frame[0] = 0x8d;
			for (k = 1; k < 14; k++) frame[k] = (uint8_t)(bench_rand() * 256);
			n = make_frame(iq, frame, amp, sigma);
			snprintf(hex, sizeof(hex), "*%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
				 frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6],
				 frame[7], frame[8], frame[9], frame[10], frame[11], frame[12], frame[13]);
			got[0] = 0;
			if (decode_count(iq, n, got, sizeof(got)) >= 1 && !strcmp(got, hex))
				ok++;
		}
		printf("  %-10d %5.1f %%   (%d / %d)\n", snr, 100.0 * ok / trials, ok, trials);
		if (snr >= 20)
			BENCH_EXPECT(100.0 * ok / trials >= 97.0, "only %.1f %% decoded at %d dB SNR (a weak preamble check lost ~17 %% here)",
				     100.0 * ok / trials, snr);
	}

	/* pure noise: how many phantom frames per million samples */
	{
		int fr = 0, buffers = bench_quick ? 4 : 40, b;
		for (b = 0; b < buffers; b++) {
			int i, nsamp = 100000;
			for (i = 0; i < nsamp; i++) put_sample(iq + 0, 0, 0, 0); /* keep iq valid */
			{
				static uint8_t noise[200000];
				double sigma = amp / sqrt(2.0 * pow(10.0, 8 / 10.0));
				for (i = 0; i < nsamp; i++) put_sample(noise, i, 0, sigma);
				fr += decode_count(noise, nsamp, NULL, 0);
			}
		}
		printf("\n  noise only (sigma = %.1f LSB): %d phantom frame(s) in %d Msamples\n",
		       amp / sqrt(2.0 * pow(10.0, 0.8)), fr, buffers / 10);
		BENCH_EXPECT(fr <= 1, "%d phantom frames on noise", fr);
	}

	/* throughput on a 131072-sample buffer holding frames back to back */
	{
		int n = 0;
		double sigma = amp / sqrt(2.0 * pow(10.0, 2.0));
		memset(big, 0, sizeof(big));
		while (n + 400 < 131072) {
			for (k = 1; k < 14; k++) frame[k] = (uint8_t)(bench_rand() * 256);
			n += make_frame(big + 2 * n, frame, amp, sigma) - 40;
		}
		{
			static uint16_t work[131072];
			BENCH_LOOP(BENCH_MIN_S, {
				int len;
				memcpy(work, big, sizeof(big));
				len = magnitute((uint8_t *)work, 2 * 131072);
				manchester(work, len);
				messages(work, len);
				fflush(file); rewind(file); if (ftruncate(fileno(file), 0)) { }
			}, spc);
			printf("  throughput: %.1f MS/s (x%.1f real time at 2 MS/s)\n", 131072 / spc / 1e6, 131072 / spc / 2e6);
		}
	}
	if (bench_failed) {
		printf("\n%d check(s) failed\n", bench_failed);
		return 1;
	}
	return 0;
}
