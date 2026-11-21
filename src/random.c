#include "quad/random.h"

#include <string.h>

/* Philox4x64-10, as in Random123 (Salmon, Moraes, Dror, Shaw, "Parallel
 * random numbers: as easy as 1, 2, 3", SC 2011). */

__extension__ typedef unsigned __int128 u128;

#define PHILOX_M0 0xD2E7470EE14C6C93ULL
#define PHILOX_M1 0xCA5A826395121157ULL
#define PHILOX_W0 0x9E3779B97F4A7C15ULL   /* golden ratio */
#define PHILOX_W1 0xBB67AE8584CAA73BULL   /* sqrt(3) - 1 */

static inline uint64_t mulhilo(uint64_t a, uint64_t b, uint64_t *hi)
{
    u128 p = (u128) a * b;
    *hi = (uint64_t) (p >> 64);
    return (uint64_t) p;
}

void qr_philox4x64(const uint64_t ctr[4], const uint64_t key[2], uint64_t out[4])
{
    uint64_t x0 = ctr[0], x1 = ctr[1], x2 = ctr[2], x3 = ctr[3];
    uint64_t k0 = key[0], k1 = key[1];
    for (int r = 0; r < 10; r++) {
        if (r > 0) {
            k0 += PHILOX_W0;
            k1 += PHILOX_W1;
        }
        uint64_t hi0, hi1;
        uint64_t lo0 = mulhilo(PHILOX_M0, x0, &hi0);
        uint64_t lo1 = mulhilo(PHILOX_M1, x2, &hi1);
        x0 = hi1 ^ x1 ^ k0;
        x1 = lo1;
        x2 = hi0 ^ x3 ^ k1;
        x3 = lo0;
    }
    out[0] = x0;
    out[1] = x1;
    out[2] = x2;
    out[3] = x3;
}

uint64_t qr_name_hash(const char *name, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char) name[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

QRStream qr_stream(uint64_t seed, uint64_t phenomenon, const char *name)
{
    QRStream s;
    s.key[0] = seed;
    s.key[1] = qr_name_hash(name, strlen(name));
    s.phenomenon = phenomenon;
    return s;
}

static void block(const QRStream *s, uint64_t event, uint64_t b, uint64_t out[4])
{
    uint64_t ctr[4] = { event, b, s->phenomenon, 0 };
    qr_philox4x64(ctr, s->key, out);
}

uint64_t qr_u64(const QRStream *s, uint64_t event, uint64_t draw)
{
    uint64_t out[4];
    block(s, event, draw / 4, out);
    return out[draw % 4];
}

QRCursor qr_cursor(const QRStream *s, uint64_t event)
{
    QRCursor c;
    memset(&c, 0, sizeof c);
    c.s = *s;
    c.event = event;
    c.block = UINT64_MAX;
    return c;
}

uint64_t qr_next(QRCursor *c)
{
    uint64_t b = c->next / 4;
    if (b != c->block) {
        block(&c->s, c->event, b, c->buf);
        c->block = b;
    }
    return c->buf[c->next++ % 4];
}

uint64_t qr_below(QRCursor *c, uint64_t n)
{
    if (n == 0) return qr_next(c);
    u128 m = (u128) qr_next(c) * n;
    uint64_t low = (uint64_t) m;
    if (low < n) {
        uint64_t threshold = (0 - n) % n;   /* 2^64 mod n */
        while (low < threshold) {
            m = (u128) qr_next(c) * n;
            low = (uint64_t) m;
        }
    }
    return (uint64_t) (m >> 64);
}

int64_t qr_range(QRCursor *c, int64_t lo, int64_t hi)
{
    uint64_t span = (uint64_t) hi - (uint64_t) lo + 1;   /* 0 means the full range */
    return (int64_t) ((uint64_t) lo + qr_below(c, span));
}

bool qr_chance(QRCursor *c, int64_t p, int scale)
{
    if (scale > 0) scale = 0;
    if (scale < -18) scale = -18;
    uint64_t one = 1;
    for (int i = 0; i < -scale; i++) one *= 10;
    /* Always use a draw, also for p = 0 and p = 1, so that a change in p does
     * not shift the later draws of the event. */
    uint64_t u = qr_below(c, one);
    return p > 0 && u < (uint64_t) p;
}

double qr_double(QRCursor *c)
{
    return (double) (qr_next(c) >> 11) * 0x1.0p-53;
}
