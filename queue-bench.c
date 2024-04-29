/*
 * SPDX-FileCopyrightText: 2024 Ondřej Surý
 *
 * SPDX-License-Identifier: WTFPL
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE 1
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
	rwlock_t *crwwp;
	uv_thread_cb cb;
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
	struct rcu_head rcu_head;
	struct cds_lfq_node_rcu node;
};

static uint8_t *rnd;

static void
mutex_queue_run(void *arg0) {
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
			newdata->value = i;
			cds_list_add_tail(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			arg->reads++;
			struct data *data = NULL;

			uv_mutex_lock(arg->mutex);
			data = cds_list_first_entry(head, struct data, head);
			if (data == NULL) {
				uv_mutex_unlock(arg->mutex);
				continue;
			}

			cds_list_del(&data->head);
			uv_mutex_unlock(arg->mutex);

			/* Do something with **data** */
			if (data != NULL) {
				free(data);
			}
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
rwlock_queue_run(void *arg0) {
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
			newdata->value = i;
			cds_list_add_tail(&newdata->head, head);
			pthread_rwlock_unlock(arg->rwlock);
		} else {
			arg->reads++;
			struct data *data;

			pthread_rwlock_rdlock(arg->rwlock);
			data = cds_list_first_entry(head, struct data, head);
			pthread_rwlock_unlock(arg->rwlock);
			if (data == NULL) {
				continue;
			}

			pthread_rwlock_wrlock(arg->rwlock);
			data = cds_list_first_entry(head, struct data, head);
			cds_list_del_rcu(&data->head);
			pthread_rwlock_unlock(arg->rwlock);

			/* Do something with **data** */
			if (data != NULL) {
				free(data);
			}
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
crwwp_queue_run(void *arg0) {
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
			newdata->value = i;
			cds_list_add_tail(&newdata->head, head);
			rwlock_wrunlock(arg->crwwp);
		} else {
			arg->reads++;
			struct data *data;

			rwlock_rdlock(arg->crwwp);
			data = cds_list_first_entry(head, struct data, head);
			if (data == NULL) {
				rwlock_rdunlock(arg->crwwp);
				continue;
			}

			int r = rwlock_tryupgrade(arg->crwwp);
			if (r != 0) {
				assert(r == EBUSY);
				rwlock_rdunlock(arg->crwwp);
				rwlock_wrlock(arg->crwwp);
				data = cds_list_first_entry(head, struct data, head);
			}

			if (data != NULL) {
				cds_list_del_rcu(&data->head);
			}
			rwlock_wrunlock(arg->crwwp);

			/* Do something with **data** */
			if (data != NULL) {
				free(data);
			}
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);
}

static void
free_data_rcu(struct rcu_head *rcu_head) {
	struct data *data = caa_container_of(rcu_head, struct data, rcu_head);
	free(data);
}

static void
rcu_queue_run(void *arg0) {
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
			newdata->value = i;
			cds_list_add_tail_rcu(&newdata->head, head);
			uv_mutex_unlock(arg->mutex);
		} else {
			arg->reads++;
			struct data *data;

			rcu_read_lock();
			data = cds_list_first_entry(head, struct data, head);
			rcu_read_unlock();
			if (data == NULL) {
				continue;
			}

			uv_mutex_lock(arg->mutex);
			data = cds_list_first_entry(head, struct data, head);
			cds_list_del(&data->head);
			uv_mutex_unlock(arg->mutex);

			if (data != NULL) {
				/* Do something with **data** */
				call_rcu(&data->rcu_head, free_data_rcu);
			}
		}
	}

	time_now(&end);

	arg->diff = time_microdiff(&end, &start);

	rcu_unregister_thread();
}

static void
lfqueue_run(void *arg0) {
	struct thread_s *arg = arg0;
	struct timespec start, end;
	struct cds_lfq_queue_rcu *queue = arg->data;

	rcu_register_thread();
	(void)uv_barrier_wait(arg->barrier);

	time_now(&start);

	for (size_t i = 0; i < arg->ops; i++) {
		if (rnd[i]) {
			arg->writes++;
			struct data *newdata = malloc(sizeof(*newdata));
			newdata->value = i;
			cds_lfq_node_init_rcu(&newdata->node);

			rcu_read_lock();
			cds_lfq_enqueue_rcu(queue, &newdata->node);
			rcu_read_unlock();
		} else {
			arg->reads++;
			struct cds_lfq_node_rcu *node = NULL;
			struct data *data = NULL;

			rcu_read_lock();
			node = cds_lfq_dequeue_rcu(queue);
			rcu_read_unlock();
			if (node == NULL) {
				continue;
			}

			data = (node != NULL) ? caa_container_of(node, struct data, node) : NULL;

			/* Do something with **data** */
			if (data != NULL) {
				call_rcu(&data->rcu_head, free_data_rcu);
			}
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
	void *(*new)(size_t nelements);
	uv_thread_cb run;
	void (*destroy)(void *);
};

static void *
list_new(size_t nelements) {
	struct cds_list_head *head = malloc(sizeof(*head));
	CDS_INIT_LIST_HEAD(head);

	for (size_t i = 0; i < nelements; i++) {
		struct data *newdata = malloc(sizeof(*newdata));
		cds_list_add_tail(&newdata->head, head);
	}

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

static void *
lfqueue_new(size_t nelements) {
	struct cds_lfq_queue_rcu *queue = malloc(sizeof(*queue));

	cds_lfq_init_rcu(queue, call_rcu);

	for (size_t i = 0; i < nelements; i++) {
		struct data *data = malloc(sizeof(*data));
		data->value = i;
		rcu_read_lock();
		cds_lfq_node_init_rcu(&data->node);
		cds_lfq_enqueue_rcu(queue, &data->node);
		rcu_read_unlock();
	}

	return queue;
}

static void
lfqueue_destroy(void *arg) {
	struct cds_lfq_queue_rcu *queue = arg;
	struct cds_lfq_node_rcu *node = NULL;

	while ((node = cds_lfq_dequeue_rcu(queue)) != NULL) {
		struct data *data = caa_container_of(node, struct data, node);

		free(data);
	}

	cds_lfq_destroy_rcu(queue);
}

static struct test test_list[] = {
	{ "mutex", list_new, mutex_queue_run, list_destroy },
	{ "rwlock", list_new, rwlock_queue_run, list_destroy },
	{ "c-rw-wp", list_new, crwwp_queue_run, list_destroy },
	{ "rculist", list_new, rcu_queue_run, list_destroy },
	{ "lfqueue", lfqueue_new, lfqueue_run, lfqueue_destroy },
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
	}

	random_init();

	threads = calloc(num_threads, sizeof(threads[0]));

	rnd = calloc(num_ops, sizeof(rnd[0]));
	random_buf(rnd, num_ops * sizeof(rnd[0]));

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

		void *data = test->new(num_ops * num_threads);

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

	return 0;
}
