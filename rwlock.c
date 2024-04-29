/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*
 * Modified C-RW-WP Implementation from NUMA-Aware Reader-Writer Locks paper:
 * http://dl.acm.org/citation.cfm?id=2442532
 *
 * This work is based on C++ code available from
 * https://github.com/pramalhe/ConcurrencyFreaks/
 *
 * Copyright (c) 2014-2016, Pedro Ramalhete, Andreia Correia
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Concurrency Freaks nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER>
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*! \file */

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

#include "atomic.h"
#include "pause.h"
#include "rwlock.h"

static atomic_uint_fast16_t _crwlock_workers = 128;

#define RWLOCK_UNLOCKED false
#define RWLOCK_LOCKED	true

/*
 * See https://csce.ucmss.com/cr/books/2017/LFS/CSREA2017/FCS3701.pdf for
 * guidance on patience level
 */
#ifndef RWLOCK_MAX_READER_PATIENCE
#define RWLOCK_MAX_READER_PATIENCE 500
#endif /* ifndef RWLOCK_MAX_READER_PATIENCE */

static void
read_indicator_wait_until_empty(rwlock_t *rwl);

#include <stdio.h>

static void
read_indicator_arrive(rwlock_t *rwl) {
	(void)atomic_fetch_add_release(&rwl->readers_ingress, 1);
}

static void
read_indicator_depart(rwlock_t *rwl) {
	(void)atomic_fetch_add_release(&rwl->readers_egress, 1);
}

static bool
read_indicator_isempty(rwlock_t *rwl) {
	return (atomic_load_acquire(&rwl->readers_egress) == atomic_load_acquire(&rwl->readers_ingress));
}

static void
writers_barrier_raise(rwlock_t *rwl) {
	(void)atomic_fetch_add_release(&rwl->writers_barrier, 1);
}

static void
writers_barrier_lower(rwlock_t *rwl) {
	(void)atomic_fetch_sub_release(&rwl->writers_barrier, 1);
}

static bool
writers_barrier_israised(rwlock_t *rwl) {
	return (atomic_load_acquire(&rwl->writers_barrier) > 0);
}

static bool
writers_lock_islocked(rwlock_t *rwl) {
	return (atomic_load_acquire(&rwl->writers_lock) == RWLOCK_LOCKED);
}

static bool
writers_lock_acquire(rwlock_t *rwl) {
	return (atomic_compare_exchange_weak_acq_rel(&rwl->writers_lock, &(bool){ RWLOCK_UNLOCKED }, RWLOCK_LOCKED));
}

static void
writers_lock_release(rwlock_t *rwl) {
	bool done = atomic_compare_exchange_strong_acq_rel(&rwl->writers_lock, &(bool){ RWLOCK_LOCKED },
							   RWLOCK_UNLOCKED);
	assert(done);
}

#define ran_out_of_patience(cnt) (cnt >= RWLOCK_MAX_READER_PATIENCE)

void
rwlock_rdlock(rwlock_t *rwl) {
	uint32_t cnt = 0;
	bool barrier_raised = false;

	while (true) {
		read_indicator_arrive(rwl);
		if (!writers_lock_islocked(rwl)) {
			/* Acquired lock in read-only mode */
			break;
		}

		/* Writer has acquired the lock, must reset to 0 and wait */
		read_indicator_depart(rwl);

		while (writers_lock_islocked(rwl)) {
			pause();
			if (ran_out_of_patience(cnt++) && !barrier_raised) {
				writers_barrier_raise(rwl);
				barrier_raised = true;
			}
		}
	}
	if (barrier_raised) {
		writers_barrier_lower(rwl);
	}
}

int
rwlock_tryrdlock(rwlock_t *rwl) {
	read_indicator_arrive(rwl);
	if (writers_lock_islocked(rwl)) {
		/* Writer has acquired the lock, release the read lock */
		read_indicator_depart(rwl);

		return (EBUSY);
	}

	/* Acquired lock in read-only mode */
	return (0);
}

void
rwlock_rdunlock(rwlock_t *rwl) {
	read_indicator_depart(rwl);
}

int
rwlock_tryupgrade(rwlock_t *rwl) {
	/* Write Barriers has been raised */
	if (writers_barrier_israised(rwl)) {
		return (EBUSY);
	}

	/* Try to acquire the write-lock */
	if (!writers_lock_acquire(rwl)) {
		return (EBUSY);
	}

	/* Unlock the read-lock */
	read_indicator_depart(rwl);

	if (!read_indicator_isempty(rwl)) {
		/* Re-acquire the read-lock back */
		read_indicator_arrive(rwl);

		/* Unlock the write-lock */
		writers_lock_release(rwl);
		return (EBUSY);
	}
	return (0);
}

static void
read_indicator_wait_until_empty(rwlock_t *rwl) {
	/* Write-lock was acquired, now wait for running Readers to finish */
	while (true) {
		if (read_indicator_isempty(rwl)) {
			break;
		}
		pause();
	}
}

void
rwlock_wrlock(rwlock_t *rwl) {
	/* Write Barriers has been raised, wait */
	while (writers_barrier_israised(rwl)) {
		pause();
	}

	/* Try to acquire the write-lock */
	while (!writers_lock_acquire(rwl)) {
		pause();
	}

	read_indicator_wait_until_empty(rwl);
}

void
rwlock_wrunlock(rwlock_t *rwl) {
	writers_lock_release(rwl);
}

int
rwlock_trywrlock(rwlock_t *rwl) {
	/* Write Barriers has been raised */
	if (writers_barrier_israised(rwl)) {
		return (EBUSY);
	}

	/* Try to acquire the write-lock */
	if (!writers_lock_acquire(rwl)) {
		return (EBUSY);
	}

	if (!read_indicator_isempty(rwl)) {
		/* Unlock the write-lock */
		writers_lock_release(rwl);

		return (EBUSY);
	}

	return (0);
}

void
rwlock_downgrade(rwlock_t *rwl) {
	read_indicator_arrive(rwl);

	writers_lock_release(rwl);
}

void
rwlock_init(rwlock_t *rwl) {
	atomic_init(&rwl->writers_lock, RWLOCK_UNLOCKED);
	atomic_init(&rwl->writers_barrier, 0);
	atomic_init(&rwl->readers_ingress, 0);
	atomic_init(&rwl->readers_egress, 0);
}

void
rwlock_destroy(rwlock_t *rwl) {
	/* Check whether write lock has been unlocked */
	bool unlocked = atomic_load(&rwl->writers_lock) == RWLOCK_UNLOCKED;
	assert(unlocked);
	bool empty = read_indicator_isempty(rwl);
	assert(empty);
}

void
rwlock_setworkers(uint16_t workers) {
	atomic_store(&_crwlock_workers, workers);
}
