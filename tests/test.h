#ifndef QUAD_TEST_H
#define QUAD_TEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

extern int t_checks, t_failures;

#define CHECK(c)                                                                     \
    do {                                                                             \
        t_checks++;                                                                  \
        if (!(c)) {                                                                  \
            t_failures++;                                                            \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);    \
        }                                                                            \
    } while (0)

#define CHECK_I64(a, b)                                                              \
    do {                                                                             \
        int64_t a_ = (a), b_ = (b);                                                  \
        t_checks++;                                                                  \
        if (a_ != b_) {                                                              \
            t_failures++;                                                            \
            fprintf(stderr, "%s:%d: %s == %" PRId64 ", expected %" PRId64 "\n",      \
                    __FILE__, __LINE__, #a, a_, b_);                                 \
        }                                                                            \
    } while (0)

/* A deterministic generator for property tests (SplitMix64). */
static inline uint64_t t_rand(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* A value in [lo, hi] */
static inline int64_t t_range(uint64_t *state, int64_t lo, int64_t hi)
{
    return lo + (int64_t) (t_rand(state) % (uint64_t) (hi - lo + 1));
}

#endif
