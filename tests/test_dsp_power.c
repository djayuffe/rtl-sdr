/* Unit tests for rtl_power helpers (tool source included directly). */
#include <sys/wait.h>
#define main rtl_power_main
#include "../src/rtl_power.c"
#undef main

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* run frequency_range() in a child: returns its exit status, or -signal */
static int range_status(const char *spec, double crop)
{
	pid_t pid;
	int st;
	char buf[128];

	fflush(NULL);
	pid = fork();
	if (pid == 0) {
		if (!freopen("/dev/null", "w", stderr)) { }
		strncpy(buf, spec, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = 0;
		frequency_range(buf, crop);
		_exit(0);
	}
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		return -WTERMSIG(st);
	return WEXITSTATUS(st);
}

static void test_remove_dc(void)
{
	int16_t d[64];
	int i;
	long si = 0, sq = 0;

	for (i = 0; i < 64; i += 2) {
		d[i] = 50 + ((i & 2) ? 20 : -20);	/* I: dc +50 */
		d[i+1] = -30 + ((i & 2) ? 5 : -5);	/* Q: dc -30 */
	}
	remove_dc(d, 64);
	remove_dc(d + 1, 63);
	for (i = 0; i < 64; i += 2) {
		si += d[i];
		sq += d[i+1];
	}
	/* only half of the DC was removed before (divided by len, not len/2) */
	CHECK(labs(si / 32) <= 1);
	CHECK(labs(sq / 32) <= 1);
	remove_dc(d, 0);	/* no crash */
	CHECK(clip16(40000) == 32767 && clip16(-40000) == -32768);
}

static void test_fft(void)
{
	int16_t buf[512];
	int n, k = 10, best = 0;
	long p, bestp = -1;

	sine_table(8);
	for (n = 0; n < 256; n++) {
		buf[2*n]   = (int16_t)(8000 * cos(2 * M_PI * k * n / 256));
		buf[2*n+1] = (int16_t)(8000 * sin(2 * M_PI * k * n / 256));
	}
	CHECK(fix_fft(buf, 8) == 0);
	for (n = 0; n < 256; n++) {
		p = (long)buf[2*n] * buf[2*n] + (long)buf[2*n+1] * buf[2*n+1];
		if (p > bestp) {
			bestp = p;
			best = n;
		}
	}
	CHECK(best == k);
}

static void test_ranges(void)
{
	/* used to divide by zero (SIGFPE) or loop over garbage */
	CHECK(range_status("100M:100M:1k", 0.0) == 1);
	CHECK(range_status("200M:100M:1k", 0.0) == 1);
	CHECK(range_status("100M:200M:0", 0.0) == 1);
	CHECK(range_status("100M:200M", 0.0) == 1);
	CHECK(range_status("nonsense", 0.0) == 1);
	CHECK(range_status("100M:100.001M:1", 0.0) == 0);	/* tiny range, tiny bins */
	CHECK(range_status("88M:108M:125k", 0.0) == 0);
	CHECK(range_status("88M:108M:125k", 0.5) == 0);
}

int main(void)
{
	test_remove_dc();
	test_fft();
	test_ranges();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_dsp_power: all checks passed\n");
	return 0;
}
