/*
 * Offline benchmark + accuracy check of the rtl_power FFT and windows.
 * usage: bench_power [-q]
 *
 * Accuracy: fix_fft() (16-bit fixed point, scaled by 1/N) against a double
 * precision FFT of the same input. Window table: coherent gain and equivalent
 * noise bandwidth (rtl_power does not normalise for them).
 */
#define main rtl_power_main
#include "../src/rtl_power.c"
#undef main
#include "bench_common.h"

static void ref_fft(double *re, double *im, int n)
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
		for (k = 0; k < n; k += m) {
			for (i = 0; i < m / 2; i++) {
				double wr = cos(ang * i), wi = sin(ang * i);
				double xr = re[k+i+m/2] * wr - im[k+i+m/2] * wi;
				double xi = re[k+i+m/2] * wi + im[k+i+m/2] * wr;
				re[k+i+m/2] = re[k+i] - xr; im[k+i+m/2] = im[k+i] - xi;
				re[k+i] += xr; im[k+i] += xi;
			}
		}
	}
}

int main(int argc, char **argv)
{
	int e, i;
	static int16_t buf[2 * 65536], src[2 * 65536];
	static double re[65536], im[65536];
	double spc;
	struct { const char *n; double (*f)(int, int); } win[] = {
		{"rectangle", rectangle}, {"hamming", hamming}, {"blackman", blackman},
		{"blackman-harris", blackman_harris}, {"hann-poisson", hann_poisson},
		{"youssef", youssef}, {"bartlett", bartlett}, {"kaiser (= rectangle)", kaiser} };

	bench_args(argc, argv);
	printf("rtl_power fixed-point FFT%s\n\n", bench_quick ? " (quick)" : "");
	printf("  %-8s %12s %12s %16s %s\n", "size", "FFT/s", "MS/s", "SNR vs double", "noise floor (dB re full scale tone)");

	for (e = 8; e <= (bench_quick ? 12 : 16); e += 2) {
		int n = 1 << e;
		double sig = 0, err = 0, floor_p = 0;

		free(Sinewave); free(power_table);
		sine_table(e);
		for (i = 0; i < n; i++) {
			double t = 20000 * cos(2 * M_PI * 37.3 * i / n) + 3000 * bench_gauss();
			double q = 20000 * sin(2 * M_PI * 37.3 * i / n) + 3000 * bench_gauss();
			if (t > 32767) { t = 32767; }
			if (t < -32768) { t = -32768; }
			if (q > 32767) { q = 32767; }
			if (q < -32768) { q = -32768; }
			src[2*i] = (int16_t)t; src[2*i+1] = (int16_t)q;
			re[i] = src[2*i]; im[i] = src[2*i+1];
		}
		memcpy(buf, src, 2 * n * sizeof(int16_t));
		fix_fft(buf, e);
		ref_fft(re, im, n);
		for (i = 0; i < n; i++) {
			double rr = re[i] / n, ri = im[i] / n;
			double dr = buf[2*i] - rr, di = buf[2*i+1] - ri;
			sig += rr * rr + ri * ri;
			err += dr * dr + di * di;
		}
		floor_p = err / n;	/* mean squared error per bin */
		BENCH_LOOP(BENCH_MIN_S, { memcpy(buf, src, 2 * n * sizeof(int16_t)); fix_fft(buf, e); }, spc);
		printf("  %-8d %12.0f %12.2f %13.1f dB %10.1f\n", n, 1.0 / spc, n / spc / 1e6,
		       10 * log10(sig / err), 10 * log10(floor_p / (32768.0 * 32768.0)));
		BENCH_EXPECT(10 * log10(sig / err) > 30.0, "FFT %d SNR vs double only %.1f dB", n, 10 * log10(sig / err));
	}

	printf("\nWindows (length 1024; rtl_power scales by 256*w and does not correct for them):\n");
	printf("  %-22s %14s %16s\n", "window", "coherent gain", "ENBW [bins]");
	for (i = 0; i < (int)(sizeof(win) / sizeof(win[0])); i++) {
		double s1 = 0, s2 = 0;
		int k, n = 1024;
		for (k = 0; k < n; k++) {
			double w = win[i].f(k, n);
			s1 += w; s2 += w * w;
		}
		printf("  %-22s %14.4f %16.4f\n", win[i].n, s1 / n, n * s2 / (s1 * s1));
		BENCH_EXPECT(s1 / n > 0.25 && s1 / n <= 1.0, "%s coherent gain %.3f", win[i].n, s1 / n);
	}
	if (bench_failed) {
		printf("\n%d check(s) failed\n", bench_failed);
		return 1;
	}
	return 0;
}
