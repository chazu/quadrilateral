#include "quad/quantity.h"

#include <stdio.h>
#include <string.h>

/* Fixed-point interval arithmetic (docs/adr/0001). The outward rounding and
 * the rule for infinite bounds follow Triangle's arith.c. */

__extension__ typedef __int128 i128;
__extension__ typedef unsigned __int128 u128;

#define I128_MAX ((i128) (((u128) 1 << 127) - 1))

const char *q_status_str(QStatus s)
{
    switch (s) {
    case Q_OK:        return "ok";
    case Q_EOVERFLOW: return "value out of range";
    case Q_EINFINITE: return "infinite value in a point operation";
    case Q_EDIVZERO:  return "division by zero";
    case Q_EINEXACT:  return "not a whole number of quanta";
    case Q_ESYNTAX:   return "malformed number";
    case Q_EDIM:      return "dimension mismatch";
    case Q_ESCALE:    return "scale out of range";
    case Q_EKIND:     return "kind mismatch";
    case Q_EWIDTH:    return "width 128 is not implemented";
    }
    return "unknown status";
}

/* --- integer helpers ---------------------------------------------------------- */

static const uint64_t P10[20] = {
    1ULL, 10ULL, 100ULL, 1000ULL,
    10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
    100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL,
    1000000000000ULL, 10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
    10000000000000000ULL, 100000000000000000ULL, 1000000000000000000ULL, 10000000000000000000ULL,
};

/* 10^k for 0 <= k <= 38, the largest power of ten in an i128 */
static i128 pow10_(int k)
{
    if (k < 20) return (i128) P10[k];
    return (i128) P10[19] * (i128) P10[k - 19];
}

static i128 abs128(i128 v) { return v < 0 ? -v : v; }
static int  sgn128(i128 v) { return (v > 0) - (v < 0); }

/* n / d rounded by `mode`; d != 0 */
static i128 divround(i128 n, i128 d, QRound mode)
{
    i128 q = n / d, r = n % d;
    if (r == 0) return q;
    int s = ((n < 0) != (d < 0)) ? -1 : 1;   /* the sign of the exact quotient */
    switch (mode) {
    case Q_ROUND_FLOOR: return s < 0 ? q - 1 : q;
    case Q_ROUND_CEIL:  return s > 0 ? q + 1 : q;
    case Q_ROUND_ZERO:  return q;
    case Q_ROUND_HALF_EVEN:
    default: {
        i128 ar = abs128(r), rest = abs128(d) - ar;
        if (ar > rest || (ar == rest && (q & 1))) return q + s;
        return q;
    }
    }
}

/* An exact value with 0 < |value| < 1/2 and sign s, rounded by `mode` */
static i128 tiny(int s, QRound mode)
{
    switch (mode) {
    case Q_ROUND_FLOOR: return s < 0 ? -1 : 0;
    case Q_ROUND_CEIL:  return s > 0 ? 1 : 0;
    default:            return 0;
    }
}

/* v × 10^k rounded by `mode`. Sets *ovf if the result does not fit an i128. */
static i128 scale_round(i128 v, int k, QRound mode, bool *ovf)
{
    *ovf = false;
    if (v == 0 || k == 0) return v;
    if (k > 0) {
        if (k > 38 || abs128(v) > I128_MAX / pow10_(k)) {
            *ovf = true;
            return 0;
        }
        return v * pow10_(k);
    }
    int m = -k;
    if (m > 38) return tiny(sgn128(v), mode);   /* |v| < 2^127 < 10^39 / 2 */
    return divround(v, pow10_(m), mode);
}

/* x × 10^k / y rounded by `mode`; x, y finite, y != 0. Sets *ovf if the
 * result does not fit an i128. */
static i128 div_scaled(i128 x, i128 y, int k, QRound mode, bool *ovf)
{
    *ovf = false;
    if (x == 0) return 0;
    if (k >= 0) {
        i128 n = scale_round(x, k, mode, ovf);
        if (*ovf) return 0;
        return divround(n, y, mode);
    }
    int m = -k;
    int s = sgn128(x) * sgn128(y);
    /* |x| < 2^63, so a denominator beyond 2^127 gives |quotient| < 2^-64 */
    if (m > 38 || abs128(y) > I128_MAX / pow10_(m)) return tiny(s, mode);
    return divround(x, y * pow10_(m), mode);
}

/* --- extended bounds ------------------------------------------------------------ */

/* A bound in i128 with infinities: inf is -1, 0 (finite) or +1. */
typedef struct { int inf; i128 v; } Ext;

static Ext ext_of(int64_t b)
{
    if (b == Q_NINF) return (Ext) { -1, 0 };
    if (b == Q_INF) return (Ext) { 1, 0 };
    return (Ext) { 0, b };
}

static int ext_sgn(Ext e) { return e.inf ? e.inf : sgn128(e.v); }

static int ext_cmp(Ext a, Ext b)
{
    if (a.inf != b.inf) return a.inf < b.inf ? -1 : 1;
    if (a.inf) return 0;
    return (a.v > b.v) - (a.v < b.v);
}

static Ext ext_add(Ext a, Ext b)
{
    if (a.inf) return a;
    if (b.inf) return b;
    return (Ext) { 0, a.v + b.v };
}

/* 0 × ±inf is 0: a zero bound times any value of the other interval is 0. */
static Ext ext_mul(Ext a, Ext b)
{
    if ((!a.inf && a.v == 0) || (!b.inf && b.v == 0)) return (Ext) { 0, 0 };
    if (a.inf || b.inf) return (Ext) { ext_sgn(a) * ext_sgn(b), 0 };
    return (Ext) { 0, a.v * b.v };   /* |a|, |b| <= 2^63, so this fits */
}

/* An overflow becomes an infinity with the value's sign. */
static Ext ext_scale(Ext e, int k, QRound mode)
{
    if (e.inf) return e;
    bool ovf;
    i128 r = scale_round(e.v, k, mode, &ovf);
    if (ovf) return (Ext) { sgn128(e.v), 0 };
    return (Ext) { 0, r };
}

/* One corner of a quotient; y does not contain 0. A finite x over an
 * infinite y is 0, the limit. An infinite x over an infinite y is also 0: 0
 * is in the closure of the quotient set, so the hull stays sound. */
static Ext ext_div(Ext x, Ext y, int k, QRound mode)
{
    if ((!x.inf && x.v == 0) || y.inf) return (Ext) { 0, 0 };
    int s = ext_sgn(x) * ext_sgn(y);
    if (x.inf) return (Ext) { s, 0 };
    bool ovf;
    i128 r = div_scaled(x.v, y.v, k, mode, &ovf);
    if (ovf) return (Ext) { s, 0 };
    return (Ext) { 0, r };
}

/* A lower bound below the finite range is -inf. A lower bound above it widens
 * to Q_FMAX, which is still below the exact bound. Upper bounds mirror this. */
static int64_t to_lo(Ext e)
{
    if (e.inf < 0 || (!e.inf && e.v < Q_FMIN)) return Q_NINF;
    if (e.inf > 0 || e.v > Q_FMAX) return Q_FMAX;
    return (int64_t) e.v;
}

static int64_t to_hi(Ext e)
{
    if (e.inf > 0 || (!e.inf && e.v > Q_FMAX)) return Q_INF;
    if (e.inf < 0 || e.v < Q_FMIN) return Q_FMIN;
    return (int64_t) e.v;
}

/* --- dimensions and types --------------------------------------------------------- */

QDim qdim_none(void)
{
    QDim d;
    memset(&d, 0, sizeof d);
    return d;
}

QDim qdim_base(int base)
{
    QDim d = qdim_none();
    if (base >= 0 && base < Q_MAXBASE) d.exp[base] = 1;
    return d;
}

bool qdim_eq(const QDim *a, const QDim *b) { return memcmp(a->exp, b->exp, sizeof a->exp) == 0; }

bool qdim_is_none(const QDim *d)
{
    for (int i = 0; i < Q_MAXBASE; i++)
        if (d->exp[i]) return false;
    return true;
}

static QStatus qdim_combine(const QDim *a, const QDim *b, int sign, QDim *out)
{
    QDim r;
    for (int i = 0; i < Q_MAXBASE; i++) {
        int e = a->exp[i] + sign * b->exp[i];
        if (e < INT8_MIN || e > INT8_MAX) return Q_EDIM;
        r.exp[i] = (int8_t) e;
    }
    *out = r;
    return Q_OK;
}

QStatus qdim_mul(const QDim *a, const QDim *b, QDim *out) { return qdim_combine(a, b, 1, out); }
QStatus qdim_div(const QDim *a, const QDim *b, QDim *out) { return qdim_combine(a, b, -1, out); }

static bool qdim_is_base(const QDim *d, int base)
{
    QDim b = qdim_base(base);
    return qdim_eq(d, &b);
}

int8_t qdim_default_scale(const QDim *d, int8_t tick_scale)
{
    if (qdim_is_none(d)) return -9;
    if (qdim_is_base(d, Q_BASE_LENGTH)) return -4;
    if (qdim_is_base(d, Q_BASE_TIME)) return tick_scale;
    if (qdim_is_base(d, Q_BASE_TEMPERATURE)) return -2;
    /* mass, amount, current, energy, power, and every other dimension */
    return -3;
}

QType qtype_make(uint32_t kind, QDim dim, int8_t scale)
{
    QType t;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.dim = dim;
    t.scale = scale;
    t.width = 64;
    t.rounding = Q_ROUND_HALF_EVEN;
    return t;
}

static QStatus check_width(const QType *t) { return t->width == 64 ? Q_OK : Q_EWIDTH; }

static QStatus make_scaled(int scale, QDim dim, QType *out)
{
    if (scale < INT8_MIN || scale > INT8_MAX) return Q_ESCALE;
    *out = qtype_make(0, dim, (int8_t) scale);
    return Q_OK;
}

QStatus qtype_mul(const QType *a, const QType *b, QType *out)
{
    QStatus s;
    QDim d;
    if ((s = check_width(a)) || (s = check_width(b))) return s;
    if ((s = qdim_mul(&a->dim, &b->dim, &d))) return s;
    return make_scaled(a->scale + b->scale, d, out);
}

QStatus qtype_div(const QType *a, const QType *b, QType *out)
{
    QStatus s;
    QDim d;
    if ((s = check_width(a)) || (s = check_width(b))) return s;
    if ((s = qdim_div(&a->dim, &b->dim, &d))) return s;
    return make_scaled(a->scale - b->scale, d, out);
}

QStatus qtype_sum(const QType *a, const QType *b, QType *out)
{
    QStatus s;
    if ((s = check_width(a)) || (s = check_width(b))) return s;
    if (!qdim_eq(&a->dim, &b->dim)) return Q_EDIM;
    if (a->kind && b->kind && a->kind != b->kind) return Q_EKIND;
    QType r = qtype_make(a->kind ? a->kind : b->kind, a->dim, a->scale < b->scale ? a->scale : b->scale);
    *out = r;
    return Q_OK;
}

QStatus qtype_conv_plan(const QType *from, const QType *to, QConv *out)
{
    QStatus s;
    if ((s = check_width(from)) || (s = check_width(to))) return s;
    if (!qdim_eq(&from->dim, &to->dim)) return Q_EDIM;
    if (from->kind && from->kind != to->kind) return Q_EKIND;
    out->k = (int16_t) (from->scale - to->scale);
    out->rounding = to->rounding;
    return Q_OK;
}

static QStatus product_plan(const QType *a, const QType *b, const QType *dest, int sign, QConv *out)
{
    QStatus s;
    QDim d;
    if ((s = check_width(a)) || (s = check_width(b)) || (s = check_width(dest))) return s;
    if ((s = qdim_combine(&a->dim, &b->dim, sign, &d))) return s;
    if (!qdim_eq(&d, &dest->dim)) return Q_EDIM;
    out->k = (int16_t) (a->scale + sign * b->scale - dest->scale);
    out->rounding = dest->rounding;
    return Q_OK;
}

QStatus qtype_mul_plan(const QType *a, const QType *b, const QType *dest, QConv *out)
{
    return product_plan(a, b, dest, 1, out);
}

QStatus qtype_div_plan(const QType *a, const QType *b, const QType *dest, QConv *out)
{
    return product_plan(a, b, dest, -1, out);
}

/* --- interval operations ------------------------------------------------------------ */

QInterval qi_point(int64_t v) { return (QInterval) { v, v }; }
QInterval qi_top(void) { return (QInterval) { Q_NINF, Q_INF }; }

static bool valid_bound(int64_t b) { return b == Q_NINF || b == Q_INF || (b >= Q_FMIN && b <= Q_FMAX); }

bool qi_valid(QInterval a)
{
    return valid_bound(a.lo) && valid_bound(a.hi) && a.lo != Q_INF && a.hi != Q_NINF && a.lo <= a.hi;
}

bool qi_is_point(QInterval a) { return a.lo == a.hi; }
bool qi_contains(QInterval a, int64_t v) { return a.lo <= v && v <= a.hi; }
bool qi_subset(QInterval a, QInterval b) { return b.lo <= a.lo && a.hi <= b.hi; }

bool qi_meet(QInterval a, QInterval b, QInterval *out)
{
    QInterval r = { a.lo > b.lo ? a.lo : b.lo, a.hi < b.hi ? a.hi : b.hi };
    if (r.lo > r.hi) return false;
    *out = r;
    return true;
}

static int64_t neg_bound(int64_t b)
{
    if (b == Q_INF) return Q_NINF;
    if (b == Q_NINF) return Q_INF;
    return -b;
}

QInterval qi_neg(QInterval a) { return (QInterval) { neg_bound(a.hi), neg_bound(a.lo) }; }

QInterval qi_add(QInterval a, QInterval b)
{
    return (QInterval) {
        to_lo(ext_add(ext_of(a.lo), ext_of(b.lo))),
        to_hi(ext_add(ext_of(a.hi), ext_of(b.hi))),
    };
}

QInterval qi_sub(QInterval a, QInterval b) { return qi_add(a, qi_neg(b)); }

QInterval qi_conv(QInterval a, const QConv *c)
{
    return (QInterval) {
        to_lo(ext_scale(ext_of(a.lo), c->k, Q_ROUND_FLOOR)),
        to_hi(ext_scale(ext_of(a.hi), c->k, Q_ROUND_CEIL)),
    };
}

QInterval qi_mul(QInterval a, QInterval b, const QConv *c)
{
    Ext xs[2] = { ext_of(a.lo), ext_of(a.hi) }, ys[2] = { ext_of(b.lo), ext_of(b.hi) };
    Ext lo = ext_mul(xs[0], ys[0]), hi = lo;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            Ext p = ext_mul(xs[i], ys[j]);
            if (ext_cmp(p, lo) < 0) lo = p;
            if (ext_cmp(p, hi) > 0) hi = p;
        }
    /* scaling is monotonic, so the extreme products give the extreme results */
    return (QInterval) { to_lo(ext_scale(lo, c->k, Q_ROUND_FLOOR)), to_hi(ext_scale(hi, c->k, Q_ROUND_CEIL)) };
}

QInterval qi_div(QInterval a, QInterval b, const QConv *c)
{
    if (b.lo <= 0 && b.hi >= 0) return qi_top();
    Ext xs[2] = { ext_of(a.lo), ext_of(a.hi) }, ys[2] = { ext_of(b.lo), ext_of(b.hi) };
    Ext lo = { 0, 0 }, hi = { 0, 0 };
    bool first = true;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            Ext f = ext_div(xs[i], ys[j], c->k, Q_ROUND_FLOOR);
            Ext u = ext_div(xs[i], ys[j], c->k, Q_ROUND_CEIL);
            if (first || ext_cmp(f, lo) < 0) lo = f;
            if (first || ext_cmp(u, hi) > 0) hi = u;
            first = false;
        }
    return (QInterval) { to_lo(lo), to_hi(hi) };
}

/* --- point operations ------------------------------------------------------------------ */

static QStatus point_in(int64_t v)
{
    if (v == Q_NINF || v == Q_INF) return Q_EINFINITE;
    if (v < Q_FMIN) return Q_EOVERFLOW;
    return Q_OK;
}

static QStatus point_out(i128 v, int64_t *out)
{
    if (v < Q_FMIN || v > Q_FMAX) return Q_EOVERFLOW;
    *out = (int64_t) v;
    return Q_OK;
}

QStatus qp_add(int64_t a, int64_t b, int64_t *out)
{
    QStatus s;
    if ((s = point_in(a)) || (s = point_in(b))) return s;
    return point_out((i128) a + b, out);
}

QStatus qp_sub(int64_t a, int64_t b, int64_t *out)
{
    QStatus s;
    if ((s = point_in(a)) || (s = point_in(b))) return s;
    return point_out((i128) a - b, out);
}

QStatus qp_conv(int64_t a, const QConv *c, int64_t *out)
{
    QStatus s;
    if ((s = point_in(a))) return s;
    bool ovf;
    i128 r = scale_round(a, c->k, (QRound) c->rounding, &ovf);
    if (ovf) return Q_EOVERFLOW;
    return point_out(r, out);
}

QStatus qp_mul(int64_t a, int64_t b, const QConv *c, int64_t *out)
{
    QStatus s;
    if ((s = point_in(a)) || (s = point_in(b))) return s;
    bool ovf;
    i128 r = scale_round((i128) a * b, c->k, (QRound) c->rounding, &ovf);
    if (ovf) return Q_EOVERFLOW;
    return point_out(r, out);
}

QStatus qp_div(int64_t a, int64_t b, const QConv *c, int64_t *out)
{
    QStatus s;
    if ((s = point_in(a)) || (s = point_in(b))) return s;
    if (b == 0) return Q_EDIVZERO;
    bool ovf;
    i128 r = div_scaled(a, b, c->k, (QRound) c->rounding, &ovf);
    if (ovf) return Q_EOVERFLOW;
    return point_out(r, out);
}

/* --- dimension text ----------------------------------------------------------------------- */

typedef struct { const char *sym; int8_t l, m, t, a, k, n, j; } UnitSym;

static const UnitSym unit_syms[] = {
    /*        m  kg   s  A  K mol cd */
    { "m",    1,  0,  0, 0, 0, 0, 0 },
    { "kg",   0,  1,  0, 0, 0, 0, 0 },
    { "s",    0,  0,  1, 0, 0, 0, 0 },
    { "A",    0,  0,  0, 1, 0, 0, 0 },
    { "K",    0,  0,  0, 0, 1, 0, 0 },
    { "mol",  0,  0,  0, 0, 0, 1, 0 },
    { "cd",   0,  0,  0, 0, 0, 0, 1 },
    { "N",    1,  1, -2, 0, 0, 0, 0 },
    { "J",    2,  1, -2, 0, 0, 0, 0 },
    { "W",    2,  1, -3, 0, 0, 0, 0 },
    { "Pa",  -1,  1, -2, 0, 0, 0, 0 },
    { "Hz",   0,  0, -1, 0, 0, 0, 0 },
};

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9');
}

/* the dimension of one symbol, or false if the symbol is unknown */
static bool symbol_dim(const char *s, size_t n, const char *const *names, size_t nnames, QDim *out)
{
    *out = qdim_none();
    for (size_t i = 0; i < sizeof unit_syms / sizeof unit_syms[0]; i++) {
        const UnitSym *u = &unit_syms[i];
        if (strlen(u->sym) == n && !memcmp(u->sym, s, n)) {
            out->exp[Q_BASE_LENGTH] = u->l;
            out->exp[Q_BASE_MASS] = u->m;
            out->exp[Q_BASE_TIME] = u->t;
            out->exp[Q_BASE_CURRENT] = u->a;
            out->exp[Q_BASE_TEMPERATURE] = u->k;
            out->exp[Q_BASE_AMOUNT] = u->n;
            out->exp[Q_BASE_LUMINOUS] = u->j;
            return true;
        }
    }
    for (size_t i = 0; i < nnames && Q_NSI + i < Q_MAXBASE; i++) {
        if (names[i] && strlen(names[i]) == n && !memcmp(names[i], s, n)) {
            out->exp[Q_NSI + i] = 1;
            return true;
        }
    }
    return false;
}

QStatus q_parse_dim(const char *s, size_t len, const char *const *names, size_t nnames, QDim *out)
{
    QDim d = qdim_none();
    size_t i = 0;
    bool denominator = false, any = false;
    if (len == 1 && s[0] == '1') {
        *out = d;
        return Q_OK;
    }
    while (i < len) {
        if (s[i] == ' ' || s[i] == '*') {
            i++;
            continue;
        }
        if (s[i] == '/') {
            if (denominator || !any) return Q_ESYNTAX;
            denominator = true;
            any = false;
            i++;
            continue;
        }
        size_t start = i;
        while (i < len && is_name_char(s[i])) i++;
        if (i == start || (s[start] >= '0' && s[start] <= '9')) return Q_ESYNTAX;
        QDim sym;
        if (!symbol_dim(s + start, i - start, names, nnames, &sym)) return Q_EDIM;
        int exp = 1;
        if (i < len && s[i] == '^') {
            i++;
            bool neg = false;
            if (i < len && s[i] == '-') {
                neg = true;
                i++;
            }
            if (i >= len || s[i] < '0' || s[i] > '9') return Q_ESYNTAX;
            exp = 0;
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                exp = exp * 10 + (s[i++] - '0');
                if (exp > 127) return Q_EDIM;
            }
            if (neg) exp = -exp;
        }
        if (denominator) exp = -exp;
        for (int b = 0; b < Q_MAXBASE; b++) {
            int e = d.exp[b] + exp * sym.exp[b];
            if (e < INT8_MIN || e > INT8_MAX) return Q_EDIM;
            d.exp[b] = (int8_t) e;
        }
        any = true;
    }
    if (denominator && !any) return Q_ESYNTAX;
    *out = d;
    return Q_OK;
}

/* --- decimal text ------------------------------------------------------------------------ */

QStatus q_parse_decimal(const char *s, size_t len, int scale, int64_t *out)
{
    size_t i = 0;
    bool neg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';

    i128 mant = 0;
    int digits = 0, frac = 0;
    bool point = false;
    for (; i < len; i++) {
        if (s[i] == '.') {
            if (point) return Q_ESYNTAX;
            point = true;
        } else if (s[i] >= '0' && s[i] <= '9') {
            if (mant > pow10_(37)) return Q_EOVERFLOW;   /* more than 38 significant digits */
            mant = mant * 10 + (s[i] - '0');
            digits++;
            if (point) frac++;
        } else {
            break;
        }
    }
    if (digits == 0) return Q_ESYNTAX;

    long exp = 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < len && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        int edigits = 0;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++, edigits++) {
            exp = exp * 10 + (s[i] - '0');
            if (exp > 10000) return Q_EOVERFLOW;
        }
        if (edigits == 0) return Q_ESYNTAX;
        if (eneg) exp = -exp;
    }
    if (i != len) return Q_ESYNTAX;

    if (mant == 0) {
        *out = 0;
        return Q_OK;
    }
    /* the value is mant × 10^(exp - frac) units = mant × 10^(exp - frac - scale) quanta */
    long k = exp - frac - scale;
    i128 v;
    if (k >= 0) {
        if (k > 38 || mant > I128_MAX / pow10_((int) k)) return Q_EOVERFLOW;
        v = mant * pow10_((int) k);
    } else {
        if (-k > 38 || mant % pow10_((int) -k) != 0) return Q_EINEXACT;
        v = mant / pow10_((int) -k);
    }
    if (neg) v = -v;
    return point_out(v, out);
}

size_t q_format_decimal(int64_t q, int scale, char *buf, size_t len)
{
    char tmp[512];
    size_t n = 0;
    if (q == Q_INF || q == Q_NINF) {
        return (size_t) snprintf(buf, len, "%s", q == Q_INF ? "inf" : "-inf");
    }
    bool neg = q < 0;
    uint64_t mag = neg ? (uint64_t) 0 - (uint64_t) q : (uint64_t) q;

    char digits[32];
    int nd = snprintf(digits, sizeof digits, "%llu", (unsigned long long) mag);

    if (neg) tmp[n++] = '-';
    if (mag == 0 || scale >= 0) {
        memcpy(tmp + n, digits, (size_t) nd);
        n += (size_t) nd;
        if (mag != 0)
            for (int z = 0; z < scale && n < sizeof tmp - 1; z++) tmp[n++] = '0';
    } else {
        int m = -scale;   /* digits after the point */
        int whole = nd - m;
        if (whole > 0) {
            memcpy(tmp + n, digits, (size_t) whole);
            n += (size_t) whole;
        } else {
            tmp[n++] = '0';
        }
        tmp[n++] = '.';
        for (int z = 0; z < -whole && n < sizeof tmp - 1; z++) tmp[n++] = '0';
        int start = whole > 0 ? whole : 0;
        memcpy(tmp + n, digits + start, (size_t) (nd - start));
        n += (size_t) (nd - start);
        while (tmp[n - 1] == '0') n--;
        if (tmp[n - 1] == '.') n--;
    }
    tmp[n] = 0;
    return (size_t) snprintf(buf, len, "%s", tmp);
}
