#include <assert.h>
#include <string.h>
#include <threads.h>
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

#include "pause.h"

static inline void
random_init(void) {
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

static inline void
random_buf(void *buf, size_t buflen) {
	uint32_t r;
	size_t i;

	for (i = 0; i + sizeof(r) <= buflen; i += sizeof(r)) {
		r = next();
		memmove((uint8_t *)buf + i, &r, sizeof(r));
	}
	r = next();
	memmove((uint8_t *)buf + i, &r, buflen % sizeof(r));
	return;
}

static inline void
time_now(struct timespec *ts) {
	int r = clock_gettime(CLOCKSOURCE, ts);
	assert(r == 0);
}

static inline uint64_t
time_microdiff(const struct timespec *t1, const struct timespec *t2) {
	uint64_t i1 = (uint64_t)t1->tv_sec * NS_PER_SEC + t1->tv_nsec;
	uint64_t i2 = (uint64_t)t2->tv_sec * NS_PER_SEC + t2->tv_nsec;

	assert(i1 >= i2);

	return (i1 - i2) / NS_PER_US;
}
