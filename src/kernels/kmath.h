#ifndef QUAD_KMATH_H
#define QUAD_KMATH_H

/* Integer helpers for the native kernels. A kernel that uses only these
 * helpers can claim QK_PORTABLE. */

#include <stdbool.h>
#include <stdint.h>

#include "quad/kernel.h"

__extension__ typedef __int128 km_i128;
__extension__ typedef unsigned __int128 km_u128;

#define KM_I128_MAX ((km_i128) (((km_u128) 1 << 127) - 1))

static inline km_i128 km_pow10(int k)
{
    km_i128 p = 1;
    while (k-- > 0) p *= 10;
    return p;
}

static inline km_i128 km_abs(km_i128 v) { return v < 0 ? -v : v; }

/* The midpoint of a finite interval, rounded toward negative infinity */
static inline int64_t km_mid(QInterval v)
{
    km_i128 s = (km_i128) v.lo + v.hi, m = s / 2;
    if (s < 0 && s % 2) m -= 1;
    return (int64_t) m;
}

/* n × 10^e / d rounded down, for n >= 0 and d > 0. Saturates at INT64_MAX. */
static inline int64_t km_scaled_floor(km_i128 n, int e, km_i128 d)
{
    km_i128 q;
    if (e >= 0) {
        if (e > 38 || n > KM_I128_MAX / km_pow10(e)) return INT64_MAX;
        q = n * km_pow10(e) / d;
    } else {
        if (-e > 38 || d > KM_I128_MAX / km_pow10(-e)) return 0;
        q = n / (d * km_pow10(-e));
    }
    return q > INT64_MAX ? INT64_MAX : (int64_t) q;
}

/* n × 10^e, rounded half to even. False if the result is not a finite
 * quantity (ADR 0001). */
static inline bool km_scale_half_even(km_i128 n, int e, int64_t *out)
{
    km_i128 q;
    if (e >= 0) {
        if (e > 38 || km_abs(n) > KM_I128_MAX / km_pow10(e)) return false;
        q = n * km_pow10(e);
    } else if (-e > 38) {
        q = 0;
    } else {
        km_i128 d = km_pow10(-e);
        q = n / d;
        km_i128 r = km_abs(n % d);
        if (r > d - r || (r != 0 && r == d - r && (q & 1))) q += n < 0 ? -1 : 1;
    }
    if (q > Q_FMAX || q < Q_FMIN) return false;
    *out = (int64_t) q;
    return true;
}

/* a × b × c, or false if it does not fit an i128 */
static inline bool km_mul3(km_i128 a, km_i128 b, km_i128 c, km_i128 *out)
{
    km_i128 ab = a * b;   /* callers pass int64 values for a and b */
    if (ab != 0 && km_abs(c) > KM_I128_MAX / km_abs(ab)) return false;
    *out = ab * c;
    return true;
}

/* now + step, kept below QK_NO_TIME, with a step of at least one tick */
static inline int64_t km_horizon(int64_t now, int64_t step)
{
    if (step < 1) step = 1;
    return step >= QK_NO_TIME - now ? QK_NO_TIME - 1 : now + step;
}

#endif
