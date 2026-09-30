/* rtl_adsb decoder tests: synthesise a Mode-S frame and decode it. */
#define main rtl_adsb_main
#include "../src/rtl_adsb.c"
#undef main

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* well known DF17 example frame */
static const char *frame_hex = "8d4840d6202cc371c32ce0576098";

static int hexval(char c)
{
	return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
}

static int frame_bit(int i)
{
	int byte = hexval(frame_hex[2*(i/8)]) * 16 + hexval(frame_hex[2*(i/8)+1]);
	return (byte >> (7 - (i % 8))) & 1;
}

/* IQ sample pair -> magnitude 8100 (high) or 4 (noise floor) */
static void put(uint8_t *buf, int n, int high)
{
	buf[2*n]   = high ? 217 : 129;
	buf[2*n+1] = 127;
}

static char *run_and_read(void)
{
	static char out[512];
	size_t n;
	fflush(file);
	rewind(file);
	n = fread(out, 1, sizeof(out) - 1, file);
	out[n] = 0;
	rewind(file);
	if (ftruncate(fileno(file), 0)) { }
	return out;
}

static void test_decode(void)
{
	uint8_t iq[2 * 400];
	int n = 0, i, bit;
	int preamble_hi[] = { 0, 2, 7, 9 };
	int len;

	squares_precompute();
	file = tmpfile();
	CHECK(file != NULL);
	memset(iq, 0, sizeof(iq));

	for (i = 0; i < 20; i++) put(iq, n++, 0);
	for (i = 0; i < 16; i++) {
		int hi = 0, j;
		for (j = 0; j < 4; j++)
			if (preamble_hi[j] == i)
				hi = 1;
		put(iq, n++, hi);
	}
	for (i = 0; i < 112; i++) {
		bit = frame_bit(i);
		put(iq, n++, bit);
		put(iq, n++, !bit);
	}
	for (i = 0; i < 60; i++) put(iq, n++, 0);

	len = magnitute(iq, 2 * n);
	CHECK(len == n);
	manchester((uint16_t *)iq, len);
	messages((uint16_t *)iq, len);
	CHECK(strcmp(run_and_read(), "*8d4840d6202cc371c32ce0576098;\r\n") == 0);
}

static void fill_bits(uint16_t *b, int nbits, int stop)
{
	int i;
	for (i = 0; i < nbits; i++)
		b[i] = (uint16_t)frame_bit(i);
	b[nbits] = (uint16_t)stop;	/* > 1: not a bit any more */
	b[nbits + 1] = 255;
}

static void test_truncated(void)
{
	uint16_t b[200];

	file = tmpfile();
	/* a complete 112 bit frame is printed */
	memset(b, 0xff, sizeof(b));
	fill_bits(b, 112, 254);
	messages(b, 120);
	CHECK(strcmp(run_and_read(), "*8d4840d6202cc371c32ce0576098;\r\n") == 0);
	/* 111 bits: used to be accepted with a zero-filled last bit */
	memset(b, 0xff, sizeof(b));
	fill_bits(b, 111, 254);
	messages(b, 120);
	CHECK(strcmp(run_and_read(), "") == 0);
}

int main(void)
{
	test_decode();
	test_truncated();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_adsb: all checks passed\n");
	return 0;
}
