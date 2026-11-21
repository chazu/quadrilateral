#include <string.h>

#include "quad/quantity.h"
#include "test.h"

__extension__ typedef __int128 i128;

static QConv conv(int k, QRound r) { return (QConv) { (int16_t) k, (uint8_t) r }; }
static QInterval iv(int64_t lo, int64_t hi) { return (QInterval) { lo, hi }; }

static QDim dim3(int l, int m, int t)
{
    QDim d = qdim_none();
    d.exp[Q_BASE_LENGTH] = (int8_t) l;
    d.exp[Q_BASE_MASS] = (int8_t) m;
    d.exp[Q_BASE_TIME] = (int8_t) t;
    return d;
}

#define CHECK_IV(r, l, h)          \
    do {                           \
        QInterval r_ = (r);        \
        CHECK_I64(r_.lo, (l));     \
        CHECK_I64(r_.hi, (h));     \
    } while (0)

static i128 p10(int k)
{
    i128 p = 1;
    while (k-- > 0) p *= 10;
    return p;
}

/* --- types ------------------------------------------------------------------ */

void test_status_strings(void)
{
    for (int s = Q_OK; s <= Q_EWIDTH; s++) CHECK(q_status_str((QStatus) s) != NULL);
    CHECK(strstr(q_status_str(Q_EWIDTH), "128") != NULL);
}

void test_dims(void)
{
    QDim len = qdim_base(Q_BASE_LENGTH), mass = qdim_base(Q_BASE_MASS), time = qdim_base(Q_BASE_TIME);
    QDim area, energy, power, tmp;
    CHECK(qdim_mul(&len, &len, &area) == Q_OK);
    CHECK(qdim_mul(&area, &mass, &tmp) == Q_OK);
    CHECK(qdim_div(&tmp, &time, &tmp) == Q_OK);
    CHECK(qdim_div(&tmp, &time, &energy) == Q_OK);
    QDim want = dim3(2, 1, -2);
    CHECK(qdim_eq(&energy, &want));
    CHECK(qdim_div(&energy, &time, &power) == Q_OK);
    want = dim3(2, 1, -3);
    CHECK(qdim_eq(&power, &want));

    QDim none = qdim_none(), out;
    CHECK(qdim_is_none(&none));
    CHECK(!qdim_is_none(&len));
    QDim bad = qdim_base(Q_MAXBASE + 3);
    CHECK(qdim_is_none(&bad));

    QDim big = qdim_none();
    big.exp[Q_NSI] = 100;   /* a declared base dimension */
    CHECK(qdim_mul(&big, &big, &out) == Q_EDIM);
    CHECK(qdim_div(&big, &big, &out) == Q_OK);
    CHECK(qdim_is_none(&out));
}

void test_default_scales(void)
{
    QDim none = qdim_none(), energy = dim3(2, 1, -2);
    QDim len = qdim_base(Q_BASE_LENGTH), time = qdim_base(Q_BASE_TIME);
    QDim temp = qdim_base(Q_BASE_TEMPERATURE), mass = qdim_base(Q_BASE_MASS);
    QDim currency = qdim_base(Q_NSI);
    CHECK_I64(qdim_default_scale(&none, -6), -9);
    CHECK_I64(qdim_default_scale(&len, -6), -4);
    CHECK_I64(qdim_default_scale(&time, -6), -6);
    CHECK_I64(qdim_default_scale(&time, 3), 3);
    CHECK_I64(qdim_default_scale(&temp, -6), -2);
    CHECK_I64(qdim_default_scale(&mass, -6), -3);
    CHECK_I64(qdim_default_scale(&energy, -6), -3);
    CHECK_I64(qdim_default_scale(&currency, -6), -3);
}

void test_type_results(void)
{
    QType len = qtype_make(1, qdim_base(Q_BASE_LENGTH), -4);
    QType t;
    CHECK(qtype_mul(&len, &len, &t) == Q_OK);
    CHECK_I64(t.kind, 0);
    CHECK_I64(t.scale, -8);
    CHECK_I64(t.dim.exp[Q_BASE_LENGTH], 2);
    CHECK(qtype_div(&len, &len, &t) == Q_OK);
    CHECK(qdim_is_none(&t.dim));
    CHECK_I64(t.scale, 0);

    QType a = qtype_make(1, qdim_base(Q_BASE_LENGTH), -3), b = qtype_make(1, qdim_base(Q_BASE_LENGTH), -4);
    CHECK(qtype_sum(&a, &b, &t) == Q_OK);
    CHECK_I64(t.kind, 1);
    CHECK_I64(t.scale, -4);
    QType anon = qtype_make(0, qdim_base(Q_BASE_LENGTH), -2);
    CHECK(qtype_sum(&anon, &a, &t) == Q_OK);
    CHECK_I64(t.kind, 1);
    QType other = qtype_make(2, qdim_base(Q_BASE_LENGTH), -3);
    CHECK(qtype_sum(&a, &other, &t) == Q_EKIND);
    QType mass = qtype_make(3, qdim_base(Q_BASE_MASS), -3);
    CHECK(qtype_sum(&a, &mass, &t) == Q_EDIM);

    QType wide = a;
    wide.width = 128;
    CHECK(qtype_mul(&wide, &a, &t) == Q_EWIDTH);
    CHECK(qtype_sum(&a, &wide, &t) == Q_EWIDTH);

    QType huge = qtype_make(0, qdim_none(), 100);
    CHECK(qtype_mul(&huge, &huge, &t) == Q_ESCALE);
    QType small = qtype_make(0, qdim_none(), -100);
    CHECK(qtype_div(&small, &huge, &t) == Q_ESCALE);
}

void test_plans(void)
{
    QType mm = qtype_make(1, qdim_base(Q_BASE_LENGTH), -4), m3 = qtype_make(1, qdim_base(Q_BASE_LENGTH), -3);
    m3.rounding = Q_ROUND_FLOOR;
    QConv c;
    CHECK(qtype_conv_plan(&mm, &m3, &c) == Q_OK);
    CHECK_I64(c.k, -1);
    CHECK_I64(c.rounding, Q_ROUND_FLOOR);

    QType other = qtype_make(2, qdim_base(Q_BASE_LENGTH), -4);
    CHECK(qtype_conv_plan(&other, &mm, &c) == Q_EKIND);
    QType anon = qtype_make(0, qdim_base(Q_BASE_LENGTH), -6);
    CHECK(qtype_conv_plan(&anon, &mm, &c) == Q_OK);
    CHECK_I64(c.k, -2);
    QType mass = qtype_make(1, qdim_base(Q_BASE_MASS), -4);
    CHECK(qtype_conv_plan(&mass, &mm, &c) == Q_EDIM);

    QType speed = qtype_make(4, dim3(1, 0, -1), -3), time = qtype_make(5, qdim_base(Q_BASE_TIME), -6);
    CHECK(qtype_div_plan(&mm, &speed, &time, &c) == Q_OK);
    CHECK_I64(c.k, 5);
    CHECK(qtype_mul_plan(&speed, &time, &mm, &c) == Q_OK);
    CHECK_I64(c.k, -5);
    CHECK(qtype_mul_plan(&speed, &speed, &mm, &c) == Q_EDIM);
}

/* --- intervals ------------------------------------------------------------------ */

void test_interval_basics(void)
{
    CHECK(qi_valid(qi_top()));
    CHECK(qi_valid(qi_point(Q_FMAX)));
    CHECK(qi_valid(qi_point(Q_FMIN)));
    CHECK(!qi_valid(iv(1, 0)));
    CHECK(!qi_valid(iv(Q_INF, Q_INF)));
    CHECK(!qi_valid(iv(Q_NINF, Q_NINF)));
    CHECK(!qi_valid(iv(INT64_MIN + 1, 0)));

    QInterval r;
    CHECK(qi_meet(iv(1, 5), iv(3, 8), &r));
    CHECK_IV(r, 3, 5);
    CHECK(!qi_meet(iv(1, 2), iv(3, 4), &r));
    CHECK(qi_meet(iv(1, 2), qi_top(), &r));
    CHECK_IV(r, 1, 2);

    CHECK(qi_subset(iv(2, 3), iv(1, 5)));
    CHECK(!qi_subset(iv(0, 3), iv(1, 5)));
    CHECK(qi_contains(iv(1, 5), 5));
    CHECK(!qi_contains(iv(1, 5), 6));
    CHECK(qi_is_point(qi_point(4)));
    CHECK(!qi_is_point(iv(4, 5)));
}

void test_interval_add_neg(void)
{
    CHECK_IV(qi_add(iv(1, 2), iv(10, 20)), 11, 22);
    CHECK_IV(qi_sub(iv(1, 2), iv(10, 20)), -19, -8);
    CHECK_IV(qi_neg(iv(Q_FMIN, Q_FMAX)), Q_FMIN, Q_FMAX);
    CHECK_IV(qi_neg(iv(-3, 7)), -7, 3);
}

void test_interval_infinities(void)
{
    QConv c0 = conv(0, Q_ROUND_HALF_EVEN);
    CHECK_IV(qi_mul(iv(1, Q_INF), iv(2, 3), &c0), 2, Q_INF);
    CHECK_IV(qi_mul(iv(0, 0), qi_top(), &c0), 0, 0);
    CHECK_IV(qi_mul(iv(Q_NINF, -1), iv(Q_NINF, -1), &c0), 1, Q_INF);
    CHECK_IV(qi_mul(iv(-2, 3), qi_top(), &c0), Q_NINF, Q_INF);
    CHECK_IV(qi_div(iv(1, 2), iv(-1, 1), &c0), Q_NINF, Q_INF);
    CHECK_IV(qi_div(iv(1, 2), iv(0, 0), &c0), Q_NINF, Q_INF);
    CHECK_IV(qi_div(iv(1, Q_INF), iv(1, Q_INF), &c0), 0, Q_INF);
    CHECK_IV(qi_div(iv(-5, -5), iv(1, Q_INF), &c0), -5, 0);
    CHECK_IV(qi_div(iv(6, 6), iv(-3, -2), &c0), -3, -2);
    CHECK_IV(qi_add(iv(Q_NINF, 3), iv(1, Q_INF)), Q_NINF, Q_INF);
    CHECK_IV(qi_neg(qi_top()), Q_NINF, Q_INF);
    CHECK_IV(qi_conv(iv(Q_NINF, 5), &(QConv) { 3, 0 }), Q_NINF, 5000);
}

void test_interval_overflow(void)
{
    CHECK_IV(qi_conv(iv(0, Q_FMAX), &(QConv) { 1, 0 }), 0, Q_INF);
    CHECK_IV(qi_conv(qi_point(Q_FMAX), &(QConv) { 1, 0 }), Q_FMAX, Q_INF);
    CHECK_IV(qi_conv(qi_point(Q_FMIN), &(QConv) { 1, 0 }), Q_NINF, Q_FMIN);
    CHECK_IV(qi_add(qi_point(Q_FMAX), qi_point(1)), Q_FMAX, Q_INF);
    CHECK_IV(qi_conv(qi_point(1), &(QConv) { 50, 0 }), Q_FMAX, Q_INF);
    CHECK_IV(qi_conv(qi_point(5), &(QConv) { -50, 0 }), 0, 1);
    CHECK_IV(qi_conv(qi_point(-5), &(QConv) { -50, 0 }), -1, 0);
    CHECK_IV(qi_conv(qi_point(15), &(QConv) { -1, 0 }), 1, 2);
    CHECK_IV(qi_conv(qi_point(-15), &(QConv) { -1, 0 }), -2, -1);
    QConv c0 = conv(0, Q_ROUND_HALF_EVEN);
    CHECK_IV(qi_mul(qi_point(Q_FMAX), qi_point(2), &c0), Q_FMAX, Q_INF);
    CHECK_IV(qi_mul(qi_point(Q_FMAX), qi_point(-2), &c0), Q_NINF, Q_FMIN);
}

/* --- points ---------------------------------------------------------------------- */

static int64_t pconv(int64_t v, int k, QRound r)
{
    int64_t out = 12345;
    QConv c = conv(k, r);
    CHECK(qp_conv(v, &c, &out) == Q_OK);
    return out;
}

static int64_t pdiv(int64_t a, int64_t b, int k, QRound r)
{
    int64_t out = 12345;
    QConv c = conv(k, r);
    CHECK(qp_div(a, b, &c, &out) == Q_OK);
    return out;
}

void test_point_rounding(void)
{
    CHECK_I64(pconv(25, -1, Q_ROUND_HALF_EVEN), 2);
    CHECK_I64(pconv(35, -1, Q_ROUND_HALF_EVEN), 4);
    CHECK_I64(pconv(-25, -1, Q_ROUND_HALF_EVEN), -2);
    CHECK_I64(pconv(-35, -1, Q_ROUND_HALF_EVEN), -4);
    CHECK_I64(pconv(26, -1, Q_ROUND_HALF_EVEN), 3);
    CHECK_I64(pconv(24, -1, Q_ROUND_HALF_EVEN), 2);
    CHECK_I64(pconv(-25, -1, Q_ROUND_FLOOR), -3);
    CHECK_I64(pconv(25, -1, Q_ROUND_FLOOR), 2);
    CHECK_I64(pconv(25, -1, Q_ROUND_CEIL), 3);
    CHECK_I64(pconv(-25, -1, Q_ROUND_CEIL), -2);
    CHECK_I64(pconv(-25, -1, Q_ROUND_ZERO), -2);
    CHECK_I64(pconv(25, -1, Q_ROUND_ZERO), 2);
    CHECK_I64(pconv(7, 3, Q_ROUND_HALF_EVEN), 7000);

    CHECK_I64(pconv(5, -50, Q_ROUND_CEIL), 1);
    CHECK_I64(pconv(5, -50, Q_ROUND_HALF_EVEN), 0);
    CHECK_I64(pconv(5, -50, Q_ROUND_FLOOR), 0);
    CHECK_I64(pconv(-5, -50, Q_ROUND_FLOOR), -1);

    CHECK_I64(pdiv(1, 3, 0, Q_ROUND_HALF_EVEN), 0);
    CHECK_I64(pdiv(2, 3, 0, Q_ROUND_HALF_EVEN), 1);
    CHECK_I64(pdiv(-2, 3, 0, Q_ROUND_HALF_EVEN), -1);
    CHECK_I64(pdiv(-1, 3, 0, Q_ROUND_FLOOR), -1);
    CHECK_I64(pdiv(1, 3, 0, Q_ROUND_CEIL), 1);
    CHECK_I64(pdiv(1, 2, 0, Q_ROUND_HALF_EVEN), 0);
    CHECK_I64(pdiv(3, 2, 0, Q_ROUND_HALF_EVEN), 2);
    CHECK_I64(pdiv(5, 2, 0, Q_ROUND_HALF_EVEN), 2);
    CHECK_I64(pdiv(-3, 2, 0, Q_ROUND_HALF_EVEN), -2);
    CHECK_I64(pdiv(3, -2, 0, Q_ROUND_FLOOR), -2);
    CHECK_I64(pdiv(1, 1, -40, Q_ROUND_HALF_EVEN), 0);
    CHECK_I64(pdiv(1, 1, -40, Q_ROUND_CEIL), 1);
    CHECK_I64(pdiv(-1, 1, -40, Q_ROUND_FLOOR), -1);
    CHECK_I64(pdiv(1, 3, 2, Q_ROUND_HALF_EVEN), 33);
    CHECK_I64(pdiv(2, 3, 2, Q_ROUND_HALF_EVEN), 67);
}

void test_point_errors(void)
{
    int64_t out;
    QConv c1 = conv(1, Q_ROUND_HALF_EVEN), c0 = conv(0, Q_ROUND_HALF_EVEN);
    CHECK(qp_conv(Q_FMAX, &c1, &out) == Q_EOVERFLOW);
    CHECK(qp_add(Q_FMAX, 1, &out) == Q_EOVERFLOW);
    CHECK(qp_sub(Q_FMIN, 1, &out) == Q_EOVERFLOW);
    CHECK(qp_add(Q_FMAX, 0, &out) == Q_OK);
    CHECK(qp_mul(Q_INF, 1, &c0, &out) == Q_EINFINITE);
    CHECK(qp_conv(Q_NINF, &c0, &out) == Q_EINFINITE);
    CHECK(qp_div(1, 0, &c0, &out) == Q_EDIVZERO);
    CHECK(qp_add(INT64_MIN + 1, 0, &out) == Q_EOVERFLOW);
    CHECK(qp_mul(10000000000LL, 10000000000LL, &c0, &out) == Q_EOVERFLOW);
    CHECK(qp_mul(10000000000LL, 10000000000LL, &(QConv) { -2, 0 }, &out) == Q_OK);
    CHECK_I64(out, 1000000000000000000LL);
    CHECK(qp_conv(1, &(QConv) { 40, 0 }, &out) == Q_EOVERFLOW);
    CHECK(qp_div(Q_FMAX, 1, &(QConv) { 30, 0 }, &out) == Q_EOVERFLOW);
}

/* --- decimal text ------------------------------------------------------------------ */

static QStatus parse(const char *s, int scale, int64_t *out) { return q_parse_decimal(s, strlen(s), scale, out); }

void test_parse_decimal(void)
{
    int64_t v;
    CHECK(parse("1500", -3, &v) == Q_OK);
    CHECK_I64(v, 1500000);
    CHECK(parse("0.15", -4, &v) == Q_OK);
    CHECK_I64(v, 1500);
    CHECK(parse("0.00015", -4, &v) == Q_EINEXACT);
    CHECK(parse("1.5e3", -3, &v) == Q_OK);
    CHECK_I64(v, 1500000);
    CHECK(parse("-0.25", -2, &v) == Q_OK);
    CHECK_I64(v, -25);
    CHECK(parse("+7", 0, &v) == Q_OK);
    CHECK_I64(v, 7);
    CHECK(parse("2.50", -1, &v) == Q_OK);
    CHECK_I64(v, 25);
    CHECK(parse("1e-3", -3, &v) == Q_OK);
    CHECK_I64(v, 1);
    CHECK(parse("1E+2", 0, &v) == Q_OK);
    CHECK_I64(v, 100);
    CHECK(parse("0", 5, &v) == Q_OK);
    CHECK_I64(v, 0);
    CHECK(parse("0.0000", 0, &v) == Q_OK);
    CHECK_I64(v, 0);
    CHECK(parse("5", 1, &v) == Q_EINEXACT);
    CHECK(parse("50", 1, &v) == Q_OK);
    CHECK_I64(v, 5);
    CHECK(parse("12.", 0, &v) == Q_OK);
    CHECK_I64(v, 12);
    CHECK(parse(".5", -1, &v) == Q_OK);
    CHECK_I64(v, 5);

    const char *bad[] = { "", ".", "1.2.3", "1e", "abc", "1x", "-", "1e+", "--1", "1 " };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(parse(bad[i], 0, &v) == Q_ESYNTAX);

    CHECK(parse("9223372036854775806", 0, &v) == Q_OK);
    CHECK_I64(v, Q_FMAX);
    CHECK(parse("-9223372036854775806", 0, &v) == Q_OK);
    CHECK_I64(v, Q_FMIN);
    CHECK(parse("9223372036854775807", 0, &v) == Q_EOVERFLOW);
    CHECK(parse("1e30", 0, &v) == Q_EOVERFLOW);
    CHECK(parse("1234567890123456789012345678901234567890", 0, &v) == Q_EOVERFLOW);
    CHECK(parse("1e99999", 0, &v) == Q_EOVERFLOW);
}

static const char *fmt(int64_t q, int scale)
{
    static char buf[256];
    q_format_decimal(q, scale, buf, sizeof buf);
    return buf;
}

void test_format_decimal(void)
{
    CHECK(!strcmp(fmt(-25, -2), "-0.25"));
    CHECK(!strcmp(fmt(1500000, -3), "1500"));
    CHECK(!strcmp(fmt(5, 2), "500"));
    CHECK(!strcmp(fmt(0, -3), "0"));
    CHECK(!strcmp(fmt(0, 3), "0"));
    CHECK(!strcmp(fmt(Q_INF, -3), "inf"));
    CHECK(!strcmp(fmt(Q_NINF, -3), "-inf"));
    CHECK(!strcmp(fmt(1, -4), "0.0001"));
    CHECK(!strcmp(fmt(123456, -3), "123.456"));
    CHECK(!strcmp(fmt(-5, -1), "-0.5"));
    CHECK(!strcmp(fmt(120, -2), "1.2"));
    CHECK(!strcmp(fmt(Q_FMIN, 0), "-9223372036854775806"));
    CHECK(!strcmp(fmt(7, 0), "7"));

    char small[2];
    CHECK_I64((int64_t) q_format_decimal(1500000, -3, small, sizeof small), 4);
    CHECK(!strcmp(small, "1"));

    /* formatting and parsing again gives the same quanta */
    uint64_t st = 7;
    for (int i = 0; i < 2000; i++) {
        int64_t q = t_range(&st, -1000000000000LL, 1000000000000LL);
        int scale = (int) t_range(&st, -12, 6);
        char buf[128];
        q_format_decimal(q, scale, buf, sizeof buf);
        int64_t back;
        CHECK(q_parse_decimal(buf, strlen(buf), scale, &back) == Q_OK);
        CHECK_I64(back, q);
    }
}

/* --- the examples in quadrilateral-design.md -------------------------------------------- */

void test_design_examples(void)
{
    /* Section 8.1: distance 100..200 m and speed 10..20 m/s give time 5..20 s. */
    QType len = qtype_make(1, qdim_base(Q_BASE_LENGTH), -4);
    QType speed = qtype_make(2, dim3(1, 0, -1), -3);
    QType time = qtype_make(3, qdim_base(Q_BASE_TIME), -6);
    QConv c;
    CHECK(qtype_div_plan(&len, &speed, &time, &c) == Q_OK);
    QInterval d = iv(100 * 10000, 200 * 10000), v = iv(10 * 1000, 20 * 1000);
    CHECK_IV(qi_div(d, v, &c), 5000000, 20000000);
    /* distance becomes 150 m: time narrows to 7.5..15 s */
    CHECK_IV(qi_div(qi_point(150 * 10000), v, &c), 7500000, 15000000);

    /* Section 9.4: 10 W/K across 28..32 °C and 19..21 °C gives 70..130 W. */
    QType temp = qtype_make(4, qdim_base(Q_BASE_TEMPERATURE), -2);
    QDim cond_dim = dim3(2, 1, -3);
    cond_dim.exp[Q_BASE_TEMPERATURE] = -1;
    QType cond = qtype_make(5, cond_dim, -3);
    QType power = qtype_make(6, dim3(2, 1, -3), -3);
    QType energy = qtype_make(7, dim3(2, 1, -2), -3);
    int64_t t28, t32, t19, t21;
    CHECK(parse("301.15", -2, &t28) == Q_OK);
    CHECK(parse("305.15", -2, &t32) == Q_OK);
    CHECK(parse("292.15", -2, &t19) == Q_OK);
    CHECK(parse("294.15", -2, &t21) == Q_OK);
    QInterval dt = qi_sub(iv(t28, t32), iv(t19, t21));
    CHECK_IV(dt, 700, 1300);
    CHECK(qtype_mul_plan(&cond, &temp, &power, &c) == Q_OK);
    QInterval p = qi_mul(qi_point(10000), dt, &c);
    CHECK_IV(p, 70000, 130000);
    CHECK(!strcmp(fmt(p.lo, power.scale), "70"));

    /* Section 9.2: 100 W for 60 s transfers 6000 J. */
    CHECK(qtype_mul_plan(&power, &time, &energy, &c) == Q_OK);
    int64_t e;
    CHECK(qp_mul(100000, 60000000, &c, &e) == Q_OK);
    CHECK_I64(e, 6000000);
    CHECK(!strcmp(fmt(e, energy.scale), "6000"));
}

/* --- properties ---------------------------------------------------------------------- */

#define LIM (1LL << 30)

/* A random interval in about [-LIM, LIM], sometimes with an infinite bound. */
static QInterval rand_iv(uint64_t *st, bool allow_inf)
{
    int64_t a = t_range(st, -LIM, LIM), b = t_range(st, -LIM, LIM);
    if (t_rand(st) % 4 == 0) b = a;   /* a point */
    QInterval r = iv(a < b ? a : b, a < b ? b : a);
    if (allow_inf && t_rand(st) % 8 == 0) r.lo = Q_NINF;
    if (allow_inf && t_rand(st) % 8 == 0) r.hi = Q_INF;
    return r;
}

/* A random member of a, from the finite part */
static int64_t sample(uint64_t *st, QInterval a)
{
    int64_t lo = a.lo == Q_NINF ? -LIM : a.lo, hi = a.hi == Q_INF ? LIM : a.hi;
    if (lo > hi) lo = hi;   /* (-inf, b] with b < -LIM, or the mirror */
    switch (t_rand(st) % 3) {
    case 0: return lo;
    case 1: return hi;
    default: return t_range(st, lo, hi);
    }
}

/* lo <= n / d <= hi, exactly, for d != 0 */
static bool encloses(QInterval r, i128 n, i128 d)
{
    if (d < 0) { n = -n; d = -d; }
    if (r.lo != Q_NINF && (i128) r.lo * d > n) return false;
    if (r.hi != Q_INF && (i128) r.hi * d < n) return false;
    return true;
}

void test_prop_mul(void)
{
    uint64_t st = 1;
    for (int i = 0; i < 20000; i++) {
        QInterval a = rand_iv(&st, true), b = rand_iv(&st, true);
        int k = (int) t_range(&st, -8, 8);
        QConv c = conv(k, Q_ROUND_HALF_EVEN);
        QInterval r = qi_mul(a, b, &c);
        CHECK(qi_valid(r));
        for (int s = 0; s < 4; s++) {
            i128 xy = (i128) sample(&st, a) * sample(&st, b);
            bool ok = k >= 0 ? encloses(r, xy * p10(k), 1) : encloses(r, xy, p10(-k));
            CHECK(ok);
            if (!ok) return;
        }
        if (qi_is_point(a) && qi_is_point(b) && r.lo != Q_NINF && r.hi != Q_INF && r.hi < Q_FMAX)
            CHECK(r.hi - r.lo <= 1);
    }
}

void test_prop_div(void)
{
    uint64_t st = 2;
    for (int i = 0; i < 20000; i++) {
        QInterval a = rand_iv(&st, true);
        int64_t y0 = t_range(&st, 1, LIM), y1 = t_range(&st, 1, LIM);
        QInterval b = iv(y0 < y1 ? y0 : y1, y0 < y1 ? y1 : y0);
        if (t_rand(&st) % 8 == 0) b.hi = Q_INF;
        if (t_rand(&st) % 2) b = qi_neg(b);
        int k = (int) t_range(&st, -8, 8);
        QConv c = conv(k, Q_ROUND_HALF_EVEN);
        QInterval r = qi_div(a, b, &c);
        CHECK(qi_valid(r));
        for (int s = 0; s < 4; s++) {
            i128 x = sample(&st, a), y = sample(&st, b);
            bool ok = k >= 0 ? encloses(r, x * p10(k), y) : encloses(r, x, y * p10(-k));
            CHECK(ok);
            if (!ok) return;
        }
        if (qi_is_point(a) && qi_is_point(b) && r.lo != Q_NINF && r.hi != Q_INF)
            CHECK(r.hi - r.lo <= 1);
    }
}

void test_prop_conv_add(void)
{
    uint64_t st = 3;
    for (int i = 0; i < 20000; i++) {
        QInterval a = rand_iv(&st, true), b = rand_iv(&st, true);
        int k = (int) t_range(&st, -12, 12);
        QConv c = conv(k, Q_ROUND_HALF_EVEN);
        QInterval r = qi_conv(a, &c), s = qi_add(a, b);
        CHECK(qi_valid(r));
        CHECK(qi_valid(s));
        i128 x = sample(&st, a), y = sample(&st, b);
        CHECK(k >= 0 ? encloses(r, x * p10(k), 1) : encloses(r, x, p10(-k)));
        CHECK(encloses(s, x + y, 1));
        if (qi_is_point(a) && qi_is_point(b)) CHECK_I64(s.hi - s.lo, 0);
    }
}

/* A point result lies in the interval result for the same operands. */
void test_prop_point_in_interval(void)
{
    uint64_t st = 4;
    for (int i = 0; i < 20000; i++) {
        int64_t x = t_range(&st, -LIM, LIM), y = t_range(&st, -LIM, LIM);
        int k = (int) t_range(&st, -10, 6);
        QConv c = conv(k, (QRound) (t_rand(&st) % 4));
        int64_t p;
        if (qp_mul(x, y, &c, &p) == Q_OK) CHECK(qi_contains(qi_mul(qi_point(x), qi_point(y), &c), p));
        if (y != 0 && qp_div(x, y, &c, &p) == Q_OK) CHECK(qi_contains(qi_div(qi_point(x), qi_point(y), &c), p));
        if (qp_conv(x, &c, &p) == Q_OK) CHECK(qi_contains(qi_conv(qi_point(x), &c), p));
    }
}
