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

#pragma once

#include <inttypes.h>
#include <stdlib.h>

/*! \file isc/rwlock.h */

typedef enum { rwlocktype_none = 0, rwlocktype_read, rwlocktype_write } rwlocktype_t;

#include <stdatomic.h>

#define CACHELINE_SIZE 64

struct rwlock {
	atomic_uint_fast32_t readers_ingress;
	uint8_t __padding1[CACHELINE_SIZE - sizeof(atomic_uint_fast32_t)];
	atomic_uint_fast32_t readers_egress;
	uint8_t __padding2[CACHELINE_SIZE - sizeof(atomic_uint_fast32_t)];
	atomic_int_fast32_t writers_barrier;
	uint8_t __padding3[CACHELINE_SIZE - sizeof(atomic_int_fast32_t)];
	atomic_bool writers_lock;
};

typedef struct rwlock rwlock_t;

void
rwlock_init(rwlock_t *rwl);

void
rwlock_rdlock(rwlock_t *rwl);

void
rwlock_wrlock(rwlock_t *rwl);

int
rwlock_tryrdlock(rwlock_t *rwl);

int
rwlock_trywrlock(rwlock_t *rwl);

void
rwlock_rdunlock(rwlock_t *rwl);

void
rwlock_wrunlock(rwlock_t *rwl);

int
rwlock_tryupgrade(rwlock_t *rwl);

void
rwlock_downgrade(rwlock_t *rwl);

void
rwlock_destroy(rwlock_t *rwl);

void
rwlock_setworkers(uint16_t workers);

#define rwlock_lock(rwl, type)              \
	{                                   \
		switch (type) {             \
		case rwlocktype_read:       \
			rwlock_rdlock(rwl); \
			break;              \
		case rwlocktype_write:      \
			rwlock_wrlock(rwl); \
			break;              \
		default:                    \
			UNREACHABLE();      \
		}                           \
	}

#define rwlock_trylock(rwl, type)                         \
	({                                                \
		int __result;                             \
		switch (type) {                           \
		case rwlocktype_read:                     \
			__result = rwlock_tryrdlock(rwl); \
			break;                            \
		case rwlocktype_write:                    \
			__result = rwlock_trywrlock(rwl); \
			break;                            \
		default:                                  \
			UNREACHABLE();                    \
		}                                         \
		__result;                                 \
	})

#define rwlock_unlock(rwl, type)              \
	{                                     \
		switch (type) {               \
		case rwlocktype_read:         \
			rwlock_rdunlock(rwl); \
			break;                \
		case rwlocktype_write:        \
			rwlock_wrunlock(rwl); \
			break;                \
		default:                      \
			UNREACHABLE();        \
		}                             \
	}
