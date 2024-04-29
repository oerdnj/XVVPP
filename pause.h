#if defined(__x86_64__)
#include <immintrin.h>
#define pause() _mm_pause()
#elif defined(__i386__)
#define pause() __asm__ __volatile__("rep; nop")
#elif defined(__ia64__)
#define pause() __asm__ __volatile__("hint @pause")
#elif defined(__aarch64__)
#define pause() __asm__ __volatile__("isb")
#elif defined(__arm__) && HAVE_ARM_YIELD
#define pause() __asm__ __volatile__("yield")
#elif defined(sun) && (defined(__sparc) || defined(__sparc__))
#include <synch.h>
#define pause() smt_pause()
#elif (defined(__sparc) || defined(__sparc__)) && HAVE_SPARC_PAUSE
#define pause() __asm__ __volatile__("pause")
#elif defined(__ppc__) || defined(_ARCH_PPC) || defined(_ARCH_PWR) || defined(_ARCH_PWR2) || defined(_POWER)
#define pause() __asm__ volatile("or 27,27,27")
#else
#define pause() sched_yield()
#endif

#define pause_n(iters)                                         \
	for (size_t __pause = 0; __pause < iters; __pause++) { \
		pause();                                       \
	}
