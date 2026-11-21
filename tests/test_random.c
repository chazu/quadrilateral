#include <string.h>

#include "quad/random.h"
#include "test.h"

/* Known-answer vectors for Philox4x64-10, from Random123's kat_vectors. */
void test_philox_kat(void)
{
    static const struct { uint64_t ctr[4], key[2], out[4]; } kat[] = {
        { { 0, 0, 0, 0 }, { 0, 0 },
          { 0x16554d9eca36314cULL, 0xdb20fe9d672d0fdcULL, 0xd7e772cee186176bULL, 0x7e68b68aec7ba23bULL } },
        { { ~0ULL, ~0ULL, ~0ULL, ~0ULL }, { ~0ULL, ~0ULL },
          { 0x87b092c3013fe90bULL, 0x438c3c67be8d0224ULL, 0x9cc7d7c69cd777b6ULL, 0xa09caebf594f0ba0ULL } },
        { { 0x243f6a8885a308d3ULL, 0x13198a2e03707344ULL, 0xa4093822299f31d0ULL, 0x082efa98ec4e6c89ULL },
          { 0x452821e638d01377ULL, 0xbe5466cf34e90c6cULL },
          { 0xa528f45403e61d95ULL, 0x38c72dbd566e9788ULL, 0xa5a1610e72fd18b5ULL, 0x57bd43b5e52b7fe6ULL } },
    };
    for (size_t i = 0; i < sizeof kat / sizeof kat[0]; i++) {
        uint64_t out[4];
        qr_philox4x64(kat[i].ctr, kat[i].key, out);
        for (int j = 0; j < 4; j++) CHECK(out[j] == kat[i].out[j]);
    }
}

void test_name_hash(void)
{
    CHECK(qr_name_hash("", 0) == 0xcbf29ce484222325ULL);
    CHECK(qr_name_hash("a", 1) == 0xaf63dc4c8601ec8cULL);
    CHECK(qr_name_hash("ignition", 8) != qr_name_hash("spread", 6));
}

void test_random_determinism(void)
{
    QRStream s = qr_stream(42, 7, "ignition");
    QRStream same = qr_stream(42, 7, "ignition");
    CHECK(qr_u64(&s, 3, 5) == qr_u64(&same, 3, 5));

    /* every input changes the draw */
    uint64_t base = qr_u64(&s, 3, 5);
    QRStream seed = qr_stream(43, 7, "ignition"), phen = qr_stream(42, 8, "ignition"), name = qr_stream(42, 7, "spread");
    CHECK(qr_u64(&seed, 3, 5) != base);
    CHECK(qr_u64(&phen, 3, 5) != base);
    CHECK(qr_u64(&name, 3, 5) != base);
    CHECK(qr_u64(&s, 4, 5) != base);
    CHECK(qr_u64(&s, 3, 6) != base);

    /* a cursor gives the same draws as qr_u64 */
    QRCursor c = qr_cursor(&s, 3);
    for (uint64_t d = 0; d < 11; d++) CHECK(qr_next(&c) == qr_u64(&s, 3, d));
}

/* Draws do not depend on the order in which events or phenomena are visited. */
void test_random_order_independence(void)
{
    enum { NP = 5, NE = 6 };
    uint64_t forward[NP][NE], backward[NP][NE];
    for (int p = 0; p < NP; p++)
        for (int e = 0; e < NE; e++) {
            QRStream s = qr_stream(9, (uint64_t) p, "walk");
            QRCursor c = qr_cursor(&s, (uint64_t) e);
            qr_next(&c);
            forward[p][e] = qr_next(&c);
        }
    for (int e = NE - 1; e >= 0; e--)
        for (int p = NP - 1; p >= 0; p--) {
            QRStream s = qr_stream(9, (uint64_t) p, "walk");
            QRCursor c = qr_cursor(&s, (uint64_t) e);
            qr_next(&c);
            backward[p][e] = qr_next(&c);
        }
    CHECK(memcmp(forward, backward, sizeof forward) == 0);
}

void test_random_bits(void)
{
    QRStream s = qr_stream(1, 1, "bits");
    QRCursor c = qr_cursor(&s, 0);
    enum { N = 100000 };
    int ones[64] = { 0 };
    for (int i = 0; i < N; i++) {
        uint64_t v = qr_next(&c);
        for (int b = 0; b < 64; b++) ones[b] += (int) ((v >> b) & 1);
    }
    for (int b = 0; b < 64; b++) CHECK(ones[b] > N / 2 - 1500 && ones[b] < N / 2 + 1500);
}

void test_random_below(void)
{
    QRStream s = qr_stream(2, 1, "below");
    QRCursor c = qr_cursor(&s, 0);
    enum { N = 100000, K = 10 };
    int count[K] = { 0 };
    for (int i = 0; i < N; i++) {
        uint64_t v = qr_below(&c, K);
        CHECK(v < K);
        if (v < K) count[v]++;
    }
    for (int k = 0; k < K; k++) CHECK(count[k] > N / K - 500 && count[k] < N / K + 500);

    /* a bound where rejection is common: about half the raw draws are rejected */
    uint64_t big = (1ULL << 63) + 1;
    uint64_t before = c.next;
    for (int i = 0; i < 1000; i++) CHECK(qr_below(&c, big) < big);
    CHECK(c.next - before > 1500);

    CHECK(qr_below(&c, 1) == 0);
    uint64_t at = c.next;
    QRCursor copy = c;
    CHECK(qr_below(&c, 0) == qr_next(&copy));
    CHECK(c.next == at + 1);
}

void test_random_range(void)
{
    QRStream s = qr_stream(3, 1, "range");
    QRCursor c = qr_cursor(&s, 0);
    bool seen_lo = false, seen_hi = false;
    for (int i = 0; i < 10000; i++) {
        int64_t v = qr_range(&c, -3, 3);
        CHECK(v >= -3 && v <= 3);
        seen_lo |= v == -3;
        seen_hi |= v == 3;
    }
    CHECK(seen_lo && seen_hi);
    CHECK(qr_range(&c, 5, 5) == 5);
    /* the full range does not overflow */
    for (int i = 0; i < 100; i++) qr_range(&c, INT64_MIN, INT64_MAX);
    int64_t v = qr_range(&c, INT64_MAX - 1, INT64_MAX);
    CHECK(v == INT64_MAX - 1 || v == INT64_MAX);
}

void test_random_chance(void)
{
    QRStream s = qr_stream(4, 1, "chance");
    QRCursor c = qr_cursor(&s, 0);
    enum { N = 100000 };
    int hits = 0;
    for (int i = 0; i < N; i++) hits += qr_chance(&c, 250000000, -9);
    CHECK(hits > N / 4 - 800 && hits < N / 4 + 800);

    for (int i = 0; i < 1000; i++) {
        CHECK(!qr_chance(&c, 0, -9));
        CHECK(qr_chance(&c, 1000000000, -9));
        CHECK(qr_chance(&c, 1, 0));
    }

    /* each call uses one draw (rejection at 10^9 has probability ~5e-11) */
    QRCursor a = qr_cursor(&s, 1), b = qr_cursor(&s, 1);
    qr_chance(&a, 0, -9);
    qr_chance(&b, 1000000000, -9);
    CHECK(a.next == 1 && b.next == 1);
    CHECK(qr_next(&a) == qr_next(&b));
}

void test_random_double(void)
{
    QRStream s = qr_stream(5, 1, "double");
    QRCursor c = qr_cursor(&s, 0);
    double sum = 0;
    enum { N = 100000 };
    for (int i = 0; i < N; i++) {
        double d = qr_double(&c);
        CHECK(d >= 0.0 && d < 1.0);
        sum += d;
    }
    CHECK(sum / N > 0.49 && sum / N < 0.51);
}
