/* rtl_tcp send queue: bounded length, no use-after-free at small limits. */
#define main rtl_tcp_main
#include "../src/rtl_tcp.c"
#undef main

static int failures;
#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int queue_len(void)
{
	int n = 0;
	struct llist *l;
	for (l = ll_buffers; l; l = l->next)
		n++;
	return n;
}

static void drain(void)
{
	free_llist(ll_buffers);
	ll_buffers = NULL;
	global_numq = 0;
}

int main(void)
{
	unsigned char buf[64];
	int limit, i;

	memset(buf, 1, sizeof(buf));
	pthread_mutex_init(&ll_mutex, NULL);
	pthread_cond_init(&cond, NULL);

	for (limit = 1; limit <= 6; limit++) {
		llbuf_num = limit;
		for (i = 0; i < 40; i++) {
			rtlsdr_callback(buf, sizeof(buf), NULL);
			CHECK(queue_len() <= limit);
		}
		CHECK(queue_len() == limit);
		drain();
	}

	/* 0 = unlimited */
	llbuf_num = 0;
	for (i = 0; i < 40; i++)
		rtlsdr_callback(buf, sizeof(buf), NULL);
	CHECK(queue_len() == 40);
	drain();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_tcp_queue: all checks passed\n");
	return 0;
}
