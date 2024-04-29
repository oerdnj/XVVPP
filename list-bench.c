/*
 * SPDX-FileCopyrightText: 2024 Ondřej Surý
 *
 * SPDX-License-Identifier: WTFPL
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <urcu.h>
#include <urcu/cds.h>
#include <uv.h>

#include "rwlock.h"
#include "util.h"

struct thread_s {
	uv_thread_t thread;
	uv_mutex_t *mutex;
	pthread_rwlock_t *rwlock;
	uv_barrier_t *barrier;
	uv_thread_cb cb;
	rwlock_t *crwwp;
	uint64_t ops;
	uint64_t reads;
	uint64_t writes;
	uint64_t diff;
	uint8_t rws;
	void *data;
};

struct data {
	uint64_t value; /* Node content */
	struct cds_list_head head;
};

static bool *rnd;

static void
mutex_list_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_list_head *head = arg->data;

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (rnd[i]) {
			arg->writes++;
			struct data *newdata = malloc(sizeof(*newdata));
			uv_mutex_lock(arg->mutex);
			cds_list_add(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			arg->reads++;
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

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (rnd[i]) {
			arg->writes++;
			struct data *newdata = malloc(sizeof(*newdata));
			pthread_rwlock_wrlock(arg->rwlock);
			cds_list_add(&newdata->head, head);
			pthread_rwlock_unlock(arg->rwlock);
		} else {
			arg->reads++;
			pthread_rwlock_rdlock(arg->rwlock);
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
			pthread_rwlock_unlock(arg->rwlock);
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
crwwp_list_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_list_head *head = arg->data;

	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (rnd[i]) {
			arg->writes++;
			struct data *newdata = malloc(sizeof(*newdata));
			rwlock_wrlock(arg->crwwp);
			cds_list_add(&newdata->head, head);
			rwlock_wrunlock(arg->crwwp);
		} else {
			arg->reads++;
			rwlock_rdlock(arg->crwwp);
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
			rwlock_rdunlock(arg->crwwp);
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
	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (rnd[i]) {
			arg->writes++;
			struct data *newdata = malloc(sizeof(*newdata));
			uv_mutex_lock(arg->mutex);
			cds_list_add_rcu(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			arg->reads++;
			rcu_read_lock();
			struct cds_list_head *pos, *p;
			cds_list_for_each_safe(pos, p, head);
			rcu_read_unlock();
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);

	rcu_unregister_thread();
}

struct thread_s *threads;

void
usage(int argc [[maybe_unused]], char **argv) {
	fprintf(stderr, "usage: %s <num_threads> <num_ops> <read_write_ratio> [<r|w|n>]\n", argv[0]);
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

static void
list_destroy(void *arg) {
	struct cds_list_head *head = arg;
	struct cds_list_head *pos, *p;

	cds_list_for_each_safe(pos, p, head) {
		struct data *data = caa_container_of(pos, struct data, head);
		free(data);
	}
}

static struct test test_list[] = {
	{ "mutex", list_new, mutex_list_run, list_destroy },
	{ "rwlock", list_new, rwlock_list_run, list_destroy },
	{ "c-rw-wp", list_new, crwwp_list_run, list_destroy },
	{ "rcu", list_new, rcu_list_run, list_destroy },
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
	uint64_t writes = 0;
	uint64_t reads = 0;
	pthread_rwlockattr_t attr;

	if (argc > 4) {
		int r;
		if (argv[4][0] == 'r') {
			r = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_READER_NP);
		} else if (argv[4][0] == 'w') {
			r = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NP);
		} else if (argv[4][0] == 'n') {
			r = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
		} else {
			usage(argc, argv);
			exit(1);
		}

		assert(r == 0);
	} else {
		int r = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_READER_NP);
		assert(r == 0);
	}

	random_init();

	threads = calloc(num_threads, sizeof(threads[0]));

	rnd = calloc(num_ops, sizeof(*rnd));
	random_buf(rnd, num_ops * sizeof(*rnd));

	uint32_t tmp = (rws * 255) / 100;
	for (size_t i = 0; i < num_ops; i++) {
		if (rnd[i] < tmp) {
			writes++;
			rnd[i] = true;
		} else {
			reads++;
			rnd[i] = false;
		}
	}

	printf("%10s | %10s | %10s | %10s | %10s \n", "", "threads", "reads", "writes", "seconds");

	for (struct test *test = test_list; test->name != NULL; test++) {
		uv_mutex_t mutex;
		pthread_rwlock_t rwlock;
		uv_barrier_t barrier;
		rwlock_t crwwp;

		int r = uv_barrier_init(&barrier, num_threads);
		assert(r == 0);

		r = uv_mutex_init(&mutex);
		assert(r == 0);

		r = pthread_rwlock_init(&rwlock, &attr);
		assert(r == 0);

		rwlock_setworkers(num_threads);
		rwlock_init(&crwwp);

		void *data = test->new();

		for (size_t i = 0; i < num_threads; i++) {
			struct thread_s *t = &threads[i];
			*t = (struct thread_s){
				.barrier = &barrier,
				.mutex = &mutex,
				.rwlock = &rwlock,
				.crwwp = &crwwp,
				.ops = num_ops,
				.rws = rws,
				.data = data,
			};

			r = uv_thread_create(&t->thread, test->run, t);
			assert(r == 0);
		}

		uint64_t diff = 0;
		writes = 0;
		reads = 0;
		for (size_t i = 0; i < num_threads; i++) {
			struct thread_s *t = &threads[i];
			r = uv_thread_join(&t->thread);
			assert(r == 0);

			diff += t->diff;
			writes += t->writes;
			reads += t->reads;
		}

		printf("%10s | %10zu | %10" PRIu64 " | %10" PRIu64 " | %10.4f \n", test->name, (size_t)num_threads,
		       reads, writes, (double)(diff / num_threads) / (US_PER_SEC));

		test->destroy(data);

		rwlock_destroy(&crwwp);
		uv_mutex_destroy(&mutex);
		pthread_rwlock_destroy(&rwlock);
		uv_barrier_destroy(&barrier);
	}

	free(rnd);
	free(threads);

	return 0;
}
