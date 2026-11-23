#include "front/front.h"

#include <ctype.h>
#include <stdio.h>

/* Kinds, dimensions, units, and exact decimal values (ADR 0015).
 *
 * A kind is a quantity type that source declares:
 *
 *   %energy: { dimension: "J", scale: -3, conserved: true }
 *   %coin:   { base: "coin", scale: -2 }
 *
 * Each SI base dimension has a built-in kind (%length, %mass, ...), and so
 * does the angle, which is a base dimension with the degree as its unit. A
 * content can declare a built-in base kind once, to change its scale or to
 * make it conserved.
 *
 * The resolver reads these declarations before it builds the type hierarchy.
 * A literal is an exact decimal with a dimension until unification gives it
 * a kind (fs.c). */

/* --- units ------------------------------------------------------------------------------- */

/* One unit is mult × 10^exp10 coherent units. A prefix can stand before a
 * symbol that allows one. */
typedef struct { const char *sym; int8_t exp[Q_NSI]; int64_t mult; int exp10; bool prefix; } UnitDef;

static const UnitDef unit_defs[] = {
    /*        m  kg   s  A  K mol cd */
    { "m",   { 1,  0,  0, 0, 0, 0, 0 }, 1, 0, true },
    { "g",   { 0,  1,  0, 0, 0, 0, 0 }, 1, -3, true },
    { "s",   { 0,  0,  1, 0, 0, 0, 0 }, 1, 0, true },
    { "A",   { 0,  0,  0, 1, 0, 0, 0 }, 1, 0, true },
    { "K",   { 0,  0,  0, 0, 1, 0, 0 }, 1, 0, true },
    { "mol", { 0,  0,  0, 0, 0, 1, 0 }, 1, 0, true },
    { "cd",  { 0,  0,  0, 0, 0, 0, 1 }, 1, 0, true },
    { "N",   { 1,  1, -2, 0, 0, 0, 0 }, 1, 0, true },
    { "J",   { 2,  1, -2, 0, 0, 0, 0 }, 1, 0, true },
    { "W",   { 2,  1, -3, 0, 0, 0, 0 }, 1, 0, true },
    { "Pa",  { -1, 1, -2, 0, 0, 0, 0 }, 1, 0, true },
    { "Hz",  { 0,  0, -1, 0, 0, 0, 0 }, 1, 0, true },
    { "C",   { 0,  0,  1, 1, 0, 0, 0 }, 1, 0, true },
    { "V",   { 2,  1, -3, -1, 0, 0, 0 }, 1, 0, true },
    { "ohm", { 2,  1, -3, -2, 0, 0, 0 }, 1, 0, true },
    { "F",   { -2, -1, 4, 2, 0, 0, 0 }, 1, 0, true },
    { "H",   { 2,  1, -2, -2, 0, 0, 0 }, 1, 0, true },
    { "S",   { -2, -1, 3, 2, 0, 0, 0 }, 1, 0, true },
    { "T",   { 0,  1, -2, -1, 0, 0, 0 }, 1, 0, true },
    { "Wb",  { 2,  1, -2, -1, 0, 0, 0 }, 1, 0, true },
    { "L",   { 3,  0,  0, 0, 0, 0, 0 }, 1, -3, true },
    { "min", { 0,  0,  1, 0, 0, 0, 0 }, 60, 0, false },
    { "h",   { 0,  0,  1, 0, 0, 0, 0 }, 3600, 0, false },
    { "d",   { 0,  0,  1, 0, 0, 0, 0 }, 86400, 0, false },
    { "pct", { 0,  0,  0, 0, 0, 0, 0 }, 1, -2, false },       /* no dimension: 50pct is 0.5 */
    { "ppm", { 0,  0,  0, 0, 0, 0, 0 }, 1, -6, false },
};

/* The angle is a base dimension of every content. Its coherent unit is the
 * degree, so that degrees are exact (ADR 0001), and a turn is 360 of them. */
static const char *const ANGLE_UNIT = "deg";

static const struct { char sym; int exp10; } prefixes[] = {
    { 'p', -12 }, { 'n', -9 }, { 'u', -6 }, { 'm', -3 }, { 'c', -2 }, { 'd', -1 },
    { 'h', 2 }, { 'k', 3 }, { 'M', 6 }, { 'G', 9 }, { 'T', 12 },
};

static const UnitDef *unit_def(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof unit_defs / sizeof *unit_defs; i++)
        if (strlen(unit_defs[i].sym) == n && !memcmp(unit_defs[i].sym, s, n)) return &unit_defs[i];
    return NULL;
}

/* One symbol: first as a whole (min, mol, cd), then as a prefix and a symbol
 * (mm, kJ). The symbols of declared base dimensions are whole symbols. */
static bool unit_symbol(const Front *c, const char *s, size_t n, QfUnit *out)
{
    memset(out, 0, sizeof *out);
    out->mult = 1;
    const UnitDef *d = unit_def(s, n);
    if (d) {
        memcpy(out->dim.exp, d->exp, sizeof d->exp);
        out->mult = d->mult;
        out->exp10 = d->exp10;
        return true;
    }
    for (size_t i = 0; i < c->bases.n; i++)
        if (strlen(c->bases.v[i]) == n && !memcmp(c->bases.v[i], s, n)) {
            out->dim.exp[Q_NSI + i] = 1;
            return true;
        }
    if (n == 4 && !memcmp(s, "turn", 4))
        for (size_t i = 0; i < c->bases.n; i++)
            if (!strcmp(c->bases.v[i], ANGLE_UNIT)) {
                out->dim.exp[Q_NSI + i] = 1;
                out->mult = 360;
                return true;
            }
    if (n < 2) return false;
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
        if (prefixes[i].sym != s[0]) continue;
        d = unit_def(s + 1, n - 1);
        if (!d || !d->prefix) return false;
        memcpy(out->dim.exp, d->exp, sizeof d->exp);
        out->mult = d->mult;
        out->exp10 = d->exp10 + prefixes[i].exp10;
        return true;
    }
    return false;
}

bool qf_unit_symbol(const Front *c, const char *name)
{
    QfUnit u;
    return unit_symbol(c, name, strlen(name), &u);
}

/* The unit of a literal: terms joined by '*' or '/', each a symbol with an
 * optional exponent. Every term after the first '/' divides, and each needs
 * its own '/': J/kg/K. A '*' after a '/' is an error, because J/kg*K has two
 * readings: (J/kg) K to a programmer, and J/(kg K) in the dimension grammar
 * of ADR 0009 section 10. */
bool qf_unit_parse(const Front *c, const char *text, QfUnit *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    out->mult = 1;
    bool divide = false, compound = strchr(text, '/') || strchr(text, '*');
    for (const char *s = text; *s;) {
        const char *sym = s;
        while (isalnum((unsigned char) *s) || *s == '_') s++;
        size_t n = (size_t) (s - sym);
        QfUnit u;
        if (!n || !unit_symbol(c, sym, n, &u)) {
            if (compound)
                snprintf(err, errlen, "unknown unit '%.*s' in '%s'. If '%.*s' is a name, put spaces around the "
                         "operator, because an operator with a name directly after it continues the unit",
                         (int) n, sym, text, (int) n, sym);
            else
                snprintf(err, errlen, "unknown unit '%s'", text);
            return false;
        }
        long e = 1;
        if (*s == '^') {
            char *end;
            e = strtol(s + 1, &end, 10);
            s = end;
            if (e > 100 || e < -100) { snprintf(err, errlen, "the exponent in the unit '%s' is too large", text); return false; }
        }
        if (divide) e = -e;
        if (e < 0 && u.mult != 1) {
            snprintf(err, errlen, "the unit '%.*s' cannot be a divisor in '%s', because its factor is not a power "
                     "of ten. Write the value with 's'", (int) n, sym, text);
            return false;
        }
        for (int i = 0; i < Q_MAXBASE; i++) {
            long x = out->dim.exp[i] + u.dim.exp[i] * e;
            if (x > 127 || x < -127) { snprintf(err, errlen, "the exponents of the unit '%s' are too large", text); return false; }
            out->dim.exp[i] = (int8_t) x;
        }
        out->exp10 += u.exp10 * (int) e;
        for (long k = 0; k < e; k++)
            if (__builtin_mul_overflow(out->mult, u.mult, &out->mult)) {
                snprintf(err, errlen, "the factor of the unit '%s' is too large", text);
                return false;
            }
        if (*s == '*' && divide) {
            Buf fix = { 0 };
            bool seen = false;
            for (const char *p = text; *p; p++) {
                seen |= *p == '/';
                qf_buf_putc(&fix, *p == '*' && seen ? '/' : *p);
            }
            snprintf(err, errlen, "in the unit '%s', a '*' after a '/' has two readings. Give each divisor its own "
                     "'/': %s", text, fix.s);
            qf_buf_free(&fix);
            return false;
        }
        if (*s == '/') divide = true;
        if (*s == '/' || *s == '*') s++;
        else if (*s) { snprintf(err, errlen, "unknown unit '%s'", text); return false; }
    }
    return true;
}

bool qf_literal(const Front *c, const XNum *n, QDim *dim, int64_t *value, int *exp, char *err, size_t errlen)
{
    QfUnit u = { .mult = 1 };
    if (n->unit && !qf_unit_parse(c, n->unit, &u, err, errlen)) return false;
    *dim = u.dim;
    if (__builtin_mul_overflow(n->mant, u.mult, value) || *value == Q_INF || *value == Q_NINF) {
        snprintf(err, errlen, "the number is out of range");
        return false;
    }
    *exp = u.exp10 - n->frac;
    return true;
}

/* --- dimensions ---------------------------------------------------------------------------- */

static const char *const si_names[Q_NSI] = { "m", "kg", "s", "A", "K", "mol", "cd" };

void qf_dim_format(const Front *c, Buf *b, const QDim *d)
{
    bool any = false;
    for (int i = 0; i < Q_MAXBASE; i++) {
        if (!d->exp[i]) continue;
        const char *name = i < Q_NSI ? si_names[i] : (size_t) (i - Q_NSI) < c->bases.n ? c->bases.v[i - Q_NSI] : "?";
        qf_buf_printf(b, any ? " %s" : "%s", name);
        if (d->exp[i] != 1) qf_buf_printf(b, "^%d", d->exp[i]);
        any = true;
    }
    if (!any) qf_buf_putc(b, '1');
}

uint32_t qf_dim_find(const Front *c, const QDim *d)
{
    for (uint32_t i = 0; i < c->dims.n; i++)
        if (qdim_eq(&c->dims.v[i].dim, d)) return i;
    return QF_BOTTOM;
}

uint32_t qf_dim_index(Front *c, const QDim *d)
{
    uint32_t i = qf_dim_find(c, d);
    if (i != QF_BOTTOM) return i;
    Buf b = { 0 };
    qf_buf_puts(&b, "dim[");
    qf_dim_format(c, &b, d);
    qf_buf_putc(&b, ']');
    DimInfo di = { *d, qf_sym(c, b.s), QF_BOTTOM };
    qf_buf_free(&b);
    VEC_PUSH(c->dims, di);
    return (uint32_t) c->dims.n - 1;
}

uint32_t qf_kind_lookup(const Front *c, Sym name)
{
    uint64_t v;
    return u64map_get(&c->kindidx, (uint64_t) (uintptr_t) name, &v) ? (uint32_t) v - 1 : QF_BOTTOM;
}

static bool kind_name(Sym s)
{
    const char *dot = strchr(s, '.');
    return s[0] == '%' || (dot && dot[1] == '%');
}

/* A kind, or a product or a quotient of kinds: %energy / %time. */
bool qf_x_dim(const Front *c, const X *x, QDim *out)
{
    if (x->k == X_NAME && kind_name(x->u.name)) {
        uint32_t k = qf_kind_lookup(c, x->u.name);
        if (k == QF_BOTTOM) return false;
        *out = c->dims.v[c->kinds.v[k].dimix].dim;
        return true;
    }
    if (x->k == X_ARITH && (x->u.ar.op == '*' || x->u.ar.op == '/')) {
        QDim a, b;
        if (!qf_x_dim(c, x->u.ar.a, &a) || !qf_x_dim(c, x->u.ar.b, &b)) return false;
        return (x->u.ar.op == '*' ? qdim_mul(&a, &b, out) : qdim_div(&a, &b, out)) == Q_OK;
    }
    return false;
}

void qf_dim_scan(Front *c, const X *x)
{
    QDim d;
    char err[256];
    int64_t v;
    int e;
    bool ok = false;
    if (x->k == X_NUM) ok = qf_literal(c, &x->u.num, &d, &v, &e, err, sizeof err);
    else if (x->k == X_BOUND && !x->u.bound.of) ok = qf_literal(c, &x->u.bound.n, &d, &v, &e, err, sizeof err);
    else if (x->k == X_ARITH) ok = qf_x_dim(c, x, &d);
    if (ok) qf_dim_type(c, qf_dim_index(c, &d));
}

/* --- exact decimals ------------------------------------------------------------------------ */

static bool finite(int64_t v) { return v != Q_INF && v != Q_NINF; }

QfDec qf_dec_norm(QfDec d)
{
    bool lo = finite(d.lo) && d.lo, hi = finite(d.hi) && d.hi;
    if (!lo && !hi) { d.exp = 0; return d; }
    while ((!lo || d.lo % 10 == 0) && (!hi || d.hi % 10 == 0)) {
        if (lo) d.lo /= 10;
        if (hi) d.hi /= 10;
        d.exp++;
    }
    return d;
}

/* v × 10^k for k >= 0. False if the result is not a finite value. */
static bool scale_up(int64_t *v, int k)
{
    if (!finite(*v) || !*v) return true;
    for (; k > 0; k--)
        if (__builtin_mul_overflow(*v, (int64_t) 10, v)) return false;
    return finite(*v) && *v != Q_NINF + 1;
}

bool qf_dec_meet(QfDec a, QfDec b, QfDec *out, bool *range)
{
    *range = false;
    a = qf_dec_norm(a);
    b = qf_dec_norm(b);
    int e = a.exp < b.exp ? a.exp : b.exp;
    /* a value that is zero or infinite has no exponent of its own */
    bool afin = (finite(a.lo) && a.lo) || (finite(a.hi) && a.hi), bfin = (finite(b.lo) && b.lo) || (finite(b.hi) && b.hi);
    if (!afin) e = b.exp;
    if (!bfin) e = afin ? a.exp : 0;
    if (!scale_up(&a.lo, a.exp - e) || !scale_up(&a.hi, a.exp - e) || !scale_up(&b.lo, b.exp - e) ||
        !scale_up(&b.hi, b.exp - e)) {
        *range = true;
        return false;
    }
    QfDec r = { 0, 0, e, false, false };
    if (a.lo > b.lo) { r.lo = a.lo; r.lo_open = a.lo_open; }
    else if (b.lo > a.lo) { r.lo = b.lo; r.lo_open = b.lo_open; }
    else { r.lo = a.lo; r.lo_open = a.lo_open || b.lo_open; }
    if (a.hi < b.hi) { r.hi = a.hi; r.hi_open = a.hi_open; }
    else if (b.hi < a.hi) { r.hi = b.hi; r.hi_open = b.hi_open; }
    else { r.hi = a.hi; r.hi_open = a.hi_open || b.hi_open; }
    if (r.lo > r.hi || (r.lo == r.hi && (r.lo_open || r.hi_open))) return false;
    *out = qf_dec_norm(r);
    return true;
}

/* floor(v / 10^k) and whether the division is exact, for k > 0. */
static int64_t div_floor(int64_t v, int k, bool *exact)
{
    int64_t div = 1;
    for (int i = 0; i < k; i++)
        if (__builtin_mul_overflow(div, (int64_t) 10, &div)) {
            *exact = v == 0;                 /* the divisor is larger than every int64 */
            return v < 0 ? -1 : 0;
        }
    int64_t q = v / div, r = v % div;
    if (r < 0) q--;
    *exact = r == 0;
    return q;
}

const char *qf_dec_to_quanta(QfDec d, int scale, int64_t *lo, int64_t *hi)
{
    static const char *const range = "the value does not fit in the range of the kind";
    int k = d.exp - scale;
    bool point = d.lo == d.hi && finite(d.lo);
    *lo = d.lo;
    *hi = d.hi;
    if (k >= 0) {
        if (!scale_up(lo, k) || !scale_up(hi, k)) return range;
        if (d.lo_open && finite(*lo)) ++*lo;
        if (d.hi_open && finite(*hi)) --*hi;
    } else {
        bool exact;
        if (finite(d.lo)) {
            int64_t f = div_floor(d.lo, -k, &exact);
            if (point && !exact) return "the value is not a whole number of quanta of the kind";
            *lo = exact && !d.lo_open ? f : f + 1;          /* inward: the next quantum above */
        }
        if (finite(d.hi)) {
            int64_t f = div_floor(d.hi, -k, &exact);
            *hi = exact && d.hi_open ? f - 1 : f;           /* inward: the next quantum below */
        }
    }
    if (*lo == Q_INF || *hi == Q_NINF || *lo == Q_NINF + 1) return range;
    if (*lo > *hi) return "the range holds no whole number of quanta of the kind";
    return NULL;
}

void qf_dec_format(Buf *b, int64_t v, int exp)
{
    if (v == Q_INF) { qf_buf_puts(b, "inf"); return; }
    if (v == Q_NINF) { qf_buf_puts(b, "-inf"); return; }
    while (v && v % 10 == 0) { v /= 10; exp++; }     /* 5000.000 prints as 5000 */
    uint64_t mag = v < 0 ? (uint64_t) -v : (uint64_t) v;
    if (v < 0) qf_buf_putc(b, '-');
    if (exp > 0 && mag) {
        if (exp <= 6) {
            qf_buf_printf(b, "%llu", (unsigned long long) mag);
            for (int i = 0; i < exp; i++) qf_buf_putc(b, '0');
        } else {
            qf_buf_printf(b, "%llue%d", (unsigned long long) mag, exp);
        }
        return;
    }
    int frac = mag ? -exp : 0;
    qf_buf_printf(b, "%0*llu", frac + 1, (unsigned long long) mag);
    if (frac) {
        size_t point = b->len - (size_t) frac;
        qf_buf_putc(b, b->s[b->len - 1]);
        memmove(b->s + point + 1, b->s + point, (size_t) frac - 1);
        b->s[point] = '.';
    }
}

/* --- kind declarations ---------------------------------------------------------------------- */

static int cmp_symp(const void *a, const void *b) { return strcmp(*(Sym const *) a, *(Sym const *) b); }

static int cmp_kind_decl(const void *a, const void *b)
{
    const Decl *x = *(Decl *const *) a, *y = *(Decl *const *) b;
    int r = name_cmp(x->label, y->label);
    if (r) return r;
    /* two declarations of one kind: the first in the source is the kind */
    r = strcmp(x->loc.file, y->loc.file);
    return r ? r : x->loc.line != y->loc.line ? x->loc.line - y->loc.line : x->loc.col - y->loc.col;
}

static const XField *kind_field(const X *body, const char *label)
{
    for (int i = 0; i < body->u.st.n; i++)
        if (body->u.st.fields[i].label && !strcmp(body->u.st.fields[i].label, label)) return &body->u.st.fields[i];
    return NULL;
}

/* The text of a string literal, or NULL. The result is interned. */
static Sym literal_text(Front *c, const X *x)
{
    if (x->k != X_ATOM || x->u.name[0] != '"') return NULL;     /* true and false are atoms too */
    Buf b = { 0 };
    qf_string_decode(x->u.name, &b);
    Sym s = qf_sym(c, b.s);
    qf_buf_free(&b);
    return s;
}

/* The built-in kinds of the SI base dimensions. A declaration can change one. */
static const struct { const char *name; int base; } base_kinds[] = {
    { "%length", Q_BASE_LENGTH }, { "%mass", Q_BASE_MASS }, { "%current", Q_BASE_CURRENT },
    { "%temperature", Q_BASE_TEMPERATURE }, { "%amount", Q_BASE_AMOUNT }, { "%luminosity", Q_BASE_LUMINOUS },
};

bool qf_builtin_kind(const char *name)
{
    for (size_t i = 0; i < sizeof base_kinds / sizeof *base_kinds; i++)
        if (!strcmp(name, base_kinds[i].name)) return true;
    return !strcmp(name, "%time") || !strcmp(name, "%count") || !strcmp(name, "%ratio") || !strcmp(name, "%angle");
}

static void add_kind(Front *c, Sym name, const QDim *dim, int scale, bool conserved, int rounding, Sym base, Loc loc,
                     bool builtin, bool fixed)
{
    KindInfo k = { name, qf_dim_index(c, dim), (int8_t) scale, conserved, (uint8_t) rounding, base, loc, builtin, fixed,
                   QF_BOTTOM };
    VEC_PUSH(c->kinds, k);
    u64map_put(&c->kindidx, (uint64_t) (uintptr_t) name, c->kinds.n);
}

static bool valid_symbol(const char *s)
{
    if (!isalpha((unsigned char) *s)) return false;
    for (; *s; s++)
        if (!isalnum((unsigned char) *s) && *s != '_') return false;
    return true;
}

bool qf_kinds_build(Front *c)
{
    size_t errors0 = c->nerrors;
    VEC(Decl *) ds = { 0 };
    for (size_t i = 0; i < c->decls.n; i++)
        if (c->decls.v[i].kind == L_KIND) VEC_PUSH(ds, &c->decls.v[i]);
    if (ds.n) qsort(ds.v, ds.n, sizeof *ds.v, cmp_kind_decl);

    /* the body is literal data: a struct of known fields */
    static const char *const known[] = { "dimension", "base", "scale", "conserved", "rounding" };
    for (size_t i = 0; i < ds.n; i++) {
        const Decl *d = ds.v[i];
        const X *body = d->value;
        if (body->k != X_STRUCT) {
            qf_diag(c, true, body->loc, "the body of the kind %s must be a struct of literal fields: "
                    "{ dimension: \"J\", scale: -3 }", d->label);
            continue;
        }
        for (int f = 0; f < body->u.st.n; f++) {
            const XField *fl = &body->u.st.fields[f];
            bool ok = false;
            for (size_t k = 0; fl->label && k < sizeof known / sizeof *known; k++) ok |= !strcmp(fl->label, known[k]);
            if (!ok)
                qf_diag(c, true, fl->loc, "a kind has the fields dimension, base, scale, conserved and rounding, not %s",
                        fl->label ? fl->label : "a computed field");
        }
    }
    if (c->nerrors > errors0) { VEC_FREE(ds); return false; }

    /* base dimensions first, in the order of their symbols: every dimension
     * text can then name them. The angle is always one of them. */
    VEC_PUSH(c->bases, qf_sym(c, ANGLE_UNIT));
    for (size_t i = 0; i < ds.n; i++) {
        const XField *bf = kind_field(ds.v[i]->value, "base");
        if (!bf) continue;
        Sym sym = literal_text(c, bf->value);
        if (!sym || !valid_symbol(sym)) {
            qf_diag(c, true, bf->value->loc, "base is the unit symbol of a new base dimension, as a string: base: \"coin\"");
        } else if (qf_unit_symbol(c, sym)) {
            bool declared = false;
            for (size_t k = 1; k < c->bases.n; k++) declared |= c->bases.v[k] == sym;      /* 0 is the angle's */
            qf_diag(c, true, bf->value->loc, declared ? "the base dimension '%s' is already declared"
                                                      : "'%s' is already a unit symbol", sym);
        } else {
            VEC_PUSH(c->bases, sym);
        }
    }
    if (c->bases.n > Q_MAXBASE - Q_NSI)
        qf_diag(c, true, ds.v[0]->loc, "a world can declare %d base dimensions, and this content declares %zu",
                Q_MAXBASE - Q_NSI - 1, c->bases.n - 1);
    if (c->nerrors > errors0) { VEC_FREE(ds); return false; }
    if (c->bases.n) qsort(c->bases.v, c->bases.n, sizeof(Sym), cmp_symp);

    /* the built-in kinds: %time has the tick of the world, and a count and a
     * ratio have their scales by definition, so content cannot change those */
    Loc noloc = { 0 };
    QDim none = qdim_none(), time = qdim_base(Q_BASE_TIME), angle = qdim_none();
    for (size_t k = 0; k < c->bases.n; k++)
        if (!strcmp(c->bases.v[k], ANGLE_UNIT)) angle = qdim_base(Q_NSI + (int) k);
    add_kind(c, qf_sym(c, "%count"), &none, 0, false, Q_ROUND_HALF_EVEN, NULL, noloc, true, true);
    add_kind(c, qf_sym(c, "%ratio"), &none, -9, false, Q_ROUND_HALF_EVEN, NULL, noloc, true, true);
    add_kind(c, qf_sym(c, "%time"), &time, c->tick_scale, false, Q_ROUND_HALF_EVEN, NULL, noloc, true, true);
    for (size_t k = 0; k < sizeof base_kinds / sizeof *base_kinds; k++) {
        QDim d = qdim_base(base_kinds[k].base);
        add_kind(c, qf_sym(c, base_kinds[k].name), &d, qdim_default_scale(&d, c->tick_scale), false, Q_ROUND_HALF_EVEN,
                 NULL, noloc, true, false);
    }
    add_kind(c, qf_sym(c, "%angle"), &angle, qdim_default_scale(&angle, c->tick_scale), false, Q_ROUND_HALF_EVEN,
             qf_sym(c, ANGLE_UNIT), noloc, true, false);

    for (size_t i = 0; i < ds.n; i++) {
        const Decl *d = ds.v[i];
        const X *body = d->value;
        uint32_t prev = qf_kind_lookup(c, d->label);
        /* a built-in base kind can have one declaration, which changes its
         * scale, conserved or rounding and keeps its dimension */
        bool change = false;
        if (prev != QF_BOTTOM) {
            const KindInfo *p = &c->kinds.v[prev];
            if (!p->builtin) {
                qf_diag(c, true, d->loc, "the kind %s is already declared at %s:%d", d->label, p->loc.file, p->loc.line);
                continue;
            }
            if (p->fixed) {
                qf_diag(c, true, d->loc, "%s is a built-in kind that content cannot change%s", d->label,
                        !strcmp(d->label, "%time") ? ": its scale is the tick of the world" : "");
                continue;
            }
            change = true;
        }
        const XField *df = kind_field(body, "dimension"), *bf = kind_field(body, "base"), *sf = kind_field(body, "scale"),
                     *cf = kind_field(body, "conserved"), *rf = kind_field(body, "rounding");
        QDim dim = qdim_none();
        Sym base = NULL;
        if (change && bf) {
            qf_diag(c, true, bf->loc, "the built-in kind %s keeps its dimension: a declaration can change its scale, "
                    "conserved and rounding", d->label);
            continue;
        } else if (change && !df) {
            dim = c->dims.v[c->kinds.v[prev].dimix].dim;
        } else if (df && bf) {
            qf_diag(c, true, bf->loc, "the kind %s has a dimension and a base: give one of them", d->label);
            continue;
        } else if (bf) {
            base = literal_text(c, bf->value);
            for (size_t k = 0; k < c->bases.n; k++)
                if (c->bases.v[k] == base) dim = qdim_base(Q_NSI + (int) k);
        } else if (df) {
            Sym text = literal_text(c, df->value);
            if (!text || q_parse_dim(text, strlen(text), (const char *const *) c->bases.v, c->bases.n, &dim) != Q_OK) {
                qf_diag(c, true, df->value->loc, "the dimension of the kind %s is not valid. Write the coherent units "
                        "as a string, for example \"J/K\" or \"kg m^2 s^-2\", or \"1\" for no dimension", d->label);
                continue;
            }
        } else {
            qf_diag(c, true, d->loc, "the kind %s needs a dimension (dimension: \"J\") or a new base dimension "
                    "(base: \"coin\")", d->label);
            continue;
        }
        if (change && !qdim_eq(&dim, &c->dims.v[c->kinds.v[prev].dimix].dim)) {
            qf_diag(c, true, df->value->loc, "the built-in kind %s keeps its dimension: a declaration can change its "
                    "scale, conserved and rounding", d->label);
            continue;
        }
        int scale = qdim_default_scale(&dim, c->tick_scale);
        if (sf) {
            const X *v = sf->value;
            if (v->k != X_NUM || v->u.num.unit || v->u.num.frac || v->u.num.mant < -60 || v->u.num.mant > 60) {
                qf_diag(c, true, v->loc, "the scale of the kind %s must be an integer from -60 to 60", d->label);
                continue;
            }
            scale = (int) v->u.num.mant;
        }
        bool conserved = false;
        if (cf) {
            const X *v = cf->value;
            if (v->k != X_ATOM || (strcmp(v->u.name, "true") && strcmp(v->u.name, "false"))) {
                qf_diag(c, true, v->loc, "conserved must be true or false");
                continue;
            }
            conserved = !strcmp(v->u.name, "true");
        }
        int rounding = Q_ROUND_HALF_EVEN;
        if (rf) {
            static const char *const names[] = { "half-even", "floor", "ceiling", "zero" };
            Sym text = literal_text(c, rf->value);
            int r = -1;
            for (int k = 0; text && k < 4; k++)
                if (!strcmp(text, names[k])) r = k;
            if (r < 0) {
                qf_diag(c, true, rf->value->loc, "rounding must be \"half-even\", \"floor\", \"ceiling\" or \"zero\"");
                continue;
            }
            rounding = r;
        }
        if (change) {
            KindInfo *k = &c->kinds.v[prev];
            k->scale = (int8_t) scale;
            k->conserved = conserved;
            k->rounding = (uint8_t) rounding;
            k->loc = d->loc;
            k->builtin = false;              /* a second declaration is an error, like that of every kind */
        } else {
            add_kind(c, d->label, &dim, scale, conserved, rounding, base, d->loc, false, false);
        }
    }
    VEC_FREE(ds);
    return c->nerrors == errors0;
}
