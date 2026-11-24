#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "front/front.h"
#include "test.h"

/* Stage 2 of the front end: kinds, the type hierarchy, and the unifier
 * (ADR 0011 section 3, ADR 0015). */

#define STR_EQ(got, want)                                                              \
    do {                                                                               \
        const char *g_ = (got), *w_ = (want);                                          \
        t_checks++;                                                                    \
        if (strcmp(g_, w_)) {                                                          \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d:\n  got    %s\n  wanted %s\n", __FILE__, __LINE__, g_, w_); \
        }                                                                              \
    } while (0)

static const char *const KINDS =
    "%length: { dimension: \"m\" }\n"
    "%mass: { dimension: \"kg\" }\n"
    "%energy: { dimension: \"J\", scale: -3, conserved: true }\n";

/* Loads sources. The result is valid until qf_free. */
#define HAS_TEXT(text, part)                                                           \
    do {                                                                               \
        const char *t_ = (text), *p_ = (part);                                         \
        t_checks++;                                                                    \
        if (!strstr(t_, p_)) {                                                         \
            t_failures++;                                                              \
            fprintf(stderr, "%s:%d:\n  got    %s\n  wanted a text with %s\n", __FILE__, __LINE__, t_, p_); \
        }                                                                              \
    } while (0)

static Front *load(const char *const *sources, size_t n, bool *ok)
{
    static const char *const names[] = { "a.quad", "b.quad", "c.quad", "d.quad", "e.quad", "f.quad", "g.quad", "h.quad" };
    Front *f = qf_new();
    *ok = qf_load(f, names, sources, n);
    return f;
}

static Front *load1(const char *src, bool *ok)
{
    return load(&src, 1, ok);
}

/* The first diagnostic of a content with an error, or "loaded". */
static const char *load_error(const char *src)
{
    static char out[1024];
    bool ok;
    Front *f = load1(src, &ok);
    CHECK(ok == (f->nerrors == 0));
    snprintf(out, sizeof out, "%s", ok ? "loaded" : f->diags.n ? f->diags.v[0].msg : "no diagnostic");
    qf_free(f);
    return out;
}

/* The text form of an instance of a content, or the first diagnostic. */
static const char *inst_text(const char *src, const char *name)
{
    static Buf out;
    out.len = 0;
    qf_buf_puts(&out, "");
    bool ok;
    Front *f = load1(src, &ok);
    uint32_t i = ok ? qf_instance(f, name) : QF_BOTTOM;
    if (i != QF_BOTTOM) qf_node_print(f, &out, f->insts.v[i].node);
    else qf_buf_puts(&out, f->diags.n ? f->diags.v[0].msg : "no such instance");
    qf_free(f);
    return out.s;
}

static const char *with_kinds(const char *rest)
{
    static char buf[4096];
    snprintf(buf, sizeof buf, "%s%s", KINDS, rest);
    return buf;
}

/* A path of features from labels: "west.energy". */
static size_t path_of(Front *f, const char *text, uint32_t *out)
{
    size_t n = 0;
    while (*text) {
        const char *e = strchr(text, '.');
        size_t len = e ? (size_t) (e - text) : strlen(text);
        out[n] = feat_lookup(f, qf_intern(f, text, len));
        CHECK(out[n] != QF_BOTTOM);
        n++;
        text += len + (e ? 1 : 0);
    }
    return n;
}

static const QfNode *at(Front *f, const QfNode *n, const char *text)
{
    uint32_t p[16];
    size_t len = path_of(f, text, p);
    return qf_at(f->op, n, p, len);
}

static const QfNode *inst(Front *f, const char *name)
{
    uint32_t i = qf_instance(f, name);
    CHECK(i != QF_BOTTOM);
    return i == QF_BOTTOM ? NULL : f->insts.v[i].node;
}

static const char *node_text(const Front *f, const QfNode *n)
{
    static Buf out;
    out.len = 0;
    qf_buf_puts(&out, "");
    if (n) qf_node_print(f, &out, n);
    else qf_buf_puts(&out, "(none)");
    return out.s;
}

/* --- units (ADR 0015 section 4) --------------------------------------------------------------- */

static const char *unit_text(const Front *f, const char *text)
{
    static Buf out;
    out.len = 0;
    QfUnit u;
    char err[400];
    if (!qf_unit_parse(f, text, &u, err, sizeof err)) { qf_buf_puts(&out, err); return out.s; }
    qf_dim_format(f, &out, &u.dim);
    qf_buf_printf(&out, " x %lld e%d", (long long) u.mult, u.exp10);
    return out.s;
}

void test_units(void)
{
    Front *f = qf_new();
    STR_EQ(unit_text(f, "m"), "m x 1 e0");
    STR_EQ(unit_text(f, "mm"), "m x 1 e-3");
    STR_EQ(unit_text(f, "km"), "m x 1 e3");
    STR_EQ(unit_text(f, "g"), "kg x 1 e-3");
    STR_EQ(unit_text(f, "kg"), "kg x 1 e0");
    STR_EQ(unit_text(f, "ug"), "kg x 1 e-9");
    STR_EQ(unit_text(f, "J"), "m^2 kg s^-2 x 1 e0");
    STR_EQ(unit_text(f, "kJ"), "m^2 kg s^-2 x 1 e3");
    STR_EQ(unit_text(f, "W/K"), "m^2 kg s^-3 K^-1 x 1 e0");
    STR_EQ(unit_text(f, "kg*m^2/s^2"), "m^2 kg s^-2 x 1 e0");
    STR_EQ(unit_text(f, "m^-1"), "m^-1 x 1 e0");
    STR_EQ(unit_text(f, "L"), "m^3 x 1 e-3");
    STR_EQ(unit_text(f, "mL"), "m^3 x 1 e-6");
    /* a symbol as a whole comes before a prefix and a symbol */
    STR_EQ(unit_text(f, "min"), "s x 60 e0");
    STR_EQ(unit_text(f, "mol"), "mol x 1 e0");
    STR_EQ(unit_text(f, "cd"), "cd x 1 e0");
    STR_EQ(unit_text(f, "h"), "s x 3600 e0");
    STR_EQ(unit_text(f, "d"), "s x 86400 e0");
    STR_EQ(unit_text(f, "hPa"), "m^-1 kg s^-2 x 1 e2");
    STR_EQ(unit_text(f, "km/h^0"), "m x 1 e3");
    STR_EQ(unit_text(f, "min^2"), "s^2 x 3600 e0");
    /* every term after the first '/' divides, and each has its own '/' */
    STR_EQ(unit_text(f, "J/kg/K"), "m^2 s^-2 K^-1 x 1 e0");
    STR_EQ(unit_text(f, "J/kg*K"), "in the unit 'J/kg*K', a '*' after a '/' has two readings. Give each divisor its "
                                   "own '/': J/kg/K");
    STR_EQ(unit_text(f, "kg*m/s^2*K*mol"), "in the unit 'kg*m/s^2*K*mol', a '*' after a '/' has two readings. Give "
                                           "each divisor its own '/': kg*m/s^2/K/mol");
    /* units with no dimension */
    STR_EQ(unit_text(f, "pct"), "1 x 1 e-2");
    STR_EQ(unit_text(f, "ppm"), "1 x 1 e-6");
    STR_EQ(unit_text(f, "kpct"), "unknown unit 'kpct'");
    STR_EQ(unit_text(f, "g/kg*pct"), "in the unit 'g/kg*pct', a '*' after a '/' has two readings. Give each divisor "
                                     "its own '/': g/kg/pct");
    STR_EQ(unit_text(f, "W/m^2/K^4"), "kg s^-3 K^-4 x 1 e0");
    STR_EQ(unit_text(f, "kJ/g"), "m^2 s^-2 x 1 e6");

    STR_EQ(unit_text(f, "furlong"), "unknown unit 'furlong'");
    STR_EQ(unit_text(f, "kmin"), "unknown unit 'kmin'");           /* min takes no prefix */
    STR_EQ(unit_text(f, "m3"), "unknown unit 'm3'");
    STR_EQ(unit_text(f, "m/x"), "unknown unit 'x' in 'm/x'. If 'x' is a name, put spaces around the operator, "
                                "because an operator with a name directly after it continues the unit");
    STR_EQ(unit_text(f, "m/min"), "the unit 'min' cannot be a divisor in 'm/min', because its factor is not a "
                                  "power of ten. Write the value with 's'");
    STR_EQ(unit_text(f, "m^200"), "the exponent in the unit 'm^200' is too large");
    STR_EQ(unit_text(f, "m^90*m^90"), "the exponents of the unit 'm^90*m^90' are too large");
    STR_EQ(unit_text(f, "d^4"), "the factor of the unit 'd^4' is too large");

    CHECK(qf_unit_symbol(f, "s") && qf_unit_symbol(f, "kg") && qf_unit_symbol(f, "min") && qf_unit_symbol(f, "mK"));
    CHECK(!qf_unit_symbol(f, "x") && !qf_unit_symbol(f, "sec") && !qf_unit_symbol(f, "coin"));

    /* a literal: the digits, the fraction and the unit give one exact decimal */
    QDim dim;
    int64_t v;
    int e;
    char err[400];
    XNum n = { 35, 2, qf_sym(f, "kg") };
    CHECK(qf_literal(f, &n, &dim, &v, &e, err, sizeof err) && v == 35 && e == -2 && dim.exp[Q_BASE_MASS] == 1);
    XNum m = { 3, 0, qf_sym(f, "min") };
    CHECK(qf_literal(f, &m, &dim, &v, &e, err, sizeof err) && v == 180 && e == 0 && dim.exp[Q_BASE_TIME] == 1);
    XNum p = { 1989, -27, qf_sym(f, "g") };
    CHECK(qf_literal(f, &p, &dim, &v, &e, err, sizeof err) && v == 1989 && e == 24);
    XNum plain = { -5, 1, NULL };
    CHECK(qf_literal(f, &plain, &dim, &v, &e, err, sizeof err) && v == -5 && e == -1 && qdim_is_none(&dim));
    XNum big = { INT64_MAX / 2, 0, qf_sym(f, "d") };
    CHECK(!qf_literal(f, &big, &dim, &v, &e, err, sizeof err));
    STR_EQ(err, "the number is out of range");
    qf_free(f);
}

/* --- exact decimals (ADR 0015 section 3) -------------------------------------------------------- */

static const char *quanta_text(QfDec d, int scale)
{
    static char out[160];
    int64_t lo, hi;
    const char *why = qf_dec_to_quanta(d, scale, &lo, &hi);
    if (why) snprintf(out, sizeof out, "%s", why);
    else snprintf(out, sizeof out, "%lld..%lld", (long long) lo, (long long) hi);
    return out;
}

static const char *dec_text(int64_t v, int exp)
{
    static Buf out;
    out.len = 0;
    qf_dec_format(&out, v, exp);
    return out.s;
}

void test_decimals(void)
{
    QfDec a = { 1500, 1500, -1, false, false };
    QfDec n = qf_dec_norm(a);
    CHECK(n.lo == 15 && n.hi == 15 && n.exp == 1);
    QfDec z = { 0, 0, -7, false, false };
    CHECK(qf_dec_norm(z).exp == 0);
    QfDec half = { 100, Q_INF, -3, false, false };
    n = qf_dec_norm(half);
    CHECK(n.lo == 1 && n.hi == Q_INF && n.exp == -1);
    QfDec two = { 100, 250, 0, false, false };
    n = qf_dec_norm(two);
    CHECK(n.lo == 10 && n.hi == 25 && n.exp == 1);
    QfDec top = { Q_NINF, Q_INF, 5, false, false };
    CHECK(qf_dec_norm(top).exp == 0);

    /* a point value must be a whole number of quanta */
    QfDec p15 = { 15, 15, -5, false, false };                   /* 0.15 mm, in metres */
    STR_EQ(quanta_text(p15, -4), "the value is not a multiple of the smallest step of the kind");
    STR_EQ(quanta_text(p15, -5), "15..15");
    STR_EQ(quanta_text(p15, -8), "15000..15000");
    /* a bound moves inward to the next quantum */
    QfDec ge = { 15, Q_INF, -5, false, false };
    STR_EQ(quanta_text(ge, -4), "2..9223372036854775807");
    QfDec gt = { 2, Q_INF, -4, true, false };                   /* > 0.2 mm */
    STR_EQ(quanta_text(gt, -4), "3..9223372036854775807");
    QfDec gtx = { 15, Q_INF, -5, true, false };                 /* > 0.15 mm */
    STR_EQ(quanta_text(gtx, -4), "2..9223372036854775807");
    QfDec le = { Q_NINF, 15, -5, false, false };
    STR_EQ(quanta_text(le, -4), "-9223372036854775808..1");
    QfDec lt = { Q_NINF, 2, -4, false, true };                  /* < 0.2 mm */
    STR_EQ(quanta_text(lt, -4), "-9223372036854775808..1");
    QfDec neg = { -15, 15, -5, false, false };                  /* -0.15 mm to 0.15 mm */
    STR_EQ(quanta_text(neg, -4), "-1..1");
    QfDec negopen = { -2, 2, -4, true, true };
    STR_EQ(quanta_text(negopen, -4), "-1..1");
    QfDec none = { 11, 19, -5, true, true };                    /* between 0.11 mm and 0.19 mm */
    STR_EQ(quanta_text(none, -4), "the range holds no multiple of the smallest step of the kind");
    QfDec huge = { 1, 1, 30, false, false };
    STR_EQ(quanta_text(huge, -4), "the value does not fit in the range of the kind");
    QfDec tiny = { 1, 1, -40, false, false };
    STR_EQ(quanta_text(tiny, -4), "the value is not a multiple of the smallest step of the kind");
    QfDec tinyb = { 1, Q_INF, -40, false, false };              /* >= 1e-40: the next quantum is 1 */
    STR_EQ(quanta_text(tinyb, -4), "1..9223372036854775807");
    QfDec tinyn = { -1, Q_INF, -40, false, false };             /* >= -1e-40: 0 is allowed */
    STR_EQ(quanta_text(tinyn, -4), "0..9223372036854775807");
    STR_EQ(quanta_text(top, -4), "-9223372036854775808..9223372036854775807");
    QfDec zero = { 0, 0, 0, false, false };
    STR_EQ(quanta_text(zero, -30), "0..0");
    STR_EQ(quanta_text(zero, 30), "0..0");

    /* meets align the powers of ten */
    QfDec r;
    bool range;
    QfDec x = { 5, Q_INF, -1, false, false }, y = { Q_NINF, 2, 0, false, true };     /* >=0.5 and <2 */
    CHECK(qf_dec_meet(x, y, &r, &range) && r.lo == 5 && r.hi == 20 && r.exp == -1 && !r.lo_open && r.hi_open);
    QfDec pt = { 2, 2, 0, false, false };
    CHECK(!qf_dec_meet(pt, y, &r, &range) && !range);            /* 2 is not below 2 */
    CHECK(qf_dec_meet(pt, x, &r, &range) && r.lo == 2 && r.hi == 2 && r.exp == 0);
    QfDec far1 = { 1, 1, 30, false, false }, far2 = { 1, 1, -30, false, false };
    CHECK(!qf_dec_meet(far1, far2, &r, &range) && range);
    CHECK(qf_dec_meet(top, far1, &r, &range) && r.lo == 1 && r.exp == 30);
    CHECK(qf_dec_meet(zero, x, &r, &range) == false);
    QfDec zr = { 0, Q_INF, 0, false, false };
    CHECK(qf_dec_meet(zr, far2, &r, &range) && r.lo == 1 && r.hi == 1 && r.exp == -30);

    STR_EQ(dec_text(5000000, -3), "5000");
    STR_EQ(dec_text(29315, -2), "293.15");
    STR_EQ(dec_text(-15, -5), "-0.00015");
    STR_EQ(dec_text(2, 3), "2000");
    STR_EQ(dec_text(1989, 27), "1989e27");
    STR_EQ(dec_text(0, -9), "0");
    STR_EQ(dec_text(Q_INF, 0), "inf");
}

/* --- kind declarations (ADR 0015 section 1) ------------------------------------------------------ */

void test_kind_declarations(void)
{
    bool ok;
    Front *f = load1("%energy: { dimension: \"J\", scale: -3, conserved: true }\n"
                     "%capacity: { dimension: \"J/K\" }\n"
                     "%coin: { base: \"coin\", scale: -2, rounding: \"floor\" }\n"
                     "%price: { dimension: \"coin/kg\" }\n"
                     "%share: { dimension: \"1\" }\n"
                     "%span: { dimension: \"s\" }\n", &ok);
    CHECK(ok);
    uint32_t k = qf_kind_lookup(f, qf_sym(f, "%energy"));
    CHECK(k != QF_BOTTOM);
    if (k != QF_BOTTOM) {
        const KindInfo *ki = &f->kinds.v[k];
        CHECK(ki->scale == -3 && ki->conserved && ki->rounding == Q_ROUND_HALF_EVEN && !ki->builtin);
        CHECK(f->dims.v[ki->dimix].dim.exp[Q_BASE_MASS] == 1 && f->dims.v[ki->dimix].dim.exp[Q_BASE_TIME] == -2);
        CHECK(f->types.v[ki->type].kind == TK_KIND && f->types.v[ki->type].name == ki->name);
        /* a kind is a leaf below the type of its dimension, which is below number */
        uint32_t dt = f->types.v[ki->type].parents.v[0];
        CHECK(f->types.v[dt].kind == TK_DIMT && f->types.v[dt].parents.v[0] == f->tnumber);
        STR_EQ(f->types.v[dt].name, "dim[m^2 kg s^-2]");
    }
    k = qf_kind_lookup(f, qf_sym(f, "%capacity"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -3 && !f->kinds.v[k].conserved);       /* the default scale */
    k = qf_kind_lookup(f, qf_sym(f, "%coin"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -2 && f->kinds.v[k].rounding == Q_ROUND_FLOOR);
    CHECK(f->bases.n == 2 && !strcmp(f->bases.v[0], "coin") && !strcmp(f->bases.v[1], "deg"));   /* the angle is built in */
    if (k != QF_BOTTOM) CHECK(f->dims.v[f->kinds.v[k].dimix].dim.exp[Q_NSI] == 1);
    k = qf_kind_lookup(f, qf_sym(f, "%price"));
    if (k != QF_BOTTOM) CHECK(f->dims.v[f->kinds.v[k].dimix].dim.exp[Q_NSI] == 1 && f->dims.v[f->kinds.v[k].dimix].dim.exp[Q_BASE_MASS] == -1);
    k = qf_kind_lookup(f, qf_sym(f, "%share"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -9);                                    /* a ratio */
    k = qf_kind_lookup(f, qf_sym(f, "%span"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -6);                                    /* the tick */
    /* the built-in kinds */
    k = qf_kind_lookup(f, qf_sym(f, "%count"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == 0 && f->kinds.v[k].builtin);
    k = qf_kind_lookup(f, qf_sym(f, "%ratio"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -9);
    k = qf_kind_lookup(f, qf_sym(f, "%time"));
    CHECK(k != QF_BOTTOM && f->kinds.v[k].scale == -6);
    /* %share and %ratio and %count have one dimension type */
    CHECK(f->kinds.v[qf_kind_lookup(f, qf_sym(f, "%share"))].dimix == f->kinds.v[k - 1].dimix);
    CHECK(qf_kind_lookup(f, qf_sym(f, "%none")) == QF_BOTTOM);
    qf_free(f);

    /* the tick of the world is the scale of %time */
    f = qf_new();
    f->tick_scale = -3;
    const char *src = "%span: { dimension: \"s\" }\n#T: { t: %time, u: %span }\na: #T & { t: 1.5s, u: 2s }\n", *name = "a.quad";
    CHECK(qf_load(f, &name, &src, 1));
    const QfNode *t = at(f, inst(f, "a"), "t"), *u = at(f, inst(f, "a"), "u");
    CHECK(t && t->lo == 1500 && t->hi == 1500);
    CHECK(u && u->lo == 2000 && f->kinds.v[qf_kind_lookup(f, qf_sym(f, "%span"))].scale == -3);
    qf_free(f);

    /* the indices of base dimensions follow their symbols, not the order of declarations */
    for (int order = 0; order < 2; order++) {
        f = load1(order ? "%b: { base: \"coin\" }\n%a: { base: \"zed\" }\n" : "%a: { base: \"zed\" }\n%b: { base: \"coin\" }\n", &ok);
        CHECK(ok && f->bases.n == 3 && !strcmp(f->bases.v[0], "coin") && !strcmp(f->bases.v[1], "deg") &&
              !strcmp(f->bases.v[2], "zed"));
        qf_free(f);
    }

    STR_EQ(load_error("%e: 5\n"),
           "a.quad:1:5: error: the body of the kind %e must be a struct of literal fields: { dimension: \"J\", scale: -3 }");
    STR_EQ(load_error("%e: { dimension: \"J\", unit: \"J\" }\n"),
           "a.quad:1:23: error: a kind has the fields dimension, base, scale, conserved and rounding, not unit");
    STR_EQ(load_error("%e: { scale: -3 }\n"),
           "a.quad:1:1: error: the kind %e needs a dimension (dimension: \"J\") or a new base dimension (base: \"coin\")");
    STR_EQ(load_error("%e: { dimension: \"J\", base: \"joule\" }\n"),
           "a.quad:1:23: error: the kind %e has a dimension and a base: give one of them");
    STR_EQ(load_error("%e: { dimension: \"furlong\" }\n"),
           "a.quad:1:18: error: the dimension of the kind %e is not valid. Write the coherent units as a string, "
           "for example \"J/K\" or \"kg m^2 s^-2\", or \"1\" for no dimension");
    STR_EQ(load_error("%e: { dimension: J }\n"),
           "a.quad:1:18: error: the dimension of the kind %e is not valid. Write the coherent units as a string, "
           "for example \"J/K\" or \"kg m^2 s^-2\", or \"1\" for no dimension");
    STR_EQ(load_error("%e: { dimension: \"J\", scale: 1.5 }\n"),
           "a.quad:1:30: error: the scale of the kind %e must be an integer from -60 to 60");
    STR_EQ(load_error("%e: { dimension: \"J\", scale: 3mm }\n"),
           "a.quad:1:30: error: the scale of the kind %e must be an integer from -60 to 60");
    STR_EQ(load_error("%e: { dimension: \"J\", scale: 61 }\n"),
           "a.quad:1:30: error: the scale of the kind %e must be an integer from -60 to 60");
    STR_EQ(load_error("%e: { dimension: \"J\", conserved: 1 }\n"), "a.quad:1:34: error: conserved must be true or false");
    STR_EQ(load_error("%e: { dimension: \"J\", rounding: \"up\" }\n"),
           "a.quad:1:33: error: rounding must be \"half-even\", \"floor\", \"ceiling\" or \"zero\"");
    STR_EQ(load_error("%e: { dimension: \"J\" }\n%e: { dimension: \"J\" }\n"),
           "a.quad:2:1: error: the kind %e is already declared at a.quad:1");
    STR_EQ(load_error("%time: { dimension: \"s\" }\n"),
           "a.quad:1:1: error: %time is a built-in kind that content cannot change: its scale is the tick of the world");
    STR_EQ(load_error("%count: { scale: -1 }\n"), "a.quad:1:1: error: %count is a built-in kind that content cannot change");
    STR_EQ(load_error("%ratio: { scale: -3 }\n"), "a.quad:1:1: error: %ratio is a built-in kind that content cannot change");
    STR_EQ(load_error("%e: { base: \"deg\" }\n"), "a.quad:1:13: error: 'deg' is already a unit symbol");
    STR_EQ(load_error("%e: { base: \"turn\" }\n"), "a.quad:1:13: error: 'turn' is already a unit symbol");
    STR_EQ(load_error("%e: { base: \"pct\" }\n"), "a.quad:1:13: error: 'pct' is already a unit symbol");
    STR_EQ(load_error("%e: { base: \"kg\" }\n"), "a.quad:1:13: error: 'kg' is already a unit symbol");
    STR_EQ(load_error("%e: { base: \"mm\" }\n"), "a.quad:1:13: error: 'mm' is already a unit symbol");
    STR_EQ(load_error("%e: { base: \"coin\" }\n%g: { base: \"coin\" }\n"),
           "a.quad:2:13: error: the base dimension 'coin' is already declared");
    STR_EQ(load_error("%e: { base: \"two words\" }\n"),
           "a.quad:1:13: error: base is the unit symbol of a new base dimension, as a string: base: \"coin\"");
    STR_EQ(load_error("%e: { dimension: \"J\" } & #T\n#T: {}\n"),
           "a.quad:1:24: error: the body of the kind %e must be a struct of literal fields: { dimension: \"J\", scale: -3 }");
    STR_EQ(load_error("#T: { x: %nothing }\n"),
           "a.quad:1:10: error: unknown kind %nothing. Declare it: %nothing: { dimension: \"J\" }");
    STR_EQ(load_error("number: 5\n"), "a.quad:1:1: error: instance name number is a built-in type");
}

/* --- built-in kinds: the base dimensions, and the angle (ADR 0015 section 1) ---------------------- */

static int64_t quanta_at(Front *f, const char *instance, const char *path)
{
    const QfNode *n = at(f, inst(f, instance), path);
    CHECK(n && n->lo == n->hi && f->types.v[n->type].kind == TK_KIND);
    return n ? n->lo : 0;
}

void test_builtin_kinds(void)
{
    bool ok;
    const char *T = "#T: { l: %length, m: %mass, i: %current, th: %temperature, n: %amount, lum: %luminosity, "
                    "a: %angle, t: %time, r: %ratio, c: %count }\n";
    char src[2048];
    /* with no declaration, each base dimension has a kind with the default scale of ADR 0001 */
    snprintf(src, sizeof src, "%sx: #T & { l: 19mm, m: 350g, i: 2mA, th: 293.15K, n: 0.5mol, lum: 3cd, a: 90deg, "
             "t: 1.5s, r: 50pct, c: 12 }\ny: #T & { a: 0.5turn, r: 250ppm, l: 1.5km }\n", T);
    Front *f = load1(src, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(quanta_at(f, "x", "l") == 190);                    /* 0.1 mm */
        CHECK(quanta_at(f, "x", "m") == 350);                    /* 1 g */
        CHECK(quanta_at(f, "x", "i") == 2);                      /* 1 mA */
        CHECK(quanta_at(f, "x", "th") == 29315);                 /* 0.01 K */
        CHECK(quanta_at(f, "x", "n") == 500);                    /* 1 mmol */
        CHECK(quanta_at(f, "x", "lum") == 3000);
        CHECK(quanta_at(f, "x", "a") == 90000);                  /* 0.001 degree */
        CHECK(quanta_at(f, "x", "t") == 1500000);                /* 1 µs */
        CHECK(quanta_at(f, "x", "r") == 500000000);              /* 50 percent is 0.5 */
        CHECK(quanta_at(f, "x", "c") == 12);
        CHECK(quanta_at(f, "y", "a") == 180000);                 /* half a turn is 180 degrees */
        CHECK(quanta_at(f, "y", "r") == 250000);
        CHECK(quanta_at(f, "y", "l") == 15000000);
        static const char *const names[] = { "%length", "%mass", "%current", "%temperature", "%amount", "%luminosity", "%angle" };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            uint32_t k = qf_kind_lookup(f, qf_sym(f, names[i]));
            CHECK(k != QF_BOTTOM && f->kinds.v[k].builtin && !f->kinds.v[k].fixed && !f->kinds.v[k].conserved);
        }
        /* the angle is a base dimension: its own exponent, and no radian */
        uint32_t ka = qf_kind_lookup(f, qf_sym(f, "%angle"));
        const QDim *ad = &f->dims.v[f->kinds.v[ka].dimix].dim;
        CHECK(f->bases.n == 1 && !strcmp(f->bases.v[0], "deg") && ad->exp[Q_NSI] == 1 && !strcmp(f->kinds.v[ka].base, "deg"));
        CHECK(qf_unit_symbol(f, "deg") && qf_unit_symbol(f, "turn") && !qf_unit_symbol(f, "rad") && !qf_unit_symbol(f, "mdeg"));
    }
    qf_free(f);
    snprintf(src, sizeof src, "%sx: #T & { a: 1rad }\n", T);
    STR_EQ(load_error(src), "a.quad:2:14: error: unknown unit 'rad'");
    snprintf(src, sizeof src, "%sx: #T & { a: 5 }\n", T);
    STR_EQ(load_error(src), "a.quad:2:9: error: a: 5 has no unit, and %angle has the unit deg");
    snprintf(src, sizeof src, "%sx: #T & { l: 90deg }\n", T);
    STR_EQ(load_error(src), "a.quad:2:9: error: l: 90 deg is not of the kind %length. The unit of %length is m");
    snprintf(src, sizeof src, "%sx: #T & { l: 0.01mm }\n", T);
    STR_EQ(load_error(src), "a.quad:2:9: error: l: 0.00001 m is not a multiple of the smallest step of %length, which is "
                            "0.0001 m. For a smaller step, declare %length with scale: -5");
    snprintf(src, sizeof src, "%sx: #T & { r: 5J/kg*K }\n", T);
    STR_EQ(load_error(src), "a.quad:2:14: error: in the unit 'J/kg*K', a '*' after a '/' has two readings. Give each "
                            "divisor its own '/': J/kg/K");

    /* one declaration changes the scale, conserved or the rounding of a built-in base kind */
    snprintf(src, sizeof src, "%s%%length: { scale: -9 }\n%%mass: { dimension: \"kg\", scale: 0, conserved: true }\n"
             "%%angle: { scale: 0, rounding: \"floor\" }\nx: #T & { l: 0.01mm, m: 2kg, a: 90deg }\n", T);
    f = load1(src, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(quanta_at(f, "x", "l") == 10000);                  /* nanometres now */
        CHECK(quanta_at(f, "x", "m") == 2);
        CHECK(quanta_at(f, "x", "a") == 90);
        uint32_t km = qf_kind_lookup(f, qf_sym(f, "%mass")), kl = qf_kind_lookup(f, qf_sym(f, "%length")),
                 ka = qf_kind_lookup(f, qf_sym(f, "%angle"));
        CHECK(f->kinds.v[km].conserved && f->kinds.v[km].scale == 0 && !f->kinds.v[km].builtin);
        CHECK(!f->kinds.v[kl].conserved && f->kinds.v[kl].scale == -9 && f->kinds.v[kl].loc.line == 2);
        CHECK(f->kinds.v[ka].rounding == Q_ROUND_FLOOR && !strcmp(f->kinds.v[ka].base, "deg"));
        /* the kind is still the one kind of its name: the fields of #T have it */
        CHECK(at(f, inst(f, "x"), "m")->type == f->kinds.v[km].type);
    }
    qf_free(f);
    snprintf(src, sizeof src, "%s%%mass: { scale: 0 }\nx: #T & { m: 350g }\n", T);
    STR_EQ(load_error(src), "a.quad:3:9: error: m: 0.35 kg is not a multiple of the smallest step of %mass, which is 1 kg. "
                            "For a smaller step, declare %mass with scale: -2");
    /* a second declaration is an error, whatever it says */
    STR_EQ(load_error("%mass: { scale: 0 }\n%mass: { scale: 0 }\n"), "a.quad:2:1: error: the kind %mass is already declared at a.quad:1");
    STR_EQ(load_error("%mass: { conserved: true }\n%mass: { conserved: false }\n"),
           "a.quad:2:1: error: the kind %mass is already declared at a.quad:1");
    const char *two[] = { "%length: { scale: -6 }\n", "%length: { scale: -9 }\n" };
    f = load(two, 2, &ok);
    CHECK(!ok && f->diags.n == 1);
    if (f->diags.n == 1) STR_EQ(f->diags.v[0].msg, "b.quad:1:1: error: the kind %length is already declared at a.quad:1");
    qf_free(f);
    /* the dimension stays */
    STR_EQ(load_error("%mass: { dimension: \"m\" }\n"),
           "a.quad:1:21: error: the built-in kind %mass keeps its dimension: a declaration can change its scale, conserved and rounding");
    STR_EQ(load_error("%mass: { base: \"lump\" }\n"),
           "a.quad:1:10: error: the built-in kind %mass keeps its dimension: a declaration can change its scale, conserved and rounding");
    STR_EQ(load_error("%angle: { dimension: \"1\" }\n"),
           "a.quad:1:22: error: the built-in kind %angle keeps its dimension: a declaration can change its scale, conserved and rounding");
    STR_EQ(load_error("%angle: { dimension: \"deg\", scale: -6 }\n"), "loaded");

    /* a package can have its own kind with a built-in name. Another package that
     * writes the plain name gets the built-in kind, not that package's. */
    const char *pk[] = { "package p\n%mass: { dimension: \"kg\", scale: 0 }\n#A: { m: %mass }\na: #A & { m: 2kg }\n",
                         "package q\n#B: { w: %mass, v: p.%mass }\nb: #B & { w: 350g, v: 3kg }\n" };
    f = load(pk, 2, &ok);
    CHECK(ok);
    if (ok) {
        CHECK(quanta_at(f, "a", "m") == 2);                      /* p's kind: kilograms */
        CHECK(quanta_at(f, "b", "w") == 350);                    /* the built-in kind: grams */
        CHECK(quanta_at(f, "b", "v") == 3);
        STR_EQ(node_text(f, inst(f, "b")), "#B & { v: p.%mass & 3, w: %mass & 0.35 }");
    }
    qf_free(f);
}

/* --- a struct states its type (ADR 0015 section 5) ------------------------------------------------- */

void test_struct_states_type(void)
{
    const char *T = "#Tool: { size: %length, mass: %mass }\n#Wrench: #Tool & { jaw: %length }\n"
                    "#Box: { best: #Tool, any: _, all: [...#Tool], loose: [...] }\n";
    char src[2048];
#define LOADS(body, want)                                                              \
    do {                                                                               \
        snprintf(src, sizeof src, "%s%s\n", T, body);                                  \
        STR_EQ(load_error(src), want);                                                 \
    } while (0)
    /* where the place has the type, a struct needs no more */
    LOADS("a: #Tool & { size: 19mm }", "loaded");
    LOADS("a: { size: 19mm } & #Tool", "loaded");                 /* the order of the conjuncts does not count */
    LOADS("a: #Wrench & { size: 19mm }", "loaded");               /* a subtype has the fields too */
    LOADS("a: #Tool\na: { size: 19mm }", "loaded");               /* the type from its other declaration */
    LOADS("a: { size: 19mm }\na: #Tool", "loaded");
    LOADS("b: #Box & { best: { size: 19mm } }", "loaded");        /* the declared type of the field */
    LOADS("b: #Box & { all: [{ size: 19mm }, { mass: 2kg }] }", "loaded");    /* the element type of the list */
    LOADS("a: #Tool & { size: 19mm }\nb: a & { mass: 2kg }", "loaded");       /* the type of an instance */
    LOADS("a: #Tool & { size: 19mm }\nb: { mass: 2kg } & a", "loaded");
    LOADS("b: #Box & { any: #Tool & { size: 19mm } }", "loaded"); /* stated */
    LOADS("b: #Box & { any: { size: 19mm } & #Tool }", "loaded"); /* stated after the struct: the same */
    LOADS("b: #Box & { loose: [#Tool & { size: 19mm }] }", "loaded");
    LOADS("b: #Box & { best: #Wrench & { jaw: 250mm } }", "loaded");
    LOADS("b: #Box & { any: {} }", "loaded");                     /* no fields: no type to get */
    LOADS("#Crate: #Box & { best: { size: 19mm } }", "loaded");   /* in a type: the field of its supertype */

    /* where the place has no type, the fields do not give one */
    LOADS("a: { size: 19mm }",
          "a.quad:4:4: error: this struct stands where no type is known, and a struct does not get a type from its "
          "fields. Its fields are fields of #Tool: write #Tool & { ... }");
    LOADS("b: #Box & { any: { size: 19mm } }",
          "a.quad:4:18: error: this struct stands where no type is known, and a struct does not get a type from its "
          "fields. Its fields are fields of #Tool: write #Tool & { ... }");
    LOADS("b: #Box & { loose: [{ size: 19mm }] }",
          "a.quad:4:21: error: this struct stands where no type is known, and a struct does not get a type from its "
          "fields. Its fields are fields of #Tool: write #Tool & { ... }");
    LOADS("#Vise: #Tool & { head: { size: .size } }",
          "a.quad:4:24: error: this struct stands where no type is known, and a struct does not get a type from its "
          "fields. Its fields are fields of #Tool: write #Tool & { ... }");
    /* and the fields do not make the type of the place more specific */
    LOADS("b: #Box & { best: { jaw: 250mm } }",
          "a.quad:4:19: error: this struct has fields of #Wrench, but it stands where the type is #Tool: write #Wrench & { ... }");
    LOADS("b: #Box & { all: [{ jaw: 250mm }] }",
          "a.quad:4:19: error: this struct has fields of #Wrench, but it stands where the type is #Tool: write #Wrench & { ... }");
    LOADS("a: #Tool & { jaw: 250mm }",
          "a.quad:4:12: error: this struct has fields of #Wrench, but it stands where the type is #Tool: write #Wrench & { ... }");
    LOADS("a: #Box & { size: 19mm }",
          "a.quad:4:11: error: this struct has fields of #Tool, but it stands where the type is #Box: write #Tool & { ... }");
#undef LOADS
    /* a stated type gives the same structure as the inferred one gave before */
    snprintf(src, sizeof src, "%sb: #Box & { best: { size: 19mm }, any: #Wrench & { jaw: 250mm } }\n", T);
    STR_EQ(inst_text(src, "b"), "#Box & { all: [...#Tool], any: #Wrench & { jaw: %length & 0.25, mass: %mass, size: %length }, "
                                "best: #Tool & { mass: %mass, size: %length & 0.019 }, loose: #List }");
    /* a reference is a path that the author wrote: it can go through a field with no type */
    snprintf(src, sizeof src, "%sb: #Box & { best: { size: .any.size } }\n", T);
    STR_EQ(inst_text(src, "b"), "#Box & { all: [...#Tool], any: #Tool & { mass: %mass, size: $1 & %length }, "
                                "best: #Tool & { mass: %mass, size: $1 }, loose: #List }");
}

/* --- a literal converts when it meets a kind (ADR 0015 section 3) -------------------------------- */

void test_literal_conversion(void)
{
    const char *T = "#T: { x: %length, e: %energy, n: number, any: _, c: %count }\n";
    char src[4096];
#define INST(body, want)                                                               \
    do {                                                                               \
        snprintf(src, sizeof src, "%s%sa: %s\n", KINDS, T, body);                      \
        STR_EQ(inst_text(src, "a"), want);                                             \
    } while (0)

    /* the value is in quanta of the kind: 0.1 mm for a length */
    bool ok;
    snprintf(src, sizeof src, "%s%sa: #T & { x: 19mm, e: 5kJ, c: 12 }\n", KINDS, T);
    Front *f = load1(src, &ok);
    CHECK(ok);
    if (ok) {
        const QfNode *x = at(f, inst(f, "a"), "x"), *e = at(f, inst(f, "a"), "e"), *c = at(f, inst(f, "a"), "c");
        CHECK(x && f->types.v[x->type].kind == TK_KIND && x->lo == 190 && x->hi == 190 && x->exp == 0);
        CHECK(e && e->lo == 5000000 && e->hi == 5000000);
        CHECK(c && c->lo == 12 && c->hi == 12);
        const QfNode *n = at(f, inst(f, "a"), "n");
        CHECK(n && n->type == f->tnumber && n->lo == Q_NINF && n->hi == Q_INF);
    }
    qf_free(f);

    INST("#T & { x: 19mm }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & 0.019 }");
    /* the order of the conjuncts does not count */
    INST("{ x: 19mm } & #T", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & 0.019 }");
    INST("#T & { x: 19mm & %length }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & 0.019 }");
    INST("#T & { any: %length & 19mm }", "#T & { any: %length & 0.019, c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { any: 19mm & %length }", "#T & { any: %length & 0.019, c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { any: 1.9cm & 19mm & 0.019m }", "#T & { any: dim[m] & 0.019, c: %count, e: %energy, n: number, x: %length }");
    /* a value with only a dimension is valid in a structure */
    INST("#T & { n: 2.5m/s }", "#T & { any: _, c: %count, e: %energy, n: dim[m s^-1] & 2.5, x: %length }");
    INST("#T & { n: 2.5 & >=1 }", "#T & { any: _, c: %count, e: %energy, n: 2.5, x: %length }");
    INST("#T & { any: %energy / %time }", "#T & { any: dim[m^2 kg s^-3], c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { any: %energy / %time & 5W }", "#T & { any: dim[m^2 kg s^-3] & 5, c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { any: %energy * %length / %length & %energy & 2J }",
         "#T & { any: %energy & 2, c: %count, e: %energy, n: number, x: %length }");
    /* bounds */
    INST("#T & { x: >=10mm & <=24mm }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & >=0.01 & <=0.024 }");
    INST("#T & { x: >10mm & <24mm }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & >=0.0101 & <=0.0239 }");
    INST("#T & { x: >=0.15mm & <0.4mm }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & >=0.0002 & <=0.0003 }");
    INST("#T & { any: >10mm & <24mm }", "#T & { any: dim[m] & >0.01 & <0.024, c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { any: >10mm & <24mm & >=1cm & <=2cm }",
         "#T & { any: dim[m] & >0.01 & <=0.02, c: %count, e: %energy, n: number, x: %length }");
    INST("#T & { x: >=10mm } & { x: 12mm }", "#T & { any: _, c: %count, e: %energy, n: number, x: %length & 0.012 }");
    INST("#T & { c: >=2 & <5 }", "#T & { any: _, c: %count & >=2 & <=4, e: %energy, n: number, x: %length }");
    INST("#T & { x: 3e-3m, e: 1e3J }", "#T & { any: _, c: %count, e: %energy & 1000, n: number, x: %length & 0.003 }");

    /* errors: each names the value and the reason */
    INST("#T & { x: 0.15mm }", "a.quad:5:9: error: x: 0.00015 m is not a multiple of the smallest step of %length, which is "
                               "0.0001 m. For a smaller step, declare %length with scale: -5");
    INST("#T & { c: 2.5 }", "a.quad:5:9: error: c: 2.5 is not a multiple of the smallest step of %count, which is 1");
    INST("#T & { x: >0.11mm & <0.19mm }", "a.quad:5:9: error: x: no multiple of the smallest step of %length, which is 0.0001 m, "
                                           "is in the range > 0.00011 m and < 0.00019 m. For a smaller step, declare %length "
                                           "with a smaller scale than -4");
    INST("#T & { x: 1e30m }", "a.quad:5:9: error: x: 1e30 m is too large for %length, which has the smallest step 0.0001 m. "
                              "For larger values, declare %length with a larger scale than -4");
    INST("#T & { x: 5kg }", "a.quad:5:9: error: x: 5 kg is not of the kind %length. The unit of %length is m");
    INST("#T & { x: 5 }", "a.quad:5:9: error: x: 5 has no unit, and %length has the unit m");
    INST("#T & { x: 3min }", "a.quad:5:9: error: x: 180 s is not of the kind %length. The unit of %length is m");
    INST("#T & { x: %mass }", "a.quad:5:9: error: x: %length and %mass are different kinds");
    INST("#T & { x: 12mm } & { x: 13mm }", "a.quad:5:21: error: x: 0.012 m and 0.013 m are different values");
    INST("#T & { any: 12mm & 13mm }", "a.quad:5:21: error: any: 0.012 m and 0.013 m are different values");
    INST("#T & { any: 1e30m & 1e-30m }", "a.quad:5:22: error: any: 1e30 m and 0.000000000000000000000000000001 m are too far apart "
                                         "for a comparison");
    INST("#T & { x: >=10mm & <5mm }", "a.quad:5:21: error: x: >= 0.01 m and < 0.005 m have no value in common");
    INST("#T & { n: \"text\" }", "a.quad:5:9: error: n: \"text\" is not a number");
    /* the checks that ADR 0014 gave to this stage */
    INST("#T & { n: 0x10 }", "a.quad:5:14: error: unknown unit 'x10'");
    INST("#T & { n: 10m/x }", "a.quad:5:14: error: unknown unit 'x' in 'm/x'. If 'x' is a name, put spaces around the "
                              "operator, because an operator with a name directly after it continues the unit");
    INST("#T & { any: 0.1/s }", "a.quad:5:20: error: unknown name 's'. 's' is a unit: for a value for each s, write the unit "
                                "directly after the number with a negative exponent, for example 0.1s^-1");
    INST("#T & { any: 2 * wrench }", "a.quad:5:20: error: the name 'wrench' cannot stand in an expression: a value of the "
                                     "structure is a reference, for example .size");
    INST("#T & { n: 1m | 2m }", "a.quad:5:17: error: a disjunction of numbers is not a type; use a range, or an enum of atoms");
#undef INST
    /* decimals in structures: copies keep the power of ten and the open bounds */
    f = load1(with_kinds("#D: { d: >0.5m & <2.5m, any: _ }\n#U: { t: #D }\nu: #U\n"
                         "p: #D & { any: 0.15mm }\nq: #D & { any: %length & 0.2mm }\n"
                         "g1: #D & { any: >=1m }\ns1: #D & { any: 0.5m }\ns2: #D & { any: 2m }\ns3: #D & { any: >=0.5m }\n"
                         "r1: #D & { any: >=0.15mm }\nr2: #D & { any: %length & >=0.2mm }\nr3: #D & { any: %length & >=0.1mm }\n"
                         "m5: #D & { any: 5m }\nt5: #D & { any: 5s }\nn150: #D & { any: 1.50m }\ntxt: #D & { any: \"hello\" }\n"
                         "lit: 1.50m\nrange: >=10.0mm\n"), &ok);
    CHECK(ok);
    if (ok) {
        STR_EQ(node_text(f, inst(f, "u")), "#U & { t: #D & { any: _, d: dim[m] & >0.5 & <2.5 } }");
        const QfNode *d = at(f, inst(f, "u"), "t.d");
        CHECK(d && d->lo == 5 && d->hi == 25 && d->exp == -1 && (d->flags & NF_LO_OPEN) && (d->flags & NF_HI_OPEN));
        /* a literal is stored with no common factor of ten: 1.50 m and 1.5 m are one value */
        const QfNode *n150 = at(f, inst(f, "n150"), "any");
        CHECK(n150 && n150->lo == 15 && n150->hi == 15 && n150->exp == -1);
        const QfNode *lit = inst(f, "lit"), *range = inst(f, "range");            /* also with no unification */
        CHECK(lit && lit->lo == 15 && lit->hi == 15 && lit->exp == -1);
        CHECK(range && range->lo == 1 && range->hi == Q_INF && range->exp == -2);
        /* a string that no enum lists is a text value */
        const QfNode *txt = at(f, inst(f, "txt"), "any");
        CHECK(txt && txt->type == f->ttext && (txt->flags & NF_VALUE) && txt->lo == txt->hi);
        STR_EQ((const char *) (intptr_t) (txt ? txt->lo : 0), "\"hello\"");
        QfOp *op = qf_op_new(f);
        QfStore *out = qf_store_new();
        bool fwd, bwd;
#define ANY(name) at(f, inst(f, name), "any")
        /* 0.15 mm is no value of the kind, so it does not subsume 0.2 mm of the kind */
        qf_subsumes(op, ANY("p"), ANY("q"), &fwd, &bwd);
        CHECK(!fwd && !bwd);
        qf_subsumes(op, ANY("g1"), ANY("s1"), &fwd, &bwd);
        CHECK(!fwd && !bwd);                                     /* 0.5 m is not >= 1 m */
        qf_subsumes(op, ANY("g1"), ANY("s2"), &fwd, &bwd);
        CHECK(fwd && !bwd);
        qf_subsumes(op, ANY("g1"), ANY("s3"), &fwd, &bwd);
        CHECK(!fwd && bwd);                                      /* >= 0.5 m is the more general */
        /* a decimal range subsumes a range of a kind when every quantum in the second is in the first */
        qf_subsumes(op, ANY("r1"), ANY("r2"), &fwd, &bwd);
        CHECK(fwd && !bwd);
        qf_subsumes(op, ANY("r1"), ANY("r3"), &fwd, &bwd);
        CHECK(!fwd && !bwd);
        /* generalization: a hull in one dimension, any number across two dimensions */
        STR_EQ(node_text(f, qf_generalize(op, ANY("m5"), ANY("t5"), out)), "number");
        STR_EQ(node_text(f, qf_generalize(op, ANY("m5"), ANY("s2"), out)), "dim[m] & >=2 & <=5");
        STR_EQ(node_text(f, qf_generalize(op, ANY("q"), ANY("s2"), out)), "dim[m] & >=0.0002 & <=2");
        STR_EQ(node_text(f, qf_generalize(op, ANY("r2"), ANY("r3"), out)), "%length & >=0.0001");
        STR_EQ(node_text(f, qf_generalize(op, ANY("g1"), ANY("txt"), out)), "_");
        STR_EQ(node_text(f, qf_generalize(op, ANY("txt"), ANY("txt"), out)), "\"hello\"");
#undef ANY
        /* an operation gives back all of its scratch */
        CHECK(op->scratch.n == 0);
        qf_store_free(out);
        qf_op_free(op);
    }
    qf_free(f);

    STR_EQ(load_error("let s = 2\n#T: { x: _ }\n"),
           "a.quad:1:5: error: let s: 's' is a unit symbol. Give the let another name, because a number directly "
           "before /s or *s takes it as a unit");
    STR_EQ(load_error("let size = 2\n#T: { x: size }\n"), "loaded");
}

/* --- the fixture: a small thermal world, with packages -------------------------------------------- */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = malloc((size_t) n + 1);
    size_t got = s ? fread(s, 1, (size_t) n, f) : 0;
    fclose(f);
    if (!s) return NULL;
    s[got] = 0;
    return s;
}

#define NFIX 4
static const char *const fixture_files[NFIX] = { "physics.quad", "world.quad", "shop.quad", "tools.quad" };

static bool fixture_sources(char *src[NFIX])
{
    bool all = true;
    for (int i = 0; i < NFIX; i++) {
        char path[256];
        snprintf(path, sizeof path, "tests/data/resolve/%s", fixture_files[i]);
        src[i] = read_file(path);
        CHECK(src[i] != NULL);
        all = all && src[i];
    }
    return all;
}

static Front *load_fixture(void)
{
    char *src[NFIX];
    if (!fixture_sources(src)) return NULL;
    Front *f = qf_new();
    bool ok = qf_load(f, fixture_files, (const char *const *) src, NFIX);
    CHECK(ok);
    for (size_t i = 0; i < f->diags.n; i++) fprintf(stderr, "  %s\n", f->diags.v[i].msg);
    for (int i = 0; i < NFIX; i++) free(src[i]);
    if (!ok) { qf_free(f); return NULL; }
    return f;
}

void test_resolve_fixture(void)
{
    Front *f = load_fixture();
    if (!f) return;
    CHECK(f->loaded && f->hash != 0);

    /* the hierarchy */
    uint32_t room = type_lookup(f, qf_sym(f, "#LumpedRoom")), thermal = type_lookup(f, qf_sym(f, "#Thermal")),
             phen = type_lookup(f, qf_sym(f, "#Phenomenon")), port = type_lookup(f, qf_sym(f, "#ThermalPort")),
             wall = type_lookup(f, qf_sym(f, "#Wall"));
    CHECK(room != QF_BOTTOM && thermal != QF_BOTTOM && phen != QF_BOTTOM && port != QF_BOTTOM && wall != QF_BOTTOM);
    CHECK(is_subtype(f, room, thermal) && is_subtype(f, room, phen) && is_subtype(f, port, thermal));
    CHECK(!is_subtype(f, thermal, room) && !is_subtype(f, wall, thermal));
    CHECK(meet_pure(f, room, thermal) == room && meet_pure(f, room, wall) == QF_BOTTOM);
    CHECK(meet_pure(f, room, port) == QF_BOTTOM);
    CHECK(meet(f, room, thermal) == room && meet(f, wall, room) == QF_BOTTOM);
    /* a conjunction of unrelated types gets a generated type, and so does a meet of two of those */
    uint32_t made = type_lookup(f, qf_sym(f, "shop.#Made")), phys = type_lookup(f, qf_sym(f, "shop.#Physical"));
    CHECK(made != QF_BOTTOM && phys != QF_BOTTOM);
    uint32_t mp = meet_pure(f, made, phys);
    CHECK(mp != QF_BOTTOM && f->types.v[mp].kind == TK_GLB);
    STR_EQ(f->types.v[mp].name, "glb[shop.#Made & shop.#Physical,shop.#Made & shop.#Priced & shop.#Tool]");
    /* a packaged type is found by its plain name where only one package has it */
    CHECK(type_lookup(f, qf_sym(f, "#Made")) == made);
    CHECK(type_pkg(f, made) == qf_sym(f, "shop") && type_pkg(f, room) == NULL);
    /* features: each has one introducing type, and a subtype has its supertypes' features */
    uint32_t fenergy = feat_lookup(f, qf_sym(f, "energy")), fwest = feat_lookup(f, qf_sym(f, "west"));
    CHECK(fenergy != QF_BOTTOM && f->feats.v[fenergy].intros.n == 1 && introduces(f, thermal, fenergy));
    CHECK(f->feats.v[fwest].intros.n == 1 && introduces(f, room, fwest) && !introduces(f, room, fenergy));
    CHECK(approp(f, room, fenergy) && approp(f, port, fenergy) && !approp(f, wall, fenergy) && !approp(f, port, fwest));
    /* an enum, and a disjunction of structures */
    uint32_t material = type_lookup(f, qf_sym(f, "shop.#Material")), steel = type_lookup(f, qf_sym(f, "\"steel\""));
    CHECK(steel != QF_BOTTOM && is_subtype(f, steel, material) && is_subtype(f, material, f->atomroot));
    uint32_t disj = type_lookup(f, qf_sym(f, "(#Circle | #Square)")), circle = type_lookup(f, qf_sym(f, "#Circle"));
    CHECK(disj != QF_BOTTOM && f->types.v[disj].kind == TK_DISJ && f->types.v[disj].isdisj);
    uint32_t alt = type_lookup(f, qf_sym(f, "(#Circle | #Square)/#Circle"));
    CHECK(alt != QF_BOTTOM && f->types.v[alt].kind == TK_ALT && is_subtype(f, alt, disj) && is_subtype(f, alt, circle));
    CHECK(is_value(f, f->tnumber) && is_value(f, f->ttext) && is_value(f, f->tconstraint) && !is_value(f, room));

    /* an instance: several declarations, references that share nodes, and exact values */
    const QfNode *a = inst(f, "roomA");
    STR_EQ(node_text(f, a),
           "#LumpedRoom & { capacity: $1 & %capacity & 1000, door: \"open\", energy: $2 & %energy & 293150, "
           "evaluator: text, laws: [constraint, ...constraint] & { first: .energy == (.capacity * .temperature), rest: #Null }, "
           "temperature: $3 & %temperature & 293.15, west: #ThermalPort & { capacity: $1, energy: $2, temperature: $3 } }");
    const QfNode *e = at(f, a, "energy");
    CHECK(e && e->lo == 293150000 && e->hi == 293150000);                       /* millijoules */
    CHECK(at(f, a, "west.energy") == e);                                        /* one node */
    const QfNode *t = at(f, a, "temperature");
    CHECK(t && t->lo == 293150000 && t->hi == 293150000);                       /* microkelvins */
    CHECK(at(f, a, "west.west") == NULL && at(f, a, "energy.west") == NULL);
    const QfNode *b = inst(f, "roomB");
    const QfNode *bt = at(f, b, "temperature"), *be = at(f, b, "energy");
    CHECK(bt && bt->lo == 273150000 && bt->hi == 373150000);
    CHECK(be && be->lo == 0 && be->hi == Q_INF);                                /* the type's bound */
    STR_EQ(node_text(f, at(f, b, "door")), "(\"closed\" | \"open\")");
    /* a path into another instance gives a copy: the wall's ports do not share with the rooms */
    const QfNode *w = inst(f, "wall");
    const QfNode *hot = at(f, w, "hot.energy");
    CHECK(hot && hot != e && hot->lo == 293150000);
    STR_EQ(node_text(f, at(f, w, "cold")),
           "#ThermalPort & { capacity: %capacity & 2000, energy: %energy & >=0, temperature: %temperature & >=273.15 & <=373.15 }");
    STR_EQ(node_text(f, at(f, w, "power")), "dim[m^2 kg s^-3]");
    /* packages, a let, generated types, a base dimension */
    STR_EQ(node_text(f, inst(f, "hammer")),
           "shop.#Made & shop.#Priced & shop.#Tool & { grip: \"hard\", mass: shop.%mass & 0.5, of: \"steel\", "
           "per: dim[kg^-1 coin], price: shop.%coin & 12.5 }");
    STR_EQ(node_text(f, inst(f, "anvil")),
           "glb[shop.#Made & shop.#Physical,shop.#Made & shop.#Priced & shop.#Tool] & { mass: shop.%mass & >=5, of: \"steel\" }");
    const QfNode *price = at(f, inst(f, "hammer"), "price");
    CHECK(price && price->lo == 1250);                                          /* hundredths of a coin */
    /* a recursive type, a disjunction of structures, a typed list */
    STR_EQ(node_text(f, inst(f, "chain")),
           "#Chain & { label: \"a\", next: #Chain & { label: \"b\", next: #Chain & { label: \"c\", next: #Chain } } }");
    STR_EQ(node_text(f, inst(f, "round")), "#Shaped & { form: (#Circle | #Square)/#Circle & { radius: tools.%length & 2 } }");
    const QfNode *kit = inst(f, "kit");
    STR_EQ(node_text(f, at(f, kit, "owner")), "\"Urist\"");
    STR_EQ(node_text(f, at(f, kit, "items.rest.first.price")), "shop.%coin & 12.5");
    STR_EQ(node_text(f, at(f, kit, "items.rest.rest")), "#Null");
    const QfNode *ks = at(f, kit, "items.first.size");
    CHECK(ks && ks->lo == 190 && at(f, kit, "items.first.head.width") == ks);

    /* constraint records: the expression, its path, and the path of each reference */
    const TypeInfo *rt = &f->types.v[room];
    CHECK(rt->ariths.n == 1);
    if (rt->ariths.n == 1) {
        const ArithRec *r = &rt->ariths.v[0];
        Buf p = { 0 };
        path_format(f, &p, r->path.v, r->path.n);
        STR_EQ(p.s, "laws.first");
        p.len = 0;
        qf_print(&p, r->expr);
        STR_EQ(p.s, ".energy == (.capacity * .temperature)");
        CHECK(r->expr->k == X_CMP && r->tags.n == 3);
        if (r->tags.n == 3) {
            STR_EQ(r->tags.v[0].name, ".energy");
            CHECK(r->tags.v[0].path.n == 1 && r->tags.v[0].path.v[0] == fenergy);
            STR_EQ(r->tags.v[2].name, ".temperature");
        }
        qf_buf_free(&p);
        /* the node of the comparison refers to the same expression */
        const QfNode *law = at(f, a, "laws.first");
        CHECK(law && law->type == f->tconstraint && (const X *) (intptr_t) law->lo == r->expr && law->lo == law->hi);
    }
    const TypeInfo *wt = &f->types.v[wall];
    CHECK(wt->ariths.n == 1 && wt->ariths.v[0].tags.n == 4);
    if (wt->ariths.n == 1 && wt->ariths.v[0].tags.n == 4) {
        Buf p = { 0 };
        path_format(f, &p, wt->ariths.v[0].tags.v[2].path.v, wt->ariths.v[0].tags.v[2].path.n);
        STR_EQ(p.s, "hot.temperature");
        qf_buf_free(&p);
    }
    /* every record of a structure: of its instance, then of the type of each node */
    QfRecUses uses = { 0 };
    qf_records(f, a, &f->insts.v[qf_instance(f, "roomA")].ariths, &uses);
    CHECK(uses.n == 1 && uses.v[0].rec == &rt->ariths.v[0] && uses.v[0].prefix.n == 0);
    qf_records_free(&uses);
    qf_free(f);

    /* a record in a field: the record is at the path of the type's node */
    bool ok;
    f = load1(with_kinds("#Leg: { len: %length, twice: .len * 2, laws: [...constraint] }\n"
                         "#Table: { leg: #Leg, spare: #Leg }\n"
                         "t: #Table & { leg: { len: 1m, laws: [ .leg.len > 0m ] } }\n"
                         "u: #Table & { spare: t.leg }\n"
                         "v: #Table & { leg: t.leg }\n"
                         "w: t & { spare: { len: 2m } }\n"
                         "#Holder: { item: #Table }\n"
                         "h: #Holder & { item: t }\n"
                         "s: #Table & { spare: .leg }\n"), &ok);
    CHECK(ok);
    if (ok) {
        uint32_t leg = type_lookup(f, qf_sym(f, "#Leg")), fleg = feat_lookup(f, qf_sym(f, "leg")),
                 fspare = feat_lookup(f, qf_sym(f, "spare"));
        CHECK(f->types.v[leg].ariths.n == 1 && f->types.v[leg].ariths.v[0].expr->k == X_ARITH);
        STR_EQ(node_text(f, inst(f, "t")),
               "#Table & { leg: #Leg & { laws: [constraint, ...constraint] & { first: .leg.len > 0m, rest: #Null }, "
               "len: %length & 1, twice: _ }, spare: #Leg & { laws: [...constraint], len: %length, twice: _ } }");
        /* the instance's own record: the comparison in t.leg.laws, with the reference at leg.len */
        const InstInfo *ti = &f->insts.v[qf_instance(f, "t")];
        CHECK(ti->ariths.n == 1);
        if (ti->ariths.n == 1) {
            Buf p = { 0 };
            path_format(f, &p, ti->ariths.v[0].path.v, ti->ariths.v[0].path.n);
            STR_EQ(p.s, "leg.laws.first");
            p.len = 0;
            CHECK(ti->ariths.v[0].tags.n == 1);
            if (ti->ariths.v[0].tags.n == 1) path_format(f, &p, ti->ariths.v[0].tags.v[0].path.v, ti->ariths.v[0].tags.v[0].path.n);
            STR_EQ(p.s, "leg.len");
            qf_buf_free(&p);
        }
        /* the records of the structure: the instance's, and #Leg's at leg and at spare */
        QfRecUses us = { 0 };
        qf_records(f, ti->node, &ti->ariths, &us);
        CHECK(us.n == 3);
        if (us.n == 3) {
            CHECK(us.v[0].rec == &ti->ariths.v[0] && us.v[0].prefix.n == 0);
            CHECK(us.v[1].rec == &f->types.v[leg].ariths.v[0] && us.v[1].prefix.n == 1 && us.v[1].prefix.v[0] == fleg);
            CHECK(us.v[2].rec == &f->types.v[leg].ariths.v[0] && us.v[2].prefix.n == 1 && us.v[2].prefix.v[0] == fspare);
        }
        qf_records_free(&us);
        /* a whole instance by name brings its records, moved to where the copy stands */
        const InstInfo *wi = &f->insts.v[qf_instance(f, "w")];
        CHECK(wi->ariths.n == 1);
        if (wi->ariths.n == 1) {
            Buf p = { 0 };
            path_format(f, &p, wi->ariths.v[0].path.v, wi->ariths.v[0].path.n);
            STR_EQ(p.s, "leg.laws.first");
            qf_buf_free(&p);
            CHECK(wi->ariths.v[0].expr == ti->ariths.v[0].expr);
        }
        STR_EQ(node_text(f, at(f, inst(f, "w"), "spare.len")), "%length & 2");
        /* at a field, the paths of the record and of its references get the path of the field */
        const InstInfo *hi = &f->insts.v[qf_instance(f, "h")];
        CHECK(hi->ariths.n == 1 && hi->ariths.v[0].tags.n == 1);
        if (hi->ariths.n == 1 && hi->ariths.v[0].tags.n == 1) {
            Buf p = { 0 };
            path_format(f, &p, hi->ariths.v[0].path.v, hi->ariths.v[0].path.n);
            STR_EQ(p.s, "item.leg.laws.first");
            p.len = 0;
            path_format(f, &p, hi->ariths.v[0].tags.v[0].path.v, hi->ariths.v[0].tags.v[0].path.n);
            STR_EQ(p.s, "item.leg.len");
            qf_buf_free(&p);
        }
        /* one node at two paths gives the records of its type once, at its first path */
        const InstInfo *si = &f->insts.v[qf_instance(f, "s")];
        CHECK(at(f, si->node, "leg") == at(f, si->node, "spare"));
        QfRecUses shared = { 0 };
        qf_records(f, si->node, &si->ariths, &shared);
        CHECK(shared.n == 1 && shared.v[0].prefix.n == 1 && shared.v[0].prefix.v[0] == fleg);
        qf_records_free(&shared);
        /* inheritance: a subtype has the records of its supertypes, once */
    }
    qf_free(f);
    f = load1(with_kinds("#A: { x: %length, y: .x * 2 }\n#B: #A & { z: .x + .y }\n#C: #A & { w: _ }\n#D: #B & #C\n"), &ok);
    CHECK(ok);
    if (ok) {
        CHECK(f->types.v[type_lookup(f, qf_sym(f, "#A"))].ariths.n == 1);
        CHECK(f->types.v[type_lookup(f, qf_sym(f, "#B"))].ariths.n == 2);
        CHECK(f->types.v[type_lookup(f, qf_sym(f, "#C"))].ariths.n == 1);
        CHECK(f->types.v[type_lookup(f, qf_sym(f, "#D"))].ariths.n == 2);
    }
    qf_free(f);
}

/* --- errors of a load ------------------------------------------------------------------------------ */

void test_resolve_errors(void)
{
    /* what a later stage brings: each error names its issue */
    STR_EQ(load_error("#A: { a: _ }\n#B: { b: _ }\nx: #C & { c: #A | #B }\n#C: { c: _ }\n"),
           "a.quad:3:17: error: a disjunction of structures stands in a type's body. An instance has one structure: "
           "give the alternatives to a type, and name the type here");

    /* the hierarchy */
    STR_EQ(load_error("#A: { x: _ }\n#B: { x: _ }\n"), "loaded");            /* two types, one field name (ADR 0017) */
    STR_EQ(load_error("#A: #B\n#B: #A\n"), "a.quad:1:1: error: the type hierarchy has a cycle through #A");
    STR_EQ(load_error("#A: #Nope & { x: _ }\n"), "a.quad:1:5: error: #Nope is not a type that #A can inherit from");
    STR_EQ(load_error("a: { x: 1 }\n"), "a.quad:1:6: error: unknown field 'x': no type declares a field with this name");
    STR_EQ(load_error("#A: { x: _ }\na: #A & { x: nope }\n"), "a.quad:2:14: error: unknown name nope");
    STR_EQ(load_error("#A: { x: _ }\n#B: { y: _ }\na: #A & { y: 1 }\n"),
           "a.quad:3:9: error: this struct has fields of #B, but it stands where the type is #A: write #B & { ... }");
    STR_EQ(load_error("#A: { x: _ }\na: #A & { x: a.x }\n"), "a.quad:2:1: error: instance a refers to itself");
    STR_EQ(load_error("#A: { x: \"p\" | \"q\" }\na: #A & { x: \"r\" }\n"),
           "a.quad:2:9: error: x: \"r\" is not one of \"p\" | \"q\"");
    STR_EQ(load_error("#A: { x: text }\na: #A & { x: \"r\" }\na: { x: \"s\" }\n"),
           "a.quad:3:1: error: a: x: \"r\" and \"s\" are different values, with its other declarations");
    STR_EQ(load_error("#A: { x: #B, w: _ }\n#B: { y: #C, z: _ }\n#C: { v: _ }\na: #A & { w: .x.y.z }\n"),
           "a.quad:4:14: error: the reference .x.y.z: x.y is #C, which has no field z");
    /* a field with no type is open: the reference gives it the fields of its path */
    STR_EQ(load_error("#A: { x: #B, w: _ }\n#B: { y: _, z: _ }\n#C: { v: _ }\na: #A & { w: .x.y.v.z }\n"), "loaded");
    STR_EQ(load_error("#A: { x: _, y: _ }\na: #A & { x: .y + $nowhere }\n"),
           "a.quad:2:17: error: a: $nowhere in an expression is not bound to a field");
    STR_EQ(load_error("let room = 1\n#A: { x: _ }\nroom: #A\n"),
           "a.quad:1:5: error: let room: an instance has this name, and a let may not hide it");
    /* packages */
    STR_EQ(load_error("package p\nx: q.#T\n"), "a.quad:2:4: error: unknown name q.#T");

    /* every error of a content, sorted by location; a content with an error has no result */
    bool ok;
    const char *srcs[] = { "#A: { x: _ }\nb: #A & { x: nope }\n", "a: #A & { x: also_nope }\n" };
    Front *f = load(srcs, 2, &ok);
    CHECK(!ok && f->nerrors == 2 && f->diags.n == 2 && f->loaded);
    if (f->diags.n == 2) {
        STR_EQ(f->diags.v[0].msg, "a.quad:2:14: error: unknown name nope");
        STR_EQ(f->diags.v[1].msg, "b.quad:1:14: error: unknown name also_nope");
    }
    qf_free(f);
    const char *unsorted[] = { "#A: { x: \"p\" | \"q\", y: _ }\ni: #A & { y: nope }\n", "#B: #A & { x: \"r\" }\n" };
    f = load(unsorted, 2, &ok);
    CHECK(!ok && f->diags.n == 2);
    if (f->diags.n == 2) {
        STR_EQ(f->diags.v[0].msg, "a.quad:2:14: error: unknown name nope");
        STR_EQ(f->diags.v[1].msg, "b.quad:1:12: error: #B inheriting from #A: x: \"r\" is not one of \"p\" | \"q\", with #A at a.quad:1");
    }
    const char *more = "x: 1\n", *name = "z.quad";
    CHECK(!qf_load(f, &name, &more, 1));                         /* a front end loads once */
    qf_free(f);
    /* a syntax error in one file stops the load */
    const char *bad[] = { "#A: { x: _ }\n", "a: #A & { x: ? }\n" };
    f = load(bad, 2, &ok);
    CHECK(!ok && f->diags.n == 1 && f->types.n == 0);
    if (f->diags.n) STR_EQ(f->diags.v[0].msg, "b.quad:1:14: error: unexpected character '?'");
    qf_free(f);
}

/* --- unification, subsumption, generalization and copies, through handles -------------------------- */

void test_structure_operations(void)
{
    bool ok;
    Front *f = load1(with_kinds(
        "#Tool: { size: %length, mass: %mass, grip: \"soft\" | \"hard\", label: text }\n"
        "#Wrench: #Tool & { jaw: %length, head: #Tool & { size: .size } }\n"
        "#Hammer: #Tool & { face: %length }\n"
        "small: #Tool & { size: <=20mm }\n"
        "w19: #Wrench & { size: 19mm, mass: 350g }\n"
        "w24: #Wrench & { size: 24mm, mass: 500g, grip: \"hard\" }\n"
        "soft: #Tool & { grip: \"soft\", label: \"comfort\" }\n"
        "club: #Hammer & { face: 40mm }\n"), &ok);
    CHECK(ok);
    if (!ok) { qf_free(f); return; }
    QfOp *op = qf_op_new(f);
    QfStore *out = qf_store_new();
    QfFailure fail;
    const QfNode *small = inst(f, "small"), *w19 = inst(f, "w19"), *w24 = inst(f, "w24"), *soft = inst(f, "soft"),
                 *club = inst(f, "club");

    /* the result has the more specific type and the narrower values */
    const QfNode *r = qf_unify(op, small, w19, out, &fail);
    CHECK(r != NULL);
    const char *both = "#Wrench & { grip: (\"hard\" | \"soft\"), head: #Tool & { grip: (\"hard\" | \"soft\"), label: text, "
                       "mass: %mass, size: $1 & %length & 0.019 }, jaw: %length, label: text, mass: %mass & 0.35, size: $1 }";
    STR_EQ(node_text(f, r), both);
    /* the operands do not change */
    STR_EQ(node_text(f, small), "#Tool & { grip: (\"hard\" | \"soft\"), label: text, mass: %mass, size: %length & <=0.02 }");
    const QfNode *r2 = qf_unify(op, w19, small, out, &fail);
    STR_EQ(node_text(f, r2), both);
    /* an unchanged operand is shared with the result, not copied */
    const QfNode *r3 = qf_unify(op, w19, w19, out, &fail);
    CHECK(r3 == w19);
    const QfNode *r4 = qf_unify(op, r, soft, out, &fail);
    CHECK(r4 != NULL);
    STR_EQ(node_text(f, at(f, r4, "grip")), "\"soft\"");
    STR_EQ(node_text(f, at(f, r4, "label")), "\"comfort\"");
    CHECK(at(f, r4, "size") == at(f, r4, "head.size"));

    /* a failure says where, and what met */
    CHECK(qf_unify(op, small, w24, out, &fail) == NULL);
    STR_EQ(fail.path, "size");
    STR_EQ(fail.left, "%length & 0.024");
    STR_EQ(fail.right, "%length & <=0.02");
    STR_EQ(fail.why, "");
    CHECK(qf_unify(op, w19, club, out, &fail) == NULL);
    STR_EQ(fail.path, "(root)");
    STR_EQ(fail.left, "#Wrench");
    STR_EQ(fail.right, "#Hammer");
    CHECK(qf_unify(op, w24, soft, out, &fail) == NULL);
    STR_EQ(fail.path, "grip");
    /* a failure leaves nothing behind: the next operation is not affected */
    CHECK(qf_unify(op, small, w19, out, NULL) != NULL);

    /* subsumption: the general one is forward */
    bool fwd, bwd;
    qf_subsumes(op, small, r, &fwd, &bwd);
    CHECK(fwd && !bwd);
    qf_subsumes(op, r, small, &fwd, &bwd);
    CHECK(!fwd && bwd);
    qf_subsumes(op, w19, w19, &fwd, &bwd);
    CHECK(fwd && bwd);
    qf_subsumes(op, w19, w24, &fwd, &bwd);
    CHECK(!fwd && !bwd);
    qf_subsumes(op, small, w24, &fwd, &bwd);
    CHECK(!fwd && !bwd);                                         /* 24 mm is not <= 20 mm */
    qf_subsumes(op, soft, r4, &fwd, &bwd);
    CHECK(fwd && !bwd);
    /* sharing counts: a structure with two equal lengths does not subsume... it does, but not the reverse */
    const QfNode *tool = qf_type_node(f, type_lookup(f, qf_sym(f, "#Tool"))),
                 *wrench = qf_type_node(f, type_lookup(f, qf_sym(f, "#Wrench")));
    qf_subsumes(op, tool, wrench, &fwd, &bwd);
    CHECK(fwd && !bwd);

    /* generalization: the most specific structure that subsumes both */
    const QfNode *g = qf_generalize(op, w19, w24, out);
    /* both operands share size with head.size, so the result shares them too */
    STR_EQ(node_text(f, g), "#Wrench & { grip: (\"hard\" | \"soft\"), head: #Tool & { grip: (\"hard\" | \"soft\"), label: text, "
                            "mass: %mass, size: $1 & %length & >=0.019 & <=0.024 }, jaw: %length, label: text, "
                            "mass: %mass & >=0.35 & <=0.5, size: $1 }");
    qf_subsumes(op, g, w19, &fwd, &bwd);
    CHECK(fwd);
    qf_subsumes(op, g, w24, &fwd, &bwd);
    CHECK(fwd);
    const QfNode *g2 = qf_generalize(op, w19, club, out);
    STR_EQ(node_text(f, g2), "#Tool & { grip: (\"hard\" | \"soft\"), label: text, mass: %mass, size: %length }");

    /* a copy shares nothing with its source, and it is equal to it */
    QfStore *other = qf_store_new(), *tmp = qf_store_new();
    const QfNode *src = qf_unify(op, r, soft, tmp, &fail);
    const QfNode *cp = qf_copy(op, src, other);
    CHECK(cp != src && cp->store == other && at(f, cp, "size") != at(f, src, "size"));
    CHECK(at(f, cp, "size") == at(f, cp, "head.size"));
    qf_subsumes(op, cp, src, &fwd, &bwd);
    CHECK(fwd && bwd);
    char *before = strdup(node_text(f, src));
    qf_store_free(tmp);                                          /* the copy does not need the store of its source */
    STR_EQ(node_text(f, cp), before);
    free(before);

    /* a structure from handles: no text is parsed */
    uint32_t ttool = type_lookup(f, qf_sym(f, "#Tool")), tlen = type_lookup(f, qf_sym(f, "%length")),
             tmass = type_lookup(f, qf_sym(f, "%mass"));
    uint32_t fsize = feat_lookup(f, qf_sym(f, "size")), fmass = feat_lookup(f, qf_sym(f, "mass")),
             flabel = feat_lookup(f, qf_sym(f, "label")), fhead = feat_lookup(f, qf_sym(f, "head"));
    QInterval q19 = { 190, 190 }, heavy = { 1000, Q_INF };
    QfNode *size = qf_build_quantity(op, out, tlen, q19);
    uint32_t feats[3] = { fsize, fmass, flabel };
    QfNode *vals[3] = { size, qf_build_quantity(op, out, tmass, heavy), qf_build_text(op, out, "from \"cells\"") };
    QfNode *built = qf_build_struct(op, out, ttool, feats, vals, 3);
    CHECK(size && vals[1] && vals[2] && built);
    STR_EQ(node_text(f, built), "#Tool & { label: \"from \\\"cells\\\"\", mass: %mass & >=1, size: %length & 0.019 }");
    /* unified with its type, it has the constraint of the type */
    const QfNode *full = qf_unify(op, built, tool, out, &fail);
    STR_EQ(node_text(f, full), "#Tool & { grip: (\"hard\" | \"soft\"), label: \"from \\\"cells\\\"\", mass: %mass & >=1, size: %length & 0.019 }");
    CHECK(qf_unify(op, built, w19, out, &fail) == NULL);          /* 350 g is not >= 1 kg */
    STR_EQ(fail.path, "mass");
    /* a shared node: the same value at two features */
    uint32_t f2[2] = { fsize, fhead };
    uint32_t fh[1] = { fsize };
    QfNode *hv[1] = { size };
    QfNode *head = qf_build_struct(op, out, type_lookup(f, qf_sym(f, "#Wrench")), fh, hv, 1);
    QfNode *v2[2] = { size, head };
    QfNode *shared = qf_build_struct(op, out, type_lookup(f, qf_sym(f, "#Wrench")), f2, v2, 2);
    STR_EQ(node_text(f, shared), "#Wrench & { head: #Wrench & { size: $1 & %length & 0.019 }, size: $1 }");
    CHECK(qf_unify(op, shared, w19, out, &fail) != NULL);
    CHECK(qf_unify(op, shared, w24, out, &fail) == NULL);
    uint32_t dup[2] = { fsize, fsize };
    CHECK(qf_build_struct(op, out, ttool, dup, v2, 2) == NULL);
    CHECK(qf_build_struct(op, out, tlen, feats, vals, 3) == NULL);
    CHECK(qf_build_quantity(op, out, ttool, q19) == NULL);
    QInterval emptyq = { 5, 4 };
    CHECK(qf_build_quantity(op, out, tlen, emptyq) == NULL);

    /* the quick check: types at a few paths. It never rejects a unification that succeeds. */
    uint32_t pgrip[1] = { feat_lookup(f, qf_sym(f, "grip")) }, psize[2] = { fhead, fsize };
    const uint32_t *paths[3] = { pgrip, psize, NULL };
    size_t lens[3] = { 1, 2, 0 };
    QfQuickCheck *qc = qf_qc_new(f, paths, lens, 3);
    CHECK(qf_qc_size(qc) == 3);                                   /* the root and two paths */
    uint32_t fg[1] = { pgrip[0] };
    QfNode *gv[1] = { qf_build_text(op, out, "soft") };
    QfNode *textgrip = qf_build_struct(op, out, ttool, fg, gv, 1);
    CHECK(textgrip && qf_unify(op, textgrip, soft, out, NULL) != NULL);     /* the atom's name as text */
    CHECK(qf_unify(op, textgrip, w24, out, NULL) == NULL);                  /* "soft" is not "hard" */
    CHECK(op->scratch.n == 0);                                              /* the scratch nodes are given back */
    const QfNode *all[] = { small, w19, w24, soft, club, r, r4, g, built, shared, textgrip };
    uint32_t va[3], vb[3];
    int rejected = 0;
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        for (size_t j = 0; j < sizeof all / sizeof *all; j++) {
            qf_qc_vector(qc, all[i], va);
            qf_qc_vector(qc, all[j], vb);
            bool compat = qf_qc_compatible(qc, va, vb), unifies = qf_unify(op, all[i], all[j], out, NULL) != NULL;
            CHECK(compat || !unifies);
            rejected += !compat;
        }
    CHECK(rejected > 0);
    qf_qc_vector(qc, w24, va);
    qf_qc_vector(qc, soft, vb);
    CHECK(!qf_qc_compatible(qc, va, vb));                         /* "hard" and "soft" */
    qf_qc_vector(qc, w19, va);
    qf_qc_vector(qc, club, vb);
    CHECK(!qf_qc_compatible(qc, va, vb));                         /* the roots */
    qf_qc_free(qc);

    qf_store_free(other);
    qf_store_free(out);
    qf_op_free(op);
    qf_free(f);
}

/* --- two contexts work on one content at the same time (ADR 0015 section 7) ------------------------ */

typedef struct { Front *f; const QfNode **nodes; size_t n; uint64_t sum; int rounds; } Worker;

static uint64_t text_hash(const char *s) { return qf_fnv1a(0xcbf29ce484222325ULL, s, strlen(s)); }

/* Unifies and generalizes every pair, and sums the hashes of the results. */
static void *work(void *arg)
{
    Worker *w = arg;
    QfOp *op = qf_op_new(w->f);
    uint64_t sum = 0;
    for (int round = 0; round < w->rounds; round++) {
        QfStore *out = qf_store_new();
        for (size_t i = 0; i < w->n; i++)
            for (size_t j = 0; j < w->n; j++) {
                Buf b = { 0 };
                qf_buf_puts(&b, "");
                const QfNode *r = qf_unify(op, w->nodes[i], w->nodes[j], out, NULL);
                if (r) qf_node_print(w->f, &b, r);
                qf_buf_putc(&b, '|');
                qf_node_print(w->f, &b, qf_generalize(op, w->nodes[i], w->nodes[j], out));
                bool fwd, bwd;
                qf_subsumes(op, w->nodes[i], w->nodes[j], &fwd, &bwd);
                qf_buf_printf(&b, "|%d%d", fwd, bwd);
                sum += text_hash(b.s) * (uint64_t) (i * w->n + j + 1);
                qf_buf_free(&b);
            }
        qf_store_free(out);
    }
    qf_op_free(op);
    w->sum = sum;
    return NULL;
}

void test_two_contexts(void)
{
    Front *f = load_fixture();
    if (!f) return;
    const QfNode *nodes[32];
    size_t n = 0;
    for (size_t i = 0; i < f->insts.n && n < 16; i++) nodes[n++] = f->insts.v[i].node;
    for (uint32_t t = 0; t < f->types.n && n < 32; t++)
        if (f->types.v[t].kind == TK_DEF && f->types.v[t].expansion) nodes[n++] = f->types.v[t].expansion;
    CHECK(n > 20);
    Worker alone = { f, nodes, n, 0, 3 };
    work(&alone);
    CHECK(alone.sum != 0);
    /* two contexts in turn */
    QfOp *a = qf_op_new(f), *b = qf_op_new(f);
    QfStore *sa = qf_store_new(), *sb = qf_store_new();
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) {
            const QfNode *ra = qf_unify(a, nodes[i], nodes[j], sa, NULL), *rb = qf_unify(b, nodes[j], nodes[i], sb, NULL);
            CHECK((ra == NULL) == (rb == NULL));
            if (ra && rb) {
                char *ta = strdup(node_text(f, ra));
                bool fwd, bwd;
                qf_subsumes(a, ra, rb, &fwd, &bwd);
                CHECK(fwd && bwd);                               /* a & b and b & a are equal */
                /* a result of one context is an operand in the other */
                const QfNode *again = qf_unify(b, ra, nodes[i], sb, NULL);
                CHECK(again != NULL);
                if (again) STR_EQ(node_text(f, again), ta);
                free(ta);
            }
        }
    qf_store_free(sa);
    qf_store_free(sb);
    qf_op_free(a);
    qf_op_free(b);
    /* four threads, each with its own context, on the one content */
    enum { NT = 4 };
    pthread_t th[NT];
    Worker ws[NT];
    for (int i = 0; i < NT; i++) {
        ws[i] = (Worker) { f, nodes, n, 0, 3 };
        CHECK(pthread_create(&th[i], NULL, work, &ws[i]) == 0);
    }
    for (int i = 0; i < NT; i++) {
        pthread_join(th[i], NULL);
        CHECK(ws[i].sum == alone.sum);
    }
    qf_free(f);
}

/* --- properties ------------------------------------------------------------------------------------- */

/* Unification is commutative and idempotent, its result is below both
 * operands, and a generalization is above both. */
void test_prop_unification(void)
{
    Front *f = load_fixture();
    if (!f) return;
    const QfNode *nodes[64];
    size_t n = 0;
    for (size_t i = 0; i < f->insts.n && n < 32; i++) nodes[n++] = f->insts.v[i].node;
    for (uint32_t t = 0; t < f->types.n && n < 64; t++)
        if ((f->types.v[t].kind == TK_DEF || f->types.v[t].kind == TK_GLB) && f->types.v[t].expansion)
            nodes[n++] = f->types.v[t].expansion;
    QfOp *op = qf_op_new(f);
    QfStore *out = qf_store_new();
    uint64_t rng = 15;
    int unified = 0;
    for (int k = 0; k < 600; k++) {
        const QfNode *a = nodes[t_range(&rng, 0, (int64_t) n - 1)], *b = nodes[t_range(&rng, 0, (int64_t) n - 1)];
        const QfNode *ab = qf_unify(op, a, b, out, NULL), *ba = qf_unify(op, b, a, out, NULL);
        CHECK((ab == NULL) == (ba == NULL));
        bool fwd, bwd;
        if (ab && ba) {
            unified++;
            qf_subsumes(op, ab, ba, &fwd, &bwd);
            CHECK(fwd && bwd);
            qf_subsumes(op, a, ab, &fwd, &bwd);
            CHECK(fwd);
            qf_subsumes(op, b, ab, &fwd, &bwd);
            CHECK(fwd);
            const QfNode *again = qf_unify(op, ab, a, out, NULL);
            CHECK(again != NULL);
            if (again) {
                qf_subsumes(op, ab, again, &fwd, &bwd);
                CHECK(fwd && bwd);
            }
        }
        const QfNode *g = qf_generalize(op, a, b, out);
        qf_subsumes(op, g, a, &fwd, &bwd);
        CHECK(fwd);
        qf_subsumes(op, g, b, &fwd, &bwd);
        CHECK(fwd);
        const QfNode *aa = qf_unify(op, a, a, out, NULL);
        CHECK(aa == a);
    }
    CHECK(unified > 30);
    qf_store_free(out);
    qf_op_free(op);
    qf_free(f);
}

/* The declarations of a source in another order, as new source text. */
static char *shuffled_source(const char *name, const char *src, uint64_t *rng)
{
    Front *f = qf_new();
    DeclVec d = { 0 };
    bool ok = qf_parse(f, name, src, &d);
    CHECK(ok);
    Buf b = { 0 };
    qf_buf_puts(&b, "");
    if (ok && d.n) {
        if (d.v[0].pkg) qf_buf_printf(&b, "package %s\n", d.v[0].pkg);
        for (size_t i = d.n - 1; i > 0; i--) {
            size_t j = (size_t) t_range(rng, 0, (int64_t) i);
            Decl tmp = d.v[i]; d.v[i] = d.v[j]; d.v[j] = tmp;
        }
        for (size_t i = 0; i < d.n; i++) {
            Decl one = d.v[i];
            one.pkg = NULL;                      /* the package line is written once, first */
            qf_print_decls(&b, &one, 1);
        }
    }
    VEC_FREE(d);
    qf_free(f);
    return b.s;
}

/* The result of a load does not depend on the order of the files or on the
 * order of the declarations in a file (ADR 0015 section 9). */
void test_prop_load_order(void)
{
    char *src[NFIX];
    if (!fixture_sources(src)) return;
    Front *base = qf_new();
    CHECK(qf_load(base, fixture_files, (const char *const *) src, NFIX));
    Buf want = { 0 };
    qf_dump(base, &want);
    uint64_t hash = base->hash;
    CHECK(hash == qf_fnv1a(0xcbf29ce484222325ULL, want.s, want.len));
    size_t ntypes = base->types.n, nfeats = base->feats.n;
    qf_free(base);

    uint64_t rng = 2026;
    for (int round = 0; round < 40; round++) {
        const char *names[NFIX];
        char *text[NFIX];
        int order[NFIX];
        for (int i = 0; i < NFIX; i++) order[i] = i;
        for (int i = NFIX - 1; i > 0; i--) {
            int j = (int) t_range(&rng, 0, i), tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (int i = 0; i < NFIX; i++) {
            names[i] = fixture_files[order[i]];
            text[i] = shuffled_source(names[i], src[order[i]], &rng);
        }
        Front *f = qf_new();
        bool ok = qf_load(f, names, (const char *const *) text, NFIX);
        CHECK(ok);
        if (ok) {
            Buf got = { 0 };
            qf_dump(f, &got);
            CHECK(f->hash == hash && f->types.n == ntypes && f->feats.n == nfeats);
            CHECK(!strcmp(got.s, want.s));
            if (strcmp(got.s, want.s) && t_failures < 3) {
                const char *g = got.s, *w = want.s;
                while (*g && *g == *w) { g++; w++; }
                fprintf(stderr, "  round %d: the dumps differ at\n  got    %.200s\n  wanted %.200s\n", round, g, w);
            }
            qf_buf_free(&got);
        } else {
            for (size_t i = 0; i < f->diags.n && i < 3; i++) fprintf(stderr, "  %s\n", f->diags.v[i].msg);
        }
        qf_free(f);
        for (int i = 0; i < NFIX; i++) free(text[i]);
    }
    qf_buf_free(&want);

    /* a content with an error fails in every order, with the same messages apart from the places */
    const char *bad[3] = { "#A: { x: \"p\" | \"q\", y: _ }\n", "a: #A & { x: \"r\" }\nb: #A & { y: nope }\n", "c: #A & { x: \"p\" }\n" };
    for (int round = 0; round < 6; round++) {
        static const int perms[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
        const char *names[3] = { "x.quad", "y.quad", "z.quad" }, *text[3];
        for (int i = 0; i < 3; i++) text[i] = bad[perms[round][i]];
        Front *f = qf_new();
        CHECK(!qf_load(f, names, text, 3));
        CHECK(f->nerrors == 2 && f->diags.n == 2);
        int conflict = 0, unknown = 0;
        for (size_t i = 0; i < f->diags.n; i++) {
            conflict += strstr(f->diags.v[i].msg, "x: \"r\" is not one of \"p\" | \"q\"") != NULL;
            unknown += strstr(f->diags.v[i].msg, "unknown name nope") != NULL;
        }
        CHECK(conflict == 1 && unknown == 1);
        qf_free(f);
    }
    for (int i = 0; i < NFIX; i++) free(src[i]);
}

/* A reference record says where a structure holds a copy of an instance, or
 * of a part of one (ADR 0016 section 3). */
static const char *refs_text(Front *f, const char *name)
{
    static Buf out;
    out.len = 0;
    qf_buf_puts(&out, "");
    uint32_t i = qf_instance(f, name);
    if (i == QF_BOTTOM) return "no such instance";
    InstRefVec refs = { 0 };
    qf_refs(f, f->insts.v[i].node, &f->insts.v[i].refs, &refs);
    VEC(char *) lines = { 0 };
    for (size_t k = 0; k < refs.n; k++) {
        Buf l = { 0 };
        path_format(f, &l, refs.v[k].path.v, refs.v[k].path.n);
        qf_buf_printf(&l, " = %s", refs.v[k].inst);
        if (refs.v[k].sub.n) {
            qf_buf_putc(&l, '.');
            path_format(f, &l, refs.v[k].sub.v, refs.v[k].sub.n);
        }
        VEC_PUSH(lines, l.s);
    }
    for (size_t a = 0; a < lines.n; a++)             /* a small list: in the order of the text */
        for (size_t b = a + 1; b < lines.n; b++)
            if (strcmp(lines.v[b], lines.v[a]) < 0) { char *t = lines.v[a]; lines.v[a] = lines.v[b]; lines.v[b] = t; }
    for (size_t k = 0; k < lines.n; k++) {
        qf_buf_printf(&out, "%s%s", k ? "; " : "", lines.v[k]);
        free(lines.v[k]);
    }
    VEC_FREE(lines);
    qf_refs_free(&refs);
    return out.s;
}

void test_reference_records(void)
{
    bool ok;
    Front *f = load1(with_kinds("#Leg: { len: %length }\n"
                                "#Table: { leg: #Leg, spare: #Leg, legs: [...#Leg] }\n"
                                "#Room: { table: #Table, other: #Table }\n"
                                "oak: #Leg & { len: 1m }\n"
                                "t: #Table & { leg: oak, legs: [oak, #Leg & { len: 2m }] }\n"
                                "u: #Table & { spare: t.leg, leg: t.legs[1] }\n"
                                "r: #Room & { table: t, other: u }\n"
                                "#OakTable: #Table & { spare: oak }\n"
                                "#Den: #Room & { table: #OakTable }\n"
                                "v: #OakTable & { leg: { len: 3m } }\n"
                                "d: #Den\n"
                                "#Nook: #Room & { other: #OakTable }\n"
                                "n: #Nook\n"
                                "plain: #Table & { leg: { len: 1m } }\n"),
                     &ok);
    CHECK(ok);
    for (size_t i = 0; i < f->diags.n; i++) fprintf(stderr, "  %s\n", f->diags.v[i].msg);
    if (ok) {
        STR_EQ(refs_text(f, "oak"), "");
        STR_EQ(refs_text(f, "plain"), "");
        /* a whole instance, and an item of a list */
        STR_EQ(refs_text(f, "t"), "leg = oak; legs.first = oak");
        /* a path into an instance. A copy of a copy names the instance that the author wrote. */
        STR_EQ(refs_text(f, "u"), "leg = t.legs.rest.first; spare = t.leg");
        /* a copy holds the references below it, at their new paths */
        STR_EQ(refs_text(f, "r"), "other = u; other.leg = t.legs.rest.first; other.spare = t.leg; table = t; "
                                  "table.leg = oak; table.legs.first = oak");
        /* a type that names an instance gives the reference to its instances, at the path of each node of the type */
        STR_EQ(refs_text(f, "v"), "spare = oak");
        STR_EQ(refs_text(f, "d"), "table.spare = oak");
        STR_EQ(refs_text(f, "n"), "other.spare = oak");            /* at the first field of the structure */
        uint32_t ot = type_lookup(f, qf_sym(f, "#OakTable")), den = type_lookup(f, qf_sym(f, "#Den"));
        CHECK(f->types.v[ot].refs.n == 1 && f->types.v[den].refs.n == 0);
        /* the text form of the load has the records, so the content hash covers them */
        Buf dump = { 0 };
        qf_dump(f, &dump);
        CHECK(strstr(dump.s, "\n  reference at leg: t.legs.rest.first\n  reference at spare: t.leg\n") != NULL);
        CHECK(strstr(dump.s, "\n  reference at spare: oak\n") != NULL);
        qf_buf_free(&dump);
    }
    qf_free(f);
}

/* The number of generated types in a content: a meet type, or a conjunction type. */
static size_t generated_types(const Front *f)
{
    size_t n = 0;
    for (uint32_t t = 0; t < f->types.n; t++) n += f->types.v[t].kind == TK_GLB;
    return n;
}

/* --- comprehensions and built-in functions (quad-ijx.9) ---------------------------------------- */

/* A comprehension, a computed label and a function call are expanded when
 * content loads. The data of the workshop example (tests/data/workshop/data.tri)
 * gives the values that Triangle gives. */
void test_comprehensions(void)
{
    /* the workshop data. Its variables m and s have other names here: m and s are unit symbols */
    static const char shop[] =
        "let ratios = [1, 2, 4]\n"
        "#Gearbox: { teeth: [...number] }\n"
        "gearbox: #Gearbox & { teeth: [for r in ratios if r > 1 { r * 12 }] }\n"
        "let sizes = [10mm, 13mm, 17mm, 19mm, 24mm]\n"
        "#Material: \"steel\" | \"brass\" | \"oak\" | \"ash\"\n"
        "#Stock: { for mat in #Material { (mat): number & >=0 & <=50 } }\n"
        "#Rack: { slots: [...%length], stock: #Stock }\n"
        "rack: #Rack & {\n"
        "    _spare: 2\n"
        "    slots:  [for size in sizes if size > 12mm { size }]\n"
        "    stock:  { steel: len(.slots) + _spare, brass: 0, oak: 0, ash: list.Sum([1, 2]) }\n"
        "}\n";
    STR_EQ(inst_text(shop, "gearbox"), "#Gearbox & { teeth: [number, ...number] & { first: 24, rest: [number, ...number] & "
                                       "{ first: 48, rest: #Null } } }");
    HAS_TEXT(inst_text(shop, "rack"), "stock: #Stock & { ash: 3, brass: 0, oak: 0, steel: 6 }");
    HAS_TEXT(inst_text(shop, "rack"), "slots: [%length, ...%length] & { first: %length & 0.013, rest: [%length, ...%length] & "
                                      "{ first: %length & 0.017, rest: [%length, ...%length] & { first: %length & 0.019, rest: "
                                      "[%length, ...%length] & { first: %length & 0.024, rest: #Null } } } }");
    /* the fields of #Stock come from the enum: another field is an error */
    char src[2048];
    snprintf(src, sizeof src, "%sbad: #Stock & { elm: 1 }\n", shop);
    STR_EQ(load_error(src), "a.quad:13:17: error: unknown field 'elm'. The fields of #Stock are: ash, brass, oak, steel");

#define ONE(body, want) STR_EQ(inst_text("#T: { a: _, b: _ }\nt: #T & { " body " }\n", "t"), "#T & { " want " }")
#define LIST2(x, y) "#Cons & { first: " x ", rest: #Cons & { first: " y ", rest: #Null } }"
    /* the clauses of a comprehension: a key, two sources, a field of this declaration */
    ONE("a: [for i, x in [10, 20] { i + x }]", "a: " LIST2("10", "21") ", b: _");
    ONE("a: [for x in [1, 2] for y in [10] { x + y }]", "a: " LIST2("11", "12") ", b: _");
    ONE("a: [1, 2, 3], b: [for x in .a if x >= 2 { x * 10 }]",
        "a: #Cons & { first: 1, rest: " LIST2("2", "3") " }, b: " LIST2("20", "30"));
    ONE("a: [for k, v in { p: 1, q: 2 } { v }]", "a: " LIST2("1", "2") ", b: _");
    ONE("a: [for k, v in { p: 1, q: 2 } { k }]", "a: " LIST2("\"p\"", "\"q\"") ", b: _");
    ONE("a: [for x in [] { x }], b: 1", "a: #Null, b: 1");
    /* a part of the value of a variable: an item of a list, a field of a struct */
    ONE("a: [for p in [[1, 2], [3, 4]] { p[0] * 10 + p[1] }]", "a: " LIST2("12", "34") ", b: _");
    ONE("a: [for p in [{ x: 1, y: 2 }, { x: 3, y: 4 }] { p.y - p.x }]", "a: " LIST2("1", "1") ", b: _");
    /* the atoms of an enum, also if the first one is its default */
    STR_EQ(inst_text("#E: *\"p\" | \"q\"\n#T: { a: _ }\nt: #T & { a: [for x in #E { x }] }\n", "t"),
           "#T & { a: " LIST2("\"p\"", "\"q\"") " }");
    /* the fields of a struct from a comprehension, with a computed label */
    STR_EQ(inst_text("let keys = [\"p\", \"q\"]\n#T: { for k in keys { (k): number } }\nt: #T & { p: 1, q: 2 }\n", "t"),
           "#T & { p: 1, q: 2 }");
    /* a declaration reads the data of another declaration, with its comprehensions expanded */
    STR_EQ(inst_text("#T: { a: _ }\nu: #T & { a: [for x in [1, 2] { x * 2 }] }\nt: #T & { a: [for x in u.a { x + 1 }] }\n", "t"),
           "#T & { a: " LIST2("3", "5") " }");

    /* an expression of numbers has its value when content loads, if the value is exact */
    ONE("a: 2 * 12mm, b: 7 / 2", "a: dim[m] & 0.024, b: 3.5");
    ONE("a: 10J / 4s, b: 3 - 5", "a: dim[m^2 kg s^-3] & 2.5, b: -2");
    ONE("a: 1 / 3, b: 1", "a: _, b: 1");                           /* not exact: it stays an expression */
    ONE("a: 1m + 1s, b: 1", "a: _, b: 1");                         /* no value: the units do not fit */
    /* a comparison of known values is true or false. With a reference it is a law */
    ONE("a: 2 > 1 && !(3 == 4), b: \"x\" != \"y\"", "a: true, b: true");
    ONE("a: 1mm >= 1m, b: 1 < 2 || 5 < 4", "a: false, b: true");
    ONE("a: [1 <= 1, 2 <= 1], b: [1 >= 1, 1 > 1]", "a: " LIST2("true", "false") ", b: " LIST2("true", "false"));
    HAS_TEXT(inst_text("#T: { a: number, b: number, ok: .a <= .b }\nt: #T & { a: 1, b: 2 }\n", "t"), "ok: .a <= .b");
    /* a bound with a named value */
    STR_EQ(inst_text("let top = 5\n#T: { n: number }\nt: #T & { n: <=top & >=5 }\n", "t"), "#T & { n: 5 }");

    /* the functions */
    ONE("a: len([1, 2, 3]) * 10, b: len(\"abc\") + len({ p: 1, q: 2 })", "a: 30, b: 5");
    ONE("a: [div(-7, 2), mod(-7, 2)], b: [quo(-7, 2), rem(-7, 2)]", "a: " LIST2("-4", "1") ", b: " LIST2("-3", "-1"));
    ONE("a: [math.Abs(-2.5mm), math.Floor(-2.5)], b: [math.Ceil(-2.5), math.Round(-2.5)]",
        "a: " LIST2("dim[m] & 0.0025", "-3") ", b: " LIST2("-2", "-3"));
    ONE("a: [math.Trunc(-2.5), math.Round(2.4)], b: [math.Floor(7), math.Round(2.5)]",
        "a: " LIST2("-2", "2") ", b: " LIST2("7", "3"));
    ONE("a: [math.Min(3mm, 2cm), math.Max(3mm, 2cm)], b: [math.Clamp(7, 1, 5), math.Clamp(-7, 1, 5)]",
        "a: " LIST2("dim[m] & 0.003", "dim[m] & 0.02") ", b: " LIST2("5", "1"));
    ONE("a: strings.Join([for x in [\"oak\", \"Ash \"] { strings.ToUpper(strings.TrimSpace(x)) }], \"-\"), "
        "b: strings.ToLower(\"OAK\")", "a: \"OAK-ASH\", b: \"oak\"");
    ONE("a: [strings.Contains(\"wrench\", \"en\"), strings.HasPrefix(\"wrench\", \"wr\")], "
        "b: [strings.HasSuffix(\"wrench\", \"ch\"), strings.HasSuffix(\"wrench\", \"x\")]",
        "a: " LIST2("true", "true") ", b: " LIST2("true", "false"));
    ONE("a: [strings.Index(\"wrench\", \"en\"), strings.Index(\"wrench\", \"zz\")], b: strings.Split(\"a,b\", \",\")",
        "a: " LIST2("2", "-1") ", b: " LIST2("\"a\"", "\"b\""));
    ONE("a: [strings.Replace(\"aaa\", \"a\", \"b\", 2), strings.Replace(\"aaa\", \"a\", \"b\", -1)], "
        "b: [strings.Repeat(\"ab\", 2), strings.Repeat(\"ab\", 0)]",
        "a: " LIST2("\"bba\"", "\"bbb\"") ", b: " LIST2("\"abab\"", "\"\""));
    ONE("a: [list.Sum([1mm, 2cm]), list.Product([2, 3, 4])], b: [list.Max([1, 5, 3]), list.Min([4, 2, 8])]",
        "a: " LIST2("dim[m] & 0.021", "24") ", b: " LIST2("5", "2"));
    ONE("a: [list.Contains([1, 2], 2), list.Contains([\"p\"], \"q\")], b: list.Concat([[1], [2]])",
        "a: " LIST2("true", "false") ", b: " LIST2("1", "2"));
    ONE("a: list.Range(5, 0, -3), b: list.Slice([1, 2, 3, 4], 1, 3)", "a: " LIST2("5", "2") ", b: " LIST2("2", "3"));
    ONE("a: list.Take([1, 2, 3], 2), b: list.Drop([1, 2, 3], 1)", "a: " LIST2("1", "2") ", b: " LIST2("2", "3"));
    ONE("a: list.Repeat([7], 2), b: list.SortStrings([\"q\", \"p\"])", "a: " LIST2("7", "7") ", b: " LIST2("\"p\"", "\"q\""));
    /* the limits: an end that a range leaves out, a start at 0, a count of 0, a text of the same length */
    ONE("a: list.Range(0, 6, 3), b: list.Range(5, -1, -3)", "a: " LIST2("0", "3") ", b: " LIST2("5", "2"));
    ONE("a: list.Slice([1, 2, 3], 0, 2), b: list.Repeat([7], 0)", "a: " LIST2("1", "2") ", b: #Null");
    ONE("a: strings.HasPrefix(\"ab\", \"ab\"), b: strings.HasSuffix(\"ab\", \"ab\")", "a: true, b: true");
#undef ONE
#undef LIST2

    /* errors */
#define ERR(body, want) STR_EQ(load_error("#T: { a: _, b: _ }\nt: #T & { " body " }\n"), "a.quad:2:" want)
    ERR("a: nope(1)", "14: error: unknown function nope: the built-in functions are len, div, mod, quo, rem, and the functions "
                      "of math, strings and list");
    ERR("a: len(1, 2)", "14: error: len takes 1 argument: len(list | struct | string)");
    ERR("a: len(5)", "14: error: len: takes a closed list, a struct or a string that is known when content loads");
    ERR("a: div(1, 0)", "14: error: div: division by zero");
    ERR("a: div(1.5, 2)", "14: error: div: takes two whole numbers that are known when content loads");
    ERR("a: math.Min(1mm, 1s)", "14: error: math.Min: takes two numbers with one unit");
    ERR("a: math.Clamp(1, 5, 2)", "14: error: math.Clamp: the lowest value is above the highest");
    ERR("a: list.Sum([1mm, 1s])", "14: error: list.Sum: the numbers have different units");
    ERR("a: list.Max([])", "14: error: list.Max: the list is empty");
    ERR("a: list.Range(0, 5, 0)", "14: error: list.Range: the step is zero");
    ERR("a: list.Slice([1, 2], 1, 5)", "14: error: list.Slice: the range is not inside the list");
    ERR("a: strings.Join([1], \"-\")", "14: error: strings.Join: takes a closed list of strings and a string");
    ERR("a: list.SortStrings([1, \"a\"])", "14: error: list.SortStrings: takes a list of strings");
    ERR("a: [for p in [[1]] { p[1] }]", "32: error: p[1]: its value has no such field or item");
    ERR("a: strings.Replace(\"a\", \"\", \"b\", 1)", "14: error: strings.Replace: the text to replace is empty");
    ERR("a: [for x in 5 { x }]", "15: error: a comprehension goes through a list, a struct or an enum that the content writes: "
                                 "not 5");
    ERR("a: [for x in [...] { x }]", "15: error: a comprehension cannot go through an open list: not all of its items are "
                                     "written");
    ERR("a: [for x in [1mm, 2] if x > 1 { x }]", "38: error: a comparison of numbers with different units");
    ERR("a: [for x in [1] if x { x }]", "25: error: a condition is true or false, known when content loads: not 1");
    ERR("a: [for x in .zzz { x }]", "24: error: .zzz is not written in this declaration: a comprehension, a function or an "
                                    "interpolation reads only values that the content writes");
    ERR("a: len(.b)", "18: error: .b is not written in this declaration: a comprehension, a function or an interpolation "
                      "reads only values that the content writes");
#undef ERR
    STR_EQ(load_error("#T: { for k in [\"1x\"] { (k): number } }\n"),
           "a.quad:1:25: error: a computed label must be the name of a field: not \"1x\"");
    STR_EQ(load_error("#T: { for k in [5] { (k): number } }\n"), "a.quad:1:22: error: a computed label is a string: not 5");
    STR_EQ(load_error("#T: { a: _ }\na: #T & { a: [for x in b.a { x }] }\nb: #T & { a: [for x in a.a { x }] }\n"),
           "a.quad:3:24: error: the comprehensions of a read a declaration whose comprehensions read a back");
    STR_EQ(load_error("#T: { a: _, b: _ }\nu: #T & { a: [.b], b: 1 }\nt: #T & { a: [for x in u.a { x }] }\n"),
           "a.quad:3:24: error: u.a holds .b, which belongs to u: a comprehension reads values as written, so the values of "
           "another declaration must be plain values");

    /* the order of the files does not count */
    static const char *const two[] = { "#T: { a: _ }\nu: #T & { a: [for x in [1, 2] { x * 2 }] }\n",
                                       "t: #T & { a: [for x in u.a { x + 1 }] }\n" };
    const char *const owt[] = { two[1], two[0] };
    bool ok, ok2;
    Front *f = load(two, 2, &ok), *g = load(owt, 2, &ok2);
    CHECK(ok && ok2 && f->hash == g->hash);
    qf_free(f);
    qf_free(g);
}

/* --- text constraints and interpolation (quad-ijx.11) ------------------------------------------- */

/* A text value is one string, a set of constraints with regular expressions,
 * or any text. An interpolation becomes its string when content loads. The
 * text of the workshop example (tests/data/workshop/text.tri) loads. */
void test_text(void)
{
    static const char shop[] =
        "#Labelled: { label: text & =~#\"^[A-Z][a-z]+-\\d+$\"#, note: text }\n"
        "#Tin: #Labelled & { count: number & >=1 & <=500 }\n"
        "tin_of_nails: #Tin & { count: 120, label: \"Nails-\\(.count)\", note: \"for \\\"soft\\\" wood\\tonly\" }\n"
        "open: #Tin & { count: 2 }\n";
    STR_EQ(inst_text(shop, "tin_of_nails"), "#Tin & { count: 120, label: \"Nails-120\", note: \"for \\\"soft\\\" wood\\tonly\" }");
    /* a text with a constraint and no string yet shows its constraint */
    STR_EQ(inst_text(shop, "open"), "#Tin & { count: 2, label: =~\"^[A-Z][a-z]+-\\\\d+$\", note: text }");
    char src[2048];
    snprintf(src, sizeof src, "%sbad: #Tin & { count: 1, label: \"nails-1\" }\n", shop);
    STR_EQ(load_error(src), "a.quad:5:13: error: label: \"nails-1\" does not meet the constraint =~\"^[A-Z][a-z]+-\\\\d+$\"");

    /* =~ and !~ together, and the constraints of a type and of its supertype */
    static const char both[] =
        "#A: { a: =~\"^a\" }\n#B: #A & { a: !~\"z$\" }\n"
        "x: #B\np: #B & { a: \"abc\" }\n";
    STR_EQ(inst_text(both, "x"), "#B & { a: =~\"^a\" & !~\"z$\" }");
    STR_EQ(inst_text(both, "p"), "#B & { a: \"abc\" }");
    snprintf(src, sizeof src, "%sq: #B & { a: \"abz\" }\n", both);
    STR_EQ(load_error(src), "a.quad:5:7: error: a: \"abz\" does not meet the constraint =~\"^a\" & !~\"z$\"");
    snprintf(src, sizeof src, "%sq: #B & { a: \"xbc\" }\n", both);
    STR_EQ(load_error(src), "a.quad:5:9: error: a: \"xbc\" does not meet the constraint =~\"^a\"");
    /* a default among text and a pattern: text covers the pattern (Triangle: test_text_defaults) */
    static const char rx[] = "#Rx: { r: *\"ab\" | =~\"^z\" | text }\nr1: #Rx\nr2: #Rx & { r: \"zz\" }\n";
    STR_EQ(inst_text(rx, "r1"), "#Rx & { r: \"ab\" }");
    STR_EQ(inst_text(rx, "r2"), "#Rx & { r: \"zz\" }");
    /* a pattern that is not a regular expression is an error, in each place */
    STR_EQ(load_error("#T: { a: =~\"(\" }\n"), "a.quad:1:10: error: the pattern \"(\" is not a regular expression: missing )");
    STR_EQ(load_error("#T: { a: *\"x\" | =~\"[\" | text }\n"),
           "a.quad:1:17: error: the pattern \"[\" is not a regular expression: missing ] in a class");
    /* a constraint is for text: a number does not meet it */
    STR_EQ(load_error("#T: { a: =~\"^a\" }\nt: #T & { a: 5 }\n"), "a.quad:2:9: error: a: 5 is not text");

    /* interpolation: numbers as the content writes them, strings, true and false, and a value of an expression */
    STR_EQ(inst_text("#T: { a: text }\nt: #T & { a: \"x \\(1 + 2) \\(true) \\(\"s\") \\(2.50mm)\" }\n", "t"),
           "#T & { a: \"x 3 true s 2.50mm\" }");
    STR_EQ(inst_text("#T: { a: text, n: number }\nt: #T & { n: 7, a: \"n is \\(.n), and \\(len([1, 2])) items\" }\n", "t"),
           "#T & { a: \"n is 7, and 2 items\", n: 7 }");
    /* in a comprehension, and as a computed label */
    STR_EQ(inst_text("#T: { a: _ }\nt: #T & { a: [for x in [1, 2] { \"item \\(x)\" }] }\n", "t"),
           "#T & { a: #Cons & { first: \"item 1\", rest: #Cons & { first: \"item 2\", rest: #Null } } }");
    STR_EQ(inst_text("let keys = [\"p\", \"q\"]\n#T: { for k in keys { \"\\(k)_x\": number } }\nt: #T & { p_x: 1, q_x: 2 }\n", "t"),
           "#T & { p_x: 1, q_x: 2 }");
    /* a part that the content does not write in the declaration is an error */
    STR_EQ(load_error("#T: { a: text, n: number }\nt: #T & { a: \"n is \\(.n)\" }\n"),
           "a.quad:2:22: error: .n is not written in this declaration: a comprehension, a function or an interpolation "
           "reads only values that the content writes");
    STR_EQ(load_error("#T: { a: text }\nt: #T & { a: \"list \\([1])\" }\n"),
           "a.quad:2:22: error: an interpolation takes a number, a string, or true or false that is known when content "
           "loads: not [1]");

    /* the unifier: a set of constraints with a string, with another set, and with any text */
    bool ok;
    Front *f = load1("#T: { a: text }\n"
                     "any: #T\nca: #T & { a: =~\"^a\" }\ncb: #T & { a: =~\"b$\" }\ncab: #T & { a: =~\"^a\" & =~\"b$\" }\n"
                     "sab: #T & { a: \"ab\" }\nsac: #T & { a: \"ac\" }\nsxx: #T & { a: \"xx\" }\n", &ok);
    CHECK(ok);
    if (ok) {
        QfOp *op = qf_op_new(f);
        QfStore *out = qf_store_new();
        QfFailure fail;
#define N(name) inst(f, name)
        /* two sets: the union. It is the same set as the one that source writes */
        const QfNode *u = qf_unify(op, N("ca"), N("cb"), out, &fail);
        CHECK(u != NULL);
        STR_EQ(node_text(f, u), "#T & { a: =~\"^a\" & =~\"b$\" }");
        bool fwd, bwd;
        qf_subsumes(op, u, N("cab"), &fwd, &bwd);
        CHECK(fwd && bwd);
        /* a set and a string: the string, if it meets each constraint */
        STR_EQ(node_text(f, qf_unify(op, N("cab"), N("sab"), out, &fail)), "#T & { a: \"ab\" }");
        CHECK(qf_unify(op, N("cab"), N("sac"), out, &fail) == NULL);
        STR_EQ(fail.text, "\"ac\" does not meet the constraint =~\"^a\" & =~\"b$\"");
        STR_EQ(node_text(f, qf_unify(op, N("any"), N("ca"), out, &fail)), "#T & { a: =~\"^a\" }");
        /* subsumption: any text, then a set, then a larger set, then a string */
        qf_subsumes(op, N("any"), N("ca"), &fwd, &bwd);
        CHECK(fwd && !bwd);
        qf_subsumes(op, N("ca"), N("cab"), &fwd, &bwd);
        CHECK(fwd && !bwd);
        qf_subsumes(op, N("ca"), N("cb"), &fwd, &bwd);
        CHECK(!fwd && !bwd);
        qf_subsumes(op, N("ca"), N("sab"), &fwd, &bwd);
        CHECK(fwd && !bwd);
        qf_subsumes(op, N("cb"), N("sac"), &fwd, &bwd);
        CHECK(!fwd && !bwd);
        /* generalization: the constraints that both keep */
        STR_EQ(node_text(f, qf_generalize(op, N("sab"), N("sac"), out)), "#T & { a: text }");
        STR_EQ(node_text(f, qf_generalize(op, N("cab"), N("ca"), out)), "#T & { a: =~\"^a\" }");
        STR_EQ(node_text(f, qf_generalize(op, N("cab"), N("sac"), out)), "#T & { a: =~\"^a\" }");
        STR_EQ(node_text(f, qf_generalize(op, N("ca"), N("sxx"), out)), "#T & { a: text }");
        STR_EQ(node_text(f, qf_generalize(op, N("ca"), N("cb"), out)), "#T & { a: text }");
        STR_EQ(node_text(f, qf_generalize(op, N("sab"), N("sab"), out)), "#T & { a: \"ab\" }");
#undef N
        qf_store_free(out);
        qf_op_free(op);
    }
    qf_free(f);

    /* the regular expressions: the syntax of RE2, with no backtracking */
    static const struct { const char *pat, *text; bool match; } cases[] = {
        { "^[A-Z][a-z]+-\\d+$", "Nails-120", true }, { "^[A-Z][a-z]+-\\d+$", "nails-120", false },
        { "a|b", "xbx", true },                      { "^(ab)*$", "ababab", true },
        { "^(ab)*$", "aba", false },                 { "^a.c$", "abc", true },
        { "^a{2,3}$", "aaaa", false },               { "^a{2,3}$", "aaa", true },
        { "^\\w+\\s\\w+$", "two words", true },      { "^[^0-9]+$", "abc1", false },
        { "(?i)^nails$", "NAILS", true },            { "^(a+)+$", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", false },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char err[200];
        Regex *re = regex_compile(cases[i].pat, strlen(cases[i].pat), err, sizeof err);
        CHECK(re != NULL);
        if (re) CHECK(regex_match(re, cases[i].text, strlen(cases[i].text)) == cases[i].match);
        regex_free(re);
    }
    char err[200];
    CHECK(regex_compile("a(", 2, err, sizeof err) == NULL && err[0]);
    CHECK(regex_compile("\\1", 2, err, sizeof err) == NULL);        /* no backreference */
}

/* --- defaults (quad-ijx.10) ------------------------------------------------------------------- */

/* Every diagnostic of a content, one on each line. */
static const char *all_diags(const char *const *sources, size_t n)
{
    static Buf out;
    out.len = 0;
    qf_buf_puts(&out, "");
    bool ok;
    Front *f = load(sources, n, &ok);
    for (size_t i = 0; i < f->diags.n; i++) qf_buf_printf(&out, "%s\n", f->diags.v[i].msg);
    qf_free(f);
    return out.s;
}

/* The structure of an instance before its defaults. */
static const char *pattern_text(const char *src, const char *name)
{
    static Buf out;
    out.len = 0;
    qf_buf_puts(&out, "");
    bool ok;
    Front *f = load1(src, &ok);
    uint32_t i = ok ? qf_instance(f, name) : QF_BOTTOM;
    if (i != QF_BOTTOM) qf_node_print(f, &out, f->insts.v[i].pattern);
    qf_free(f);
    return out.s;
}

/* A default applies after unification, with the rules of Triangle: the more
 * specific contributor wins, a default never changes a value, and defaults
 * that conflict do not apply. The cases of Triangle's tests give the same
 * results here. */
void test_defaults(void)
{
    /* a default with one other alternative: a number, an enum, a range (Triangle: test_defaults_with_type_alternatives) */
    static const char H[] =
        "#M: \"steel\" | \"oak\"\n"
        "#H: { h: *5 | number, m: *\"oak\" | #M, n: *3 | >=0 & <=9 }\n"
        "x: #H\ny: #H & { h: 7 }\n";
    STR_EQ(inst_text(H, "x"), "#H & { h: 5, m: \"oak\", n: 3 }");
    STR_EQ(inst_text(H, "y"), "#H & { h: 7, m: \"oak\", n: 3 }");
    /* the structure before the defaults stays: a name in source copies it */
    STR_EQ(pattern_text(H, "x"), "#H & { h: number, m: #M, n: >=0 & <=9 }");

    /* #R overrides #P. #R and #Q have no order, so neither applies. #P stays
     * overridden (Triangle: test_default_override_survives_tie) */
    static const char *const tie[] = {
        "#F: { f: \"a\" | \"b\" | \"c\" }\n"
        "#P: #F & { f: *\"a\" }\n#Q: #F & { f: *\"b\" }\n#R: #P & { f: *\"c\" }\n"
        "x: #R & #Q\ny: #R\nz: #P & #Q\n" };
    STR_EQ(inst_text(tie[0], "x"), "#Q & #R & { f: (\"a\" | \"b\" | \"c\") }");
    STR_EQ(inst_text(tie[0], "y"), "#R & { f: \"c\" }");
    STR_EQ(inst_text(tie[0], "z"), "glb[#P & #Q,#Q & #R] & { f: (\"a\" | \"b\" | \"c\") }");
    STR_EQ(all_diags(tie, 1),
           "a.quad:5:1: warning: x: the defaults for f from #Q, #R conflict, so none of them applies. Give f a value\n"
           "a.quad:7:1: warning: z: the defaults for f from #P, #Q conflict, so none of them applies. Give f a value\n");

    /* a supertype that states the value of one side of a conflict does not decide the conflict */
    STR_EQ(inst_text("#F: { f: \"a\" | \"b\" | \"c\" }\n"
                     "#P: #F & { f: *\"c\" }\n#Q: #F & { f: *\"b\" }\n#R: #P & { f: *\"c\" }\nx: #R & #Q\n", "x"),
           "#Q & #R & { f: (\"a\" | \"b\" | \"c\") }");
    /* of two defaults with no order, one loses to a value: the other applies, with no warning */
    static const char *const one[] = {
        "#F: { f: number }\n#Q: #F & { f: *1 }\n#R: #F & { f: *2 }\nx: #R & #Q & { f: <=1 }\ny: #R & #Q & { f: 2 }\n" };
    STR_EQ(inst_text(one[0], "x"), "#Q & #R & { f: 1 }");
    STR_EQ(inst_text(one[0], "y"), "#Q & #R & { f: 2 }");
    STR_EQ(all_diags(one, 1), "");
    /* a default that comes through two parents is one default */
    static const char *const diamond[] = {
        "#A: { x: *1 | number }\n#B: #A & { b: _ }\n#C: #A & { c: _ }\n#D: #B & #C\nd: #D\n" };
    STR_EQ(inst_text(diamond[0], "d"), "#D & { b: _, c: _, x: 1 }");
    STR_EQ(all_diags(diamond, 1), "");

    /* a default among text, through a subtype too (Triangle: test_text_defaults) */
    static const char text[] =
        "#Log: { splitsInto: *\"firewood_20\" | text }\n"
        "#Trunk: #Log & { lengths: number }\n"
        "log1: #Log & {}\nlog2: #Log & { splitsInto: \"kindling\" }\ntrunk1: #Trunk & { lengths: 3 }\n";
    STR_EQ(inst_text(text, "log1"), "#Log & { splitsInto: \"firewood_20\" }");
    STR_EQ(inst_text(text, "log2"), "#Log & { splitsInto: \"kindling\" }");
    STR_EQ(inst_text(text, "trunk1"), "#Trunk & { lengths: 3, splitsInto: \"firewood_20\" }");

    /* the default of an instance wins over the default of a type, and loses to a value of the instance */
    STR_EQ(inst_text("#H: { h: *5 | number }\nx: #H & { h: *7 | number }\n", "x"), "#H & { h: 7 }");
    STR_EQ(inst_text("#H: { h: *5 | number }\nx: #H & { h: *7 | number }\nx: { h: 9 }\n", "x"), "#H & { h: 9 }");
    /* a default with no alternative: the field stays open for a value */
    STR_EQ(inst_text("#T: { x: *5 }\na: #T\n", "a"), "#T & { x: 5 }");
    STR_EQ(inst_text("#T: { x: *5 }\na: #T & { x: \"other\" }\n", "a"), "#T & { x: \"other\" }");
    /* a quantity: the value converts to the kind of the field */
    static const char W[] = "#W: { tolerance: *1pct | %ratio }\na: #W\nb: #W & { tolerance: 5pct }\n";
    STR_EQ(inst_text(W, "a"), "#W & { tolerance: %ratio & 0.01 }");
    STR_EQ(inst_text(W, "b"), "#W & { tolerance: %ratio & 0.05 }");

    /* an instance that names another instance takes its structure before the
     * defaults, and its defaults: below its own, above those of a type */
    static const char *const named[] = {
        "#Bolt: { state: *\"loose\" | \"tight\" }\n"
        "#Machine: { bolt: #Bolt, material: *\"steel\" }\n"
        "m1: #Machine & { material: *\"ash\" }\n"
        "m2: m1 & { bolt: { state: \"tight\" } }\n"
        "m3: m1 & { material: *\"oak\" }\n"
        "m4: #Machine & { material: *\"elm\" }\n"
        "m5: m1 & m4\n"
        "m6: m5 & { material: \"pine\" }\n" };
    STR_EQ(inst_text(named[0], "m1"), "#Machine & { bolt: #Bolt & { state: \"loose\" }, material: \"ash\" }");
    STR_EQ(inst_text(named[0], "m2"), "#Machine & { bolt: #Bolt & { state: \"tight\" }, material: \"ash\" }");
    STR_EQ(inst_text(named[0], "m3"), "#Machine & { bolt: #Bolt & { state: \"loose\" }, material: \"oak\" }");
    STR_EQ(inst_text(named[0], "m5"), "#Machine & { bolt: #Bolt & { state: \"loose\" }, material: _ }");
    STR_EQ(inst_text(named[0], "m6"), "#Machine & { bolt: #Bolt & { state: \"loose\" }, material: \"pine\" }");
    STR_EQ(all_diags(named, 1), "a.quad:7:1: warning: m5: the defaults for material from m1, m4 conflict, so none of them "
                                "applies. Give material a value\n");

    /* the defaults of the workshop example (tests/data/workshop/blueprints.tri, section 5), as Triangle resolves them */
    static const char *const shop[] = {
        "#Material: \"steel\" | \"brass\" | \"oak\" | \"ash\"\n"
        "#Physical: { material: #Material, mass: %mass }\n"
        "#Sized: { size: %length }\n"
        "#Tool: #Physical & #Sized & { wear: *0 | >=0 & <=1000 }\n"
        "#Nut: #Physical & #Sized & { state: \"loose\" | \"tight\" }\n",
        "#Metal: #Physical & { material: *\"steel\" }\n"
        "#Wooden: #Physical & { material: *\"oak\" }\n"
        "#Handled: #Wooden & { mass: <=2kg }\n"
        "hammer: #Tool & #Metal & #Handled & { size: 30cm, mass: 1.2kg }\n"
        "mallet: #Tool & #Handled & #Metal & { material: *\"ash\", size: 25cm, mass: 0.9kg }\n"
        "nut: #Nut & { material: \"brass\", size: 12mm, mass: 9g, state: *\"loose\" | \"tight\" }\n" };
    const char *const pohs[] = { shop[1], shop[0] };
    bool ok, ok2;
    Front *f = load(shop, 2, &ok), *g = load(pohs, 2, &ok2);
    CHECK(ok && ok2);
    if (ok && ok2) {
        STR_EQ(node_text(f, inst(f, "mallet")),
               "#Handled & #Metal & #Tool & { mass: %mass & 0.9, material: \"ash\", size: %length & 0.25, wear: 0 }");
        STR_EQ(node_text(f, inst(f, "hammer")),
               "#Handled & #Metal & #Tool & { mass: %mass & 1.2, material: #Material, size: %length & 0.3, wear: 0 }");
        STR_EQ(node_text(f, inst(f, "nut")),
               "#Nut & { mass: %mass & 0.009, material: \"brass\", size: %length & 0.012, state: \"loose\" }");
        /* the order of the files does not count: the same structures, the same warning, the same hash */
        STR_EQ(node_text(g, inst(g, "mallet")),
               "#Handled & #Metal & #Tool & { mass: %mass & 0.9, material: \"ash\", size: %length & 0.25, wear: 0 }");
        STR_EQ(node_text(g, inst(g, "hammer")),
               "#Handled & #Metal & #Tool & { mass: %mass & 1.2, material: #Material, size: %length & 0.3, wear: 0 }");
        CHECK(f->hash == g->hash);
        CHECK(f->diags.n == 1 && g->diags.n == 1);
        if (f->diags.n == 1 && g->diags.n == 1) {
            STR_EQ(f->diags.v[0].msg, "b.quad:4:1: warning: hammer: the defaults for material from #Metal, #Wooden conflict, "
                                      "so none of them applies. Give material a value");
            STR_EQ(g->diags.v[0].msg + 1, f->diags.v[0].msg + 1);          /* the same, in the other file */
        }
    }
    qf_free(f);
    qf_free(g);
    /* a default is a part of the content: the hash changes with it */
    Front *h5 = load1("#H: { h: *5 | number }\n", &ok), *h6 = load1("#H: { h: *6 | number }\n", &ok2);
    CHECK(ok && ok2 && h5->hash != h6->hash);
    qf_free(h5);
    qf_free(h6);

    /* a default alternative of structures in a type: the default gives the
     * alternative, and then the alternative gives its own default */
    static const char G[] =
        "#G: { grip: { kind: \"bare\" } | *{ kind: \"wrapped\", wraps: *3 | number } }\n"
        "g: #G\nh: #G & { grip: { kind: \"bare\" } }\n";
    STR_EQ(inst_text(G, "g"), "#G & { grip: ({ kind: \"bare\" } | { kind: \"wrapped\", wraps: *3 | number })/{ kind: "
                              "\"wrapped\", wraps: *3 | number } & { kind: \"wrapped\", wraps: 3 } }");
    /* a value that the default alternative does not allow: the default loses */
    STR_EQ(inst_text(G, "h"), "#G & { grip: ({ kind: \"bare\" } | { kind: \"wrapped\", wraps: *3 | number }) & { kind: "
                              "\"bare\" } }");

    /* errors */
    STR_EQ(load_error("#T: { x: *1 | *2 | number }\n"), "a.quad:1:15: error: more than one default in a disjunction");
    STR_EQ(load_error("#T: { a: _ }\nx: #T & { a: *\"p\" | *\"q\" }\n"),
           "a.quad:2:21: error: more than one default in a disjunction");
    STR_EQ(load_error("#A: { a: _ }\n#B: { b: _ }\n#T: { s: *#A | *#B }\n"),
           "a.quad:3:16: error: more than one default in a disjunction");
    /* numbers that are apart are still no type */
    STR_EQ(load_error("#T: { x: *1 | 2 | 3 }\n"),
           "a.quad:1:17: error: a disjunction of numbers is not a type; use a range, or an enum of atoms");
    /* a default that can never apply: in a type, against the field; in an instance, against its other alternative */
    STR_EQ(load_error("#T: { x: *1 | 2 }\n"),
           "a.quad:1:11: error: #T: the default for x is not a value of its other alternative: 2 and 1 are different "
           "values. In *d | rest, the default d must be one of the values of rest");
    STR_EQ(load_error("#T: { x: *0 | >=1 & <=10 }\n"),
           "a.quad:1:11: error: #T: the default for x is not a value of its other alternative: the range 1 .. 10 and 0 "
           "have no value in common. In *d | rest, the default d must be one of the values of rest");
    /* the default is a value of its other alternative, and the field of the supertype refuses it */
    STR_EQ(load_error("#F: { f: >=5 }\n#P: #F & { f: *0 | number }\n"),
           "a.quad:2:16: error: #P: the default for f can never apply: f: >= 5 and 0 have no value in common");
    STR_EQ(load_error("#F: { f: \"a\" | \"b\" | \"c\" }\n#P: #F & { f: *\"z\" }\n"),
           "a.quad:2:16: error: #P: the default for f can never apply: f: \"z\" is not one of \"a\" | \"b\" | \"c\"");
    STR_EQ(load_error("#H: { h: number }\nx: #H & { h: *\"q\" | number }\n"),
           "a.quad:2:15: error: x: the default for h is not a value of its other alternative: \"q\" is not a number. "
           "In *d | rest, the default d must be one of the values of rest");
    /* an error in the value of a default comes once, and not for each instance */
    static const char *const nope[] = { "#T: { x: *nope | number }\na: #T\nb: #T\n" };
    STR_EQ(all_diags(nope, 1), "a.quad:1:11: error: unknown name nope\n");
    static const char *const nope2[] = { "#T: { x: number }\na: #T & { x: *nope | number }\n" };
    STR_EQ(all_diags(nope2, 1), "a.quad:2:15: error: unknown name nope\n");
}

/* A message of the unifier uses the words of the author: the field, the
 * value with its unit, and the kind. It has no word of the implementation
 * (quad-fjp). */
void test_messages(void)
{
    static const char K[] =
        "%voltage: { dimension: \"V\" }\n"
        "%temperature: { scale: -6 }\n"
        "%width: { dimension: \"m\" }\n"
        "%energy: { dimension: \"J\" }\n"
        "#T: { v: %voltage, t: %temperature, on: bool, c: %count, p: %energy / %time, n: number, w: %width, "
        "l: %length, any: _ }\n";
    char src[1024];
#define MSG(inst, want)                                                    \
    do {                                                                   \
        snprintf(src, sizeof src, "%sx: %s\n", K, inst);                   \
        STR_EQ(load_error(src), "a.quad:6:" want);                         \
    } while (0)

    /* a value with the unit of another dimension */
    MSG("#T & { v: 12A }", "9: error: v: 12 A is not of the kind %voltage. The unit of %voltage is V");
    MSG("#T & { p: 5m }", "9: error: p: 5 m is not a quantity in W");
    MSG("#T & { any: 12mm & 5kg }", "21: error: any: 0.012 m and 5 kg have different units");
    MSG("#T & { any: (%energy / %time) & (%length / %time) }",
        "34: error: any: a quantity in W and a quantity in m s^-1 are different: the unit of the first is W, and the "
        "unit of the second is m s^-1");

    /* C is the coulomb: the message gives the value in kelvin, if the value is one number that fits */
    MSG("#T & { t: 25C }", "9: error: t: 25 C is not of the kind %temperature. The unit of %temperature is K. The unit C "
                           "is the coulomb. Write a temperature in kelvin: 25 degrees Celsius is 298.15K");
    MSG("#T & { t: 25.5C }", "9: error: t: 25.5 C is not of the kind %temperature. The unit of %temperature is K. The "
                             "unit C is the coulomb. Write a temperature in kelvin: 25.5 degrees Celsius is 298.65K");
    MSG("#T & { t: -40C }", "9: error: t: -40 C is not of the kind %temperature. The unit of %temperature is K. The "
                            "unit C is the coulomb. Write a temperature in kelvin: -40 degrees Celsius is 233.15K");
    MSG("#T & { t: 0.001C }", "9: error: t: 0.001 C is not of the kind %temperature. The unit of %temperature is K. The "
                              "unit C is the coulomb. Write a temperature in kelvin: 0.001 degrees Celsius is 273.151K");
    MSG("#T & { t: >=25C }", "9: error: t: >= 25 C is not of the kind %temperature. The unit of %temperature is K. The "
                             "unit C is the coulomb. Write a temperature in kelvin");
    MSG("#T & { t: 1e30C }", "9: error: t: 1e30 C is not of the kind %temperature. The unit of %temperature is K. The "
                             "unit C is the coulomb. Write a temperature in kelvin");
    /* the hint is for a temperature only */
    MSG("#T & { v: 25C }", "9: error: v: 25 C is not of the kind %voltage. The unit of %voltage is V");

    /* a number with no unit, and a unit where the field has none */
    MSG("#T & { p: 5 }", "9: error: p: 5 has no unit, and the field is a quantity in W");
    MSG("#T & { v: 5 }", "9: error: v: 5 has no unit, and %voltage has the unit V");
    MSG("#T & { c: 5m }", "9: error: c: 5 m has a unit, and %count is a number with no unit");

    /* two kinds with one dimension */
    MSG("#T & { w: %length }", "9: error: w: %width and %length are different kinds");
    MSG("#T & { l: 5m } & { w: .l }", "26: error: the reference .l: l: %length and %width are different kinds");

    /* a value of another type */
    MSG("#T & { on: 3 }", "9: error: on: 3 is not of the type bool");
    MSG("#T & { on: \"yes\" }", "9: error: on: \"yes\" is not of the type bool");
    MSG("#T & { t: \"warm\" }", "9: error: t: \"warm\" is not of the kind %temperature");

    /* an unknown field: the message lists the fields of the type, 12 at most */
    MSG("#T & { volts: 12V }", "11: error: unknown field 'volts'. The fields of #T are: any, c, l, n, on, p, t, v, w");
    STR_EQ(load_error("#Big: { a: _, b: _, c: _, d: _, e: _, f: _, g: _, h: _, i: _, j: _, k: _, l: _, m: _, n: _ }\n"
                      "x: #Big & { z: 1 }\n"),
           "a.quad:2:13: error: unknown field 'z'. The fields of #Big are: a, b, c, d, e, f, g, h, i, j, k, l, and 2 more");
    STR_EQ(load_error("#Twelve: { a: _, b: _, c: _, d: _, e: _, f: _, g: _, h: _, i: _, j: _, k: _, l: _ }\n"
                      "x: #Twelve & { z: 1 }\n"),
           "a.quad:2:16: error: unknown field 'z'. The fields of #Twelve are: a, b, c, d, e, f, g, h, i, j, k, l");
    STR_EQ(load_error("x: { z: 1 }\n"), "a.quad:1:6: error: unknown field 'z': no type declares a field with this name");
    /* two types together have no name of their own */
    STR_EQ(load_error("#A: { a: _ }\n#B: { b: _ }\nx: #A & #B & { z: 1 }\n"),
           "a.quad:3:16: error: unknown field 'z'. The fields of this type are: a, b");
#undef MSG
}

/* Unrelated types can each have a field with the same name (ADR 0017). The
 * name is one feature, and each of the types introduces it. */
void test_shared_fields(void)
{
    bool ok;
    const char *src = with_kinds("#A: { x: %length, own: text }\n"
                                 "#B: { x: text }\n"
                                 "#H: { item: _ }\n"
                                 "a: #A & { x: 2m }\n"
                                 "b: #B & { x: \"two\" }\n");
    Front *f = load1(src, &ok);
    CHECK(ok);
    if (ok) {
        STR_EQ(node_text(f, inst(f, "a")), "#A & { own: text, x: %length & 2 }");
        STR_EQ(node_text(f, inst(f, "b")), "#B & { x: \"two\" }");
        uint32_t a = type_lookup(f, qf_sym(f, "#A")), b = type_lookup(f, qf_sym(f, "#B")), h = type_lookup(f, qf_sym(f, "#H")),
                 fx = feat_lookup(f, qf_sym(f, "x")), fown = feat_lookup(f, qf_sym(f, "own"));
        CHECK(fx != QF_BOTTOM && fown != QF_BOTTOM);
        if (fx != QF_BOTTOM && fown != QF_BOTTOM) {
            /* one feature for the name, and both types introduce it */
            CHECK(f->feats.v[fx].intros.n == 2 && introduces(f, a, fx) && introduces(f, b, fx) && !introduces(f, h, fx));
            CHECK(f->feats.v[fown].intros.n == 1 && introduces(f, a, fown));
            CHECK(approp(f, a, fx) && approp(f, b, fx) && !approp(f, h, fx) && !approp(f, b, fown));
            /* the hierarchy has no new type: the two types stay unrelated, below the top */
            CHECK(meet_pure(f, a, b) == QF_BOTTOM && generated_types(f) == 0);
            CHECK(f->types.v[a].parents.n == 1 && f->types.v[a].parents.v[0] == f->top);
        }
        Buf dump = { 0 };
        qf_dump(f, &dump);
        CHECK(strstr(dump.s, "\nfeature x of #A, #B\n") != NULL && strstr(dump.s, "\nfeature own of #A\n") != NULL);
        qf_buf_free(&dump);
    }
    qf_free(f);

    /* each type keeps its own declaration of the field */
    STR_EQ(load_error(with_kinds("#A: { x: %length }\n#B: { x: text }\na: #A & { x: \"no\" }\n")),
           "a.quad:6:7: error: x: \"no\" is not of the kind %length");
    /* a structure still cannot have a field that its type does not declare */
    STR_EQ(load_error("#A: { x: _ }\n#B: { x: _, y: _ }\na: #A & { y: 1 }\n"),
           "a.quad:3:9: error: this struct has fields of #B, but it stands where the type is #A: write #B & { ... }");
    /* a struct that stands where no type is known: the message names the types */
    STR_EQ(load_error(with_kinds("#A: { x: %length }\n#B: { x: text }\n#H: { item: _ }\nh: #H & { item: { x: 2m } }\n")),
           "a.quad:7:17: error: this struct stands where no type is known, and a struct does not get a type from its "
           "fields. Its fields are fields of more than one type: state its type before it, with '&'. The types with "
           "these fields: #A, #B");
    STR_EQ(inst_text(with_kinds("#A: { x: %length }\n#B: { x: text }\n#H: { item: _ }\nh: #H & { item: #A & { x: 2m } }\n"), "h"),
           "#H & { item: #A & { x: %length & 2 } }");
    STR_EQ(inst_text(with_kinds("#A: { x: %length }\n#B: { x: text }\n#H: { item: #B }\nh: #H & { item: { x: \"ok\" } }\n"), "h"),
           "#H & { item: #B & { x: \"ok\" } }");

    /* a type below both: the two declarations unify, or the conflict is an error at that type */
    STR_EQ(inst_text(with_kinds("#A: { x: %length & >=1m }\n#B: { x: %length & <=5m }\n#C: #A & #B\nc: #C & { x: 2m }\n"), "c"),
           "#C & { x: %length & 2 }");
    STR_EQ(load_error(with_kinds("#A: { x: %length & >=1m }\n#B: { x: %length & <=5m }\n#C: #A & #B\nc: #C & { x: 6m }\n")),
           "a.quad:7:7: error: x: the range 1 m .. 5 m and 6 m have no value in common");
    STR_EQ(load_error(with_kinds("#A: { x: %length }\n#B: { x: text }\n#C: #A & #B\n")),
           "a.quad:6:1: error: #C inheriting from #B: x: a value cannot be both %length and text, with #B at a.quad:5");

    /* many shared names give no generated type */
    f = load1(with_kinds("#Port: { side: text }\n"
                         "#Wall: { hot: #Port, cold: #Port, mass: %mass }\n"
                         "#Glass: { hot: #Port, cold: #Port, tint: text }\n"
                         "#Door: { hot: #Port, mass: %mass }\n"
                         "#Solid: { mass: %mass }\n"
                         "#Slab: #Solid & { hot: #Port }\n"),
              &ok);
    CHECK(ok);
    if (ok) {
        uint32_t wall = type_lookup(f, qf_sym(f, "#Wall")), door = type_lookup(f, qf_sym(f, "#Door")),
                 glass = type_lookup(f, qf_sym(f, "#Glass")), solid = type_lookup(f, qf_sym(f, "#Solid")),
                 slab = type_lookup(f, qf_sym(f, "#Slab")), hot = feat_lookup(f, qf_sym(f, "hot")),
                 cold = feat_lookup(f, qf_sym(f, "cold")), mass = feat_lookup(f, qf_sym(f, "mass"));
        CHECK(generated_types(f) == 0);
        CHECK(f->feats.v[hot].intros.n == 4 && introduces(f, slab, hot) && !introduces(f, solid, hot));
        CHECK(f->feats.v[cold].intros.n == 2 && introduces(f, wall, cold) && introduces(f, glass, cold));
        /* the slab has its mass from #Solid: it does not introduce the name */
        CHECK(f->feats.v[mass].intros.n == 3 && introduces(f, solid, mass) && !introduces(f, slab, mass));
        CHECK(approp(f, slab, mass) && approp(f, door, mass) && !approp(f, glass, mass) && !approp(f, door, cold));
        for (size_t i = 1; i < f->feats.v[hot].intros.n; i++)      /* in the order of the types */
            CHECK(f->feats.v[hot].intros.v[i - 1] < f->feats.v[hot].intros.v[i]);
    }
    qf_free(f);

    /* a supertype that declares the field introduces it, alone */
    f = load1(with_kinds("#T: { x: _ }\n#A: #T & { x: text }\n#B: #T & { x: %length }\na: #A & { x: \"q\" }\n"), &ok);
    CHECK(ok);
    if (ok) {
        uint32_t fx = feat_lookup(f, qf_sym(f, "x"));
        CHECK(f->feats.v[fx].intros.n == 1 && introduces(f, type_lookup(f, qf_sym(f, "#T")), fx));
    }
    qf_free(f);

    /* a package has its own field names */
    const char *pk[2] = { "package p\n%size: { dimension: \"m\" }\n#A: { x: %size }\n#B: { x: text }\na: #A & { x: 1m }\n",
                          "#C: { x: text }\nc: #C & { x: \"root\" }\n" };
    f = load(pk, 2, &ok);
    CHECK(ok);
    if (ok) {
        uint32_t px = feat_lookup(f, qf_sym(f, "p.x")), rx = feat_lookup(f, qf_sym(f, "x"));
        CHECK(px != QF_BOTTOM && rx != QF_BOTTOM && px != rx);
        if (px != QF_BOTTOM && rx != QF_BOTTOM) {
            CHECK(f->feats.v[px].intros.n == 2 && introduces(f, type_lookup(f, qf_sym(f, "p.#A")), px) &&
                  introduces(f, type_lookup(f, qf_sym(f, "p.#B")), px));
            CHECK(f->feats.v[rx].intros.n == 1 && introduces(f, type_lookup(f, qf_sym(f, "#C")), rx));
        }
        STR_EQ(node_text(f, inst(f, "c")), "#C & { x: \"root\" }");
    }
    qf_free(f);

    /* An alternative that is a structure declares its own fields. It is not
     * below a named type that declares the same field. A struct that stands
     * where the type is a disjunction chooses an alternative by its fields. */
    const char *alts = "#A: { x: %length }\n"
                       "#S: { form: { x: text, r: %length } | { side: %length } }\n"
                       "s: #S & { form: { side: 1m } }\n"
                       "t: #S & { form: { x: \"a\", r: 2m } }\n"
                       "u: #S & { form: { x: \"a\" } }\n"
                       "a: #A & { x: 3m }\n";
    STR_EQ(inst_text(with_kinds(alts), "s"),
           "#S & { form: ({ side: %length } | { x: text, r: %length })/{ side: %length } & { side: %length & 1 } }");
    STR_EQ(inst_text(with_kinds(alts), "t"),
           "#S & { form: ({ side: %length } | { x: text, r: %length })/{ x: text, r: %length } & { r: %length & 2, x: \"a\" } }");
    STR_EQ(inst_text(with_kinds(alts), "u"),
           "#S & { form: ({ side: %length } | { x: text, r: %length })/{ x: text, r: %length } & { r: %length, x: \"a\" } }");
    STR_EQ(inst_text(with_kinds(alts), "a"), "#A & { x: %length & 3 }");
    STR_EQ(load_error(with_kinds("#S: { form: { x: text, r: %length } | { side: %length } }\ns: #S & { form: { side: 1m, r: 2m } }\n")),
           "a.quad:5:29: error: no type has all of these fields (at 'r')");

    /* a field that the alternatives of one disjunction share, and no named type has, is the disjunction's */
    const char *alts2 = "#S: { form: { x: text, r: %length } | { x: text, side: %length } }\n"
                        "u: #S & { form: { x: \"a\" } }\n"
                        "v: #S & { form: { x: \"a\", side: 1m } }\n";
    STR_EQ(inst_text(with_kinds(alts2), "u"),
           "#S & { form: ({ x: text, r: %length } | { x: text, side: %length }) & { x: \"a\" } }");
    STR_EQ(inst_text(with_kinds(alts2), "v"),
           "#S & { form: ({ x: text, r: %length } | { x: text, side: %length })/{ x: text, side: %length } & "
           "{ side: %length & 1, x: \"a\" } }");

    /* a struct with fields of two alternatives, and of no one alternative alone */
    STR_EQ(load_error(with_kinds("#N: { x: text }\n"
                                 "#S: { form: { x: text, r: %length } | { x: text, side: %length } }\n"
                                 "u: #S & { form: { x: \"a\" } }\n")),
           "a.quad:6:17: error: the fields of this struct are fields of more than one alternative of "
           "({ x: text, r: %length } | { x: text, side: %length }): state its type before it, with '&'");
    STR_EQ(inst_text(with_kinds("#N: { x: text }\n"
                                "#S: { form: { x: text, r: %length } | { x: text, side: %length } }\n"
                                "u: #S & { form: { x: \"a\", side: 2m } }\nn: #N & { x: \"b\" }\n"), "u"),
           "#S & { form: ({ x: text, r: %length } | { x: text, side: %length })/{ x: text, side: %length } & "
           "{ side: %length & 2, x: \"a\" } }");
    STR_EQ(load_error(with_kinds("#N: { x: text }\n#S: { form: { x: text, r: %length } | { side: %length } }\n"
                                 "#H: { item: _ }\nh: #H & { item: { x: \"a\" } }\n")),
           "a.quad:7:17: error: this struct stands where no type is known, and a struct does not get a type from its "
           "fields. Its fields are fields of more than one type: state its type before it, with '&'. The types with "
           "these fields: #N, ({ side: %length } | { x: text, r: %length })/{ x: text, r: %length }");

    /* Many types with many common names: the hierarchy gets no new type, so
     * the load stays fast. A generated supertype for each name made this
     * content take minutes (ADR 0017, considered options). */
    {
        Buf big = { 0 };
        for (int t = 0; t < 80; t++) {
            qf_buf_printf(&big, "#T%d: { own%d: text", t, t);
            for (int l = 0; l < 40; l++)
                if ((t * 7 + l * 13 + t * l) % 5 != 0) qf_buf_printf(&big, ", f%d: text", l);
            qf_buf_puts(&big, " }\n");
        }
        qf_buf_puts(&big, "t3: #T3 & { f1: \"a\", own3: \"b\" }\n");
        f = load1(big.s, &ok);
        CHECK(ok);
        if (ok) {
            CHECK(generated_types(f) == 0);
            uint32_t f1 = feat_lookup(f, qf_sym(f, "f1"));
            CHECK(f1 != QF_BOTTOM && f->feats.v[f1].intros.n > 50);
        }
        qf_free(f);
        qf_buf_free(&big);
    }

    /* the result does not depend on the order of the declarations */
    const char *mix = "%length: { dimension: \"m\" }\n"
                      "#Port: { side: text }\n"
                      "#Wall: { hot: #Port, cold: #Port, thick: %length, wide: %length, rule: .thick == .wide }\n"
                      "#Glass: { hot: #Port, cold: #Port, tint: text, wide: %length }\n"
                      "#Door: { hot: #Port, wide: %length, rule: .wide == .tall, tall: %length }\n"
                      "#Frame: #Door & #Glass\n"
                      "w: #Wall & { thick: 2m, wide: 2m }\n"
                      "d: #Door & { wide: 1m }\n"
                      "fr: #Frame & { tint: \"clear\" }\n";
    f = load1(mix, &ok);
    CHECK(ok);
    for (size_t i = 0; i < f->diags.n; i++) fprintf(stderr, "  %s\n", f->diags.v[i].msg);
    if (ok) {
        /* hot and wide: all three types. cold: the wall and the glass. rule: the wall and the door. */
        CHECK(f->feats.v[feat_lookup(f, qf_sym(f, "wide"))].intros.n == 3 && f->feats.v[feat_lookup(f, qf_sym(f, "rule"))].intros.n == 2);
        CHECK(generated_types(f) == 0);              /* #Frame names its two supertypes, so it needs no meet type */
        STR_EQ(node_text(f, inst(f, "fr")),
               "#Frame & { cold: #Port & { side: text }, hot: #Port & { side: text }, rule: .wide == .tall, tall: %length, "
               "tint: \"clear\", wide: %length }");
        Buf want = { 0 };
        qf_dump(f, &want);
        uint64_t rng = 13;
        for (int round = 0; round < 20; round++) {
            char *text = shuffled_source("a.quad", mix, &rng);
            bool ok2;
            Front *g = load1(text, &ok2);
            CHECK(ok2);
            if (ok2) {
                Buf got = { 0 };
                qf_dump(g, &got);
                CHECK(!strcmp(got.s, want.s) && g->hash == f->hash);
                qf_buf_free(&got);
            }
            qf_free(g);
            free(text);
        }
        qf_buf_free(&want);
    }
    qf_free(f);
}
