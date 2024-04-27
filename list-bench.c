/*
 * SPDX-FileCopyrightText: 2024 Ondřej Surý
 *
 * SPDX-License-Identifier: WTFPL
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <assert.h>
#include <stdatomic.h>
#include <threads.h>
#include <time.h>
#include <urcu.h>
#include <urcu/cds.h>
#include <uv.h>

enum {
	MS_PER_SEC = 1000,		 /*%< Milliseonds per second. */
	US_PER_MS = 1000,		 /*%< Microseconds per millisecond. */
	US_PER_SEC = 1000 * 1000,	 /*%< Microseconds per second. */
	NS_PER_US = 1000,		 /*%< Nanoseconds per microsecond. */
	NS_PER_MS = 1000 * 1000,	 /*%< Nanoseconds per millisecond. */
	NS_PER_SEC = 1000 * 1000 * 1000, /*%< Nanoseconds per second. */
};

#if defined(CLOCK_REALTIME)
#define CLOCKSOURCE CLOCK_REALTIME
#elif defined(CLOCK_REALTIME_COARSE)
#define CLOCKSOURCE CLOCK_REALTIME_COARSE
#elif defined(CLOCK_REALTIME_FAST)
#define CLOCKSOURCE CLOCK_REALTIME_FAST
#else /* if defined(CLOCK_REALTIME_COARSE) */
#define CLOCKSOURCE CLOCK_REALTIME
#endif /* if defined(CLOCK_REALTIME_COARSE) */

static thread_local uint32_t seed[4] = { 0 };

void random_init(void) {
	int r = uv_random(NULL, NULL, seed, sizeof(seed), 0, NULL);
	assert(r == 0);
}

static uint32_t
rotl(const uint32_t x, int k) {
	return ((x << k) | (x >> (32 - k)));
}

static uint32_t
next(void) {
	uint32_t result_starstar, t;

	result_starstar = rotl(seed[0] * 5, 7) * 9;
	t = seed[1] << 9;

	seed[2] ^= seed[0];
	seed[3] ^= seed[1];
	seed[1] ^= seed[2];
	seed[0] ^= seed[3];

	seed[2] ^= t;

	seed[3] = rotl(seed[3], 11);

	return (result_starstar);
}

uint8_t
random8(void) {
	return ((uint8_t)next());
}

static void
time_now(struct timespec *ts) {
	int r = clock_gettime(CLOCKSOURCE, ts);
	assert(r == 0);
}

uint64_t
time_microdiff(const struct timespec *t1, const struct timespec *t2) {
	uint64_t i1 = (uint64_t)t1->tv_sec * NS_PER_SEC + t1->tv_nsec;
	uint64_t i2 = (uint64_t)t2->tv_sec * NS_PER_SEC + t2->tv_nsec;

	assert(i1 >= i2);

	return (i1 - i2) / NS_PER_US;
}

struct thread_s {
	union {
		struct {
			uv_thread_t thread;
			uv_mutex_t *mutex;
			uv_rwlock_t *rwlock;
			uv_barrier_t *barrier;
			uv_thread_cb cb;
			uint64_t ops;
			uint8_t rws;
			uint64_t diff;
			void *data;
		};
		uint8_t __padding[64];
	};
};

static bool
is_write(uint8_t num, uint8_t percent) {
	uint32_t tmp = (num * 100) / 256;

	return tmp <= percent;
}

struct data {
	struct cds_list_head head;
};

static void
mutex_list_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_list_head *head = arg->data;

	random_init();

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (is_write(random8(), arg->rws)) {
			uv_mutex_lock(arg->mutex);
			struct data *newdata = malloc(sizeof(*newdata));
			cds_list_add(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			uv_mutex_lock(arg->mutex);
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
			uv_mutex_unlock(arg->mutex);
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
rwlock_list_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_list_head *head = arg->data;

	random_init();

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (is_write(random8(), arg->rws)) {
			uv_rwlock_wrlock(arg->rwlock);
			struct data *newdata = malloc(sizeof(*newdata));
			cds_list_add(&newdata->head, head);
			uv_rwlock_wrunlock(arg->rwlock);
		} else {
			uv_rwlock_rdlock(arg->rwlock);
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
			uv_rwlock_rdunlock(arg->rwlock);
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
rcu_list_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_list_head *head = arg->data;

	rcu_register_thread();
	random_init();

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (is_write(random8(), arg->rws)) {
			uv_mutex_lock(arg->mutex);
			struct data *newdata = malloc(sizeof(*newdata));
			cds_list_add_rcu(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);

	rcu_unregister_thread();
}

#define NUM_THREADS 128

struct thread_s threads[NUM_THREADS];

void
usage(int argc [[maybe_unused]], char **argv) {
	fprintf(stderr, "usage: %s <num_threads> <num_ops> <read_write_ratio>\n", argv[0]);
}

struct test {
	const char *name;
	void *(*new)(void);
	uv_thread_cb run;
	void (*destroy)(void *);
};

static void *
list_new(void) {
	struct cds_list_head *head = malloc(sizeof(*head));
	CDS_INIT_LIST_HEAD(head);

	return head;
}

static struct test test_list[] = {
	{ "mutex", list_new, mutex_list_run, free },
	{ "rwlock", list_new, rwlock_list_run, free },
	{ "rcu", list_new, rcu_list_run, free },
	{ NULL, NULL, NULL, NULL },
};

int
main(int argc, char **argv) {
	if (argc < 4) {
		usage(argc, argv);
		exit(1);
	}

	uint8_t num_threads = atoi(argv[1]);
	uint64_t num_ops = atoll(argv[2]);
	uint8_t rws = atoi(argv[3]);

	for (struct test *test = test_list; test->name != NULL; test++) {
		uv_mutex_t mutex;
		uv_rwlock_t rwlock;
		uv_barrier_t barrier;

		int r = uv_barrier_init(&barrier, num_threads);
		assert(r == 0);

		r = uv_mutex_init(&mutex);
		assert(r == 0);

		r = uv_rwlock_init(&rwlock);
		assert(r == 0);

		void *data = test->new();

		for (size_t i = 0; i < num_threads; i++) {
			struct thread_s *t = &threads[i];
			*t = (struct thread_s){
				.barrier = &barrier,
				.mutex = &mutex,
				.rwlock = &rwlock,
				.ops = num_ops,
				.rws = rws,
				.data = data,
			};

			r = uv_thread_create(&t->thread, test->run, t);
			assert(r == 0);
		}

		uint64_t diff = 0;
		for (size_t i = 0; i < num_threads; i++) {
			struct thread_s *t = &threads[i];
			r = uv_thread_join(&t->thread);
			assert(r == 0);

			diff += t->diff;
		}

		printf("%10s | %10zu | %10.4f \n", test->name, (size_t)num_threads,
		       (double)(diff / num_threads) / (1000.0 * 1000.0)

		);

		test->destroy(data);

		uv_mutex_destroy(&mutex);
		uv_rwlock_destroy(&rwlock);
		uv_barrier_destroy(&barrier);
	}
	return 0;
}
