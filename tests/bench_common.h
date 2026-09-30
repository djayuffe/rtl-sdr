/* tiny helpers shared by the offline DSP benchmarks */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static int bench_quick;
static int bench_check;		/* -c: exit non-zero when a quality threshold is missed */
static int bench_failed;

#define BENCH_EXPECT(cond, ...) do { \
	if (bench_check && !(cond)) { \
		printf("  CHECK FAILED: "); printf(__VA_ARGS__); printf("\n"); \
		bench_failed++; \
	} \
} while (0)

static double bench_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* run f() until at least `min_s` seconds elapsed; returns seconds per call */
#define BENCH_LOOP(min_s, body, out_spc) do { \
	double t0_ = bench_now(), t1_; \
	long n_ = 0; \
	do { body; n_++; t1_ = bench_now(); } while (t1_ - t0_ < (min_s)); \
	(out_spc) = (t1_ - t0_) / n_; \
} while (0)

/* deterministic gaussian-ish noise */
static uint32_t bench_seed = 1;
static double bench_rand(void)		/* uniform [0,1) */
{
	bench_seed = bench_seed * 1664525u + 1013904223u;
	return (bench_seed >> 8) / 16777216.0;
}
static double bench_gauss(void)		/* N(0,1), Box-Muller */
{
	double u = bench_rand() + 1e-12, v = bench_rand();
	return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

static void bench_args(int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-q"))
			bench_quick = 1;
		if (!strcmp(argv[i], "-c"))
			bench_check = 1;
	}
}

#define BENCH_MIN_S (bench_quick ? 0.02 : 0.25)

#endif
