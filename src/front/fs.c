#include "front/front.h"
#include "quad/ir.h"

#include <stdio.h>

/* Feature structure nodes, Tomabechi quasi-destructive unification, copying
 * with structure sharing, subsumption, generalization, and printing.
 *
 * Borrowed from Triangle's fs.c (bruggadung f425574), under ADR 0003.
 * Changes from Triangle (ADR 0015):
 *   - An operation keeps the scratch of each node in its context (QfOp), in
 *     a table for each store, and not in the node. So two contexts can work
 *     on one content at the same time.
 *   - A number is a kind value in quanta, or an exact decimal with a
 *     dimension. A decimal converts when unification gives it a kind.
 *   - Text has no regular-expression constraints yet (quad-ijx.11). */

/* --- contexts and stores ------------------------------------------------------------------ */

QfStore *qf_store_new(void)
{
    QfStore *s = qf_xcalloc(1, sizeof *s);
    s->arena = qf_arena_new();
    return s;
}

void qf_store_free(QfStore *s)
{
    if (!s) return;
    qf_arena_free(s->arena);
    free(s);
}

QfOp *qf_op_new(Front *f)
{
    QfOp *o = qf_xcalloc(1, sizeof *o);
    o->f = f;
    o->scratch.arena = qf_arena_new();
    /* A new slot holds generation 0. So no operation uses 0, and the
     * generation outside any operation (1) is never handed out either. */
    o->genctr = 1;
    o->gen = 1;
    o->tscratch.store = &o->scratch;
    o->tcontent.store = &f->store;
    return o;
}

static void table_free(SlotTable *t)
{
    for (size_t i = 0; i < t->npages; i++) free(t->pages[i]);
    free(t->pages);
}

void qf_op_free(QfOp *o)
{
    if (!o) return;
    qf_arena_free(o->scratch.arena);
    table_free(&o->tscratch);
    table_free(&o->tcontent);
    for (size_t i = 0; i < o->tother.n; i++) table_free(&o->tother.v[i]);
    VEC_FREE(o->tother);
    free(o->pathstack);
    free(o->failpath);
    free(o);
}

/* The scratch of the op's own arena, with the count of its nodes. */
typedef struct { QfMark m; uint32_t n; } ScratchMark;

static ScratchMark scratch_mark(QfOp *c)
{
    ScratchMark m = { qf_arena_mark(c->scratch.arena), c->scratch.n };
    return m;
}

static void scratch_release(QfOp *c, ScratchMark m)
{
    qf_arena_release(c->scratch.arena, m.m);
    c->scratch.n = m.n;
}

#define SLOT_PAGE 256

/* The context's slot for a node: by the node's store, then by its id. A
 * page is zero when it is made, and no generation is zero, so a new slot
 * holds nothing. A slot never moves. */
static Slot *slot(QfOp *c, const QfNode *n)
{
    SlotTable *t = NULL;
    if (n->store == &c->scratch) t = &c->tscratch;
    else if (n->store == &c->f->store) t = &c->tcontent;
    else {
        for (size_t i = 0; i < c->tother.n && !t; i++)
            if (c->tother.v[i].store == n->store) t = &c->tother.v[i];
        if (!t) {
            SlotTable nt = { n->store, NULL, 0 };
            VEC_PUSH(c->tother, nt);
            t = &c->tother.v[c->tother.n - 1];
        }
    }
    size_t pg = n->id / SLOT_PAGE;
    if (pg >= t->npages) {
        size_t np = t->npages ? t->npages : 8;
        while (np <= pg) np *= 2;
        t->pages = qf_xrealloc(t->pages, np * sizeof *t->pages);
        memset(t->pages + t->npages, 0, (np - t->npages) * sizeof *t->pages);
        t->npages = np;
    }
    if (!t->pages[pg]) t->pages[pg] = qf_xcalloc(SLOT_PAGE, sizeof(Slot));
    return &t->pages[pg][n->id % SLOT_PAGE];
}

uint32_t op_meet(QfOp *c, uint32_t a, uint32_t b)
{
    if (a == b) return a;
    if (a == QF_BOTTOM || b == QF_BOTTOM) return QF_BOTTOM;
    uint64_t key = a < b ? ((uint64_t) a << 32 | b) : ((uint64_t) b << 32 | a);
    size_t i = (size_t) (qf_hash64(key) & 1023);
    if (c->meetcache[i].key == key + 1) return c->meetcache[i].val;
    uint32_t r = meet_pure(c->f, a, b);
    c->meetcache[i].key = key + 1;
    c->meetcache[i].val = r;
    return r;
}

/* --- generations ------------------------------------------------------------------ */

/* Each operation gets a fresh generation; scratch from other operations becomes
 * invalid without being cleared. Nested operations (lazy expansion during a
 * unification) save and restore the outer generation. */
uint64_t op_begin(QfOp *c)
{
    uint64_t saved = c->gen;
    c->gen = ++c->genctr;
    c->opdepth++;
    return saved;
}

void op_end(QfOp *c, uint64_t saved)
{
    c->gen = saved;
    c->opdepth--;
}

/* --- the path stack ------------------------------------------------------------- */

/* The path stack has no bound: a conflict at item 2000 of a list is 2002
 * features deep. Growth doubles, so a push is constant time on average. */
static uint32_t *grow_path(uint32_t *v, size_t *cap, size_t n)
{
    if (n <= *cap) return v;
    size_t cap2 = *cap ? *cap : 64;
    while (cap2 < n) cap2 *= 2;
    v = qf_xrealloc(v, cap2 * sizeof *v);
    *cap = cap2;
    return v;
}

void path_push(QfOp *c, uint32_t f)
{
    if ((size_t) c->depth == c->pathcap) c->pathstack = grow_path(c->pathstack, &c->pathcap, (size_t) c->depth + 1);
    c->pathstack[c->depth++] = f;
}

void path_set(QfOp *c, const uint32_t *path, size_t n)
{
    c->pathstack = grow_path(c->pathstack, &c->pathcap, n);
    if (n) memcpy(c->pathstack, path, n * sizeof(uint32_t));
    c->depth = (int) n;
}

/* --- nodes ------------------------------------------------------------------------ */

QfNode *node_new(QfOp *c, QfStore *a, uint32_t type)
{
    QfNode *n = qf_alloc_raw(a->arena, sizeof(QfNode));
    n->type = type;
    n->narcs = 0;
    n->flags = a == &c->scratch ? NF_SCRATCH : 0;
    n->id = a->n++;
    n->exp = 0;
    n->store = a;
    if (is_value(c->f, type)) {
        n->flags |= NF_VALUE;
        n->lo = Q_NINF;
        n->hi = Q_INF;
    } else {
        n->arcs = NULL;
        n->hi = 0;
    }
    return n;
}

QfNode *leaf_new(QfOp *c, QfStore *a, uint32_t type, int64_t lo, int64_t hi)
{
    QfNode *n = node_new(c, a, type);
    n->lo = lo;
    n->hi = hi;
    return n;
}

Scratch *sx_get(QfOp *c, const QfNode *n)
{
    Slot *sl = slot(c, n);
    return sl->sxgen == c->gen ? sl->sx : NULL;
}

Scratch *sx_make(QfOp *c, const QfNode *n)
{
    Slot *sl = slot(c, n);
    if (sl->sxgen != c->gen) {
        Scratch *s = qf_alloc_raw(c->scratch.arena, sizeof(Scratch));
        s->fwd = s->bwd = NULL;
        s->carcs = NULL;
        s->ncarcs = s->ccap = 0;
        s->hasiv = false;
        s->forced = false;
        sl->sx = s;
        sl->sxgen = c->gen;
    }
    return sl->sx;
}

QfNode *deref(QfOp *c, QfNode *n)
{
    for (;;) {
        Scratch *s = sx_get(c, n);
        if (!s || !s->fwd) return n;
        n = s->fwd;
    }
}

static QfVal own_val(const QfNode *n)
{
    QfVal v = { Q_NINF, Q_INF, 0, 0 };
    if (n->flags & NF_VALUE) {
        v.lo = n->lo;
        v.hi = n->hi;
        v.exp = n->exp;
        v.open = (uint8_t) (n->flags & (NF_LO_OPEN | NF_HI_OPEN));
    }
    return v;
}

QfVal node_val(QfOp *c, const QfNode *n)
{
    Scratch *s = sx_get(c, n);
    return s && s->hasiv ? s->val : own_val(n);
}

static void set_val(QfNode *n, QfVal v)
{
    n->lo = v.lo;
    n->hi = v.hi;
    n->exp = v.exp;
    n->flags = (uint16_t) ((n->flags & ~(NF_LO_OPEN | NF_HI_OPEN)) | v.open);
}

QfNode *node_arc(const QfNode *n, uint32_t f)
{
    size_t lo = 0, hi = n->narcs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (n->arcs[mid].feat == f) return n->arcs[mid].val;
        if (n->arcs[mid].feat < f) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

QfNode *carc_find(QfOp *c, QfNode *n, uint32_t f)
{
    QfNode *v = n->narcs ? node_arc(n, f) : NULL;
    if (v) return v;
    Scratch *s = sx_get(c, n);
    if (s)
        for (uint32_t i = 0; i < s->ncarcs; i++)
            if (s->carcs[i].feat == f) return s->carcs[i].val;
    return NULL;
}

void carc_add(QfOp *c, QfNode *n, uint32_t f, QfNode *v)
{
    Scratch *s = sx_make(c, n);
    if (s->ncarcs == s->ccap) {
        uint32_t ncap = s->ccap ? s->ccap * 2 : 4;
        Arc *na = qf_alloc_raw(c->scratch.arena, ncap * sizeof(Arc));
        if (s->ncarcs) memcpy(na, s->carcs, s->ncarcs * sizeof(Arc));
        s->carcs = na;
        s->ccap = ncap;
    }
    s->carcs[s->ncarcs].feat = f;
    s->carcs[s->ncarcs].val = v;
    s->ncarcs++;
}

static uint32_t arc_count(QfOp *c, QfNode *n)
{
    Scratch *s = sx_get(c, n);
    return ((n->flags & NF_VALUE) ? 0 : n->narcs) + (s ? s->ncarcs : 0);
}

/* --- values ------------------------------------------------------------------------------- */

static bool is_num(TypeKind k) { return k == TK_KIND || k == TK_DIMT || k == TK_NUMBER; }
static bool open_val(QfVal v) { return v.lo == Q_NINF && v.hi == Q_INF; }

static QfDec val_dec(QfVal v)
{
    QfDec d = { v.lo, v.hi, v.exp, (v.open & NF_LO_OPEN) != 0, (v.open & NF_HI_OPEN) != 0 };
    return d;
}

static QfVal dec_val(QfDec d)
{
    QfVal v = { d.lo, d.hi, d.exp, (uint8_t) ((d.lo_open ? NF_LO_OPEN : 0) | (d.hi_open ? NF_HI_OPEN : 0)) };
    return v;
}

static int kind_scale(const Front *f, uint32_t t) { return f->kinds.v[f->types.v[t].kindix].scale; }

/* The value of a node of type `type` as quanta of the kind t. A decimal
 * converts (ADR 0015 section 3). NULL, or the reason that it cannot. */
static const char *as_quanta(const Front *f, uint32_t t, uint32_t type, QfVal v, int64_t *lo, int64_t *hi)
{
    if (type == t) { *lo = v.lo; *hi = v.hi; return NULL; }
    return qf_dec_to_quanta(val_dec(v), kind_scale(f, t), lo, hi);
}

/* The value of a number node as a decimal: quanta of a kind are × 10^scale. */
static QfDec as_dec(const Front *f, uint32_t type, QfVal v)
{
    QfDec d = val_dec(v);
    if (f->types.v[type].kind == TK_KIND) {
        d.exp = kind_scale(f, type);
        d.lo_open = d.hi_open = false;
    }
    return d;
}

/* Meets the values of two nodes whose types meet in t. */
static bool val_meet(QfOp *c, uint32_t t, uint32_t ta, QfVal a, uint32_t tb, QfVal b, QfVal *out, const char **why)
{
    const Front *f = c->f;
    TypeKind k = f->types.v[t].kind;
    if (k == TK_KIND) {
        int64_t alo, ahi, blo, bhi;
        if ((*why = as_quanta(f, t, ta, a, &alo, &ahi)) || (*why = as_quanta(f, t, tb, b, &blo, &bhi))) return false;
        QfVal r = { alo > blo ? alo : blo, ahi < bhi ? ahi : bhi, 0, 0 };
        *out = r;
        return r.lo <= r.hi;
    }
    if (is_num(k)) {
        QfDec d;
        bool range;
        if (!qf_dec_meet(val_dec(a), val_dec(b), &d, &range)) {
            if (range) *why = "the number is out of range";
            return false;
        }
        *out = dec_val(d);
        return true;
    }
    /* text, constraint: any value, or one value */
    if (open_val(a)) *out = b;
    else if (open_val(b) || a.lo == b.lo) *out = a;
    else return false;
    return true;
}

static bool same_val(QfVal a, QfVal b)
{
    return a.lo == b.lo && a.hi == b.hi && a.exp == b.exp && a.open == b.open;
}

/* --- failure recording ---------------------------------------------------------------- */

static void describe_num(Buf *b, const char *op, int64_t v, int exp)
{
    qf_buf_puts(b, op);
    qf_dec_format(b, v, exp);
}

/* A value as it could stand in source: %energy & >=0 & <=100, with the
 * numbers in coherent units. */
static void describe_value(const Front *f, Buf *b, uint32_t type, QfVal v)
{
    const TypeInfo *ti = &f->types.v[type];
    if (!is_value(f, type)) { qf_buf_puts(b, ti->name); return; }
    if (ti->kind == TK_TEXT || ti->kind == TK_CONSTRAINT) {
        if (v.lo != v.hi) qf_buf_puts(b, ti->name);
        else if (ti->kind == TK_TEXT) qf_buf_puts(b, (const char *) (intptr_t) v.lo);
        else qf_print(b, (const X *) (intptr_t) v.lo);
        return;
    }
    QfDec d = as_dec(f, type, v);
    bool plain = ti->kind == TK_DIMT && qdim_is_none(&f->dims.v[ti->dimix].dim);   /* 5, not dim[1] & 5 */
    if (open_val(v) || !plain) qf_buf_puts(b, ti->name);
    if (open_val(v)) return;
    const char *sep = plain ? "" : " & ";
    if (d.lo == d.hi) { describe_num(b, sep, d.lo, d.exp); return; }
    if (d.lo != Q_NINF) {
        qf_buf_puts(b, sep);
        describe_num(b, d.lo_open ? ">" : ">=", d.lo, d.exp);
        sep = " & ";
    }
    if (d.hi != Q_INF) {
        qf_buf_puts(b, sep);
        describe_num(b, d.hi_open ? "<" : "<=", d.hi, d.exp);
    }
}

void path_format(const Front *f, Buf *b, const uint32_t *path, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (i) qf_buf_putc(b, '.');
        qf_buf_puts(b, f->feats.v[path[i]].name);
    }
    if (!n) qf_buf_puts(b, "(root)");
}

/* A path as text in fewer than `len` bytes. A path too long for that keeps its
 * first and last features and counts those it leaves out between them:
 * items.rest.rest.<140 more>.rest.first. Cut at the end, it would lose the
 * part that says where the conflict is. */
static void path_format_fit(const Front *f, Buf *b, size_t len, const uint32_t *path, size_t n)
{
    path_format(f, b, path, n);
    if (b->len < len) return;
    b->len = 0;
    b->s[0] = 0;
    size_t head = 0, hl = 0, tail = 0, tl = 0;
    while (head < n && hl + strlen(f->feats.v[path[head]].name) + 1 <= len / 3)
        hl += strlen(f->feats.v[path[head++]].name) + 1;
    /* ".<N more>." with N up to 20 digits */
    size_t room = len - 1 - hl - 32;
    while (tail < n - head && tl + strlen(f->feats.v[path[n - 1 - tail]].name) + 1 <= room)
        tl += strlen(f->feats.v[path[n - 1 - tail++]].name) + 1;
    for (size_t i = 0; i < head; i++) {
        qf_buf_puts(b, f->feats.v[path[i]].name);
        qf_buf_putc(b, '.');
    }
    qf_buf_printf(b, "<%zu more>", n - head - tail);
    for (size_t i = n - tail; i < n; i++) {
        qf_buf_putc(b, '.');
        qf_buf_puts(b, f->feats.v[path[i]].name);
    }
}

/* Records a failure cheaply; most failures are never looked at, so the text
 * is made only when fail_info() is asked for it. */
static void fail_at(QfOp *c, QfNode *a, QfNode *b, const char *why)
{
    if (c->failed) return;
    c->failed = true;
    c->failfmt = false;
    c->faildepth = c->depth;
    c->failpath = grow_path(c->failpath, &c->failcap, (size_t) c->depth);
    if (c->depth) memcpy(c->failpath, c->pathstack, sizeof(uint32_t) * (size_t) c->depth);
    c->failraw.why = why;
    c->failraw.ltype = a->type;
    c->failraw.l = node_val(c, a);
    c->failraw.hasr = b != NULL;
    if (b) {
        c->failraw.rtype = b->type;
        c->failraw.r = node_val(c, b);
    }
}

/* --- a failure in the words of the author ------------------------------------------------- */

static bool is_quantity(const Front *f, uint32_t t)
{
    TypeKind k = f->types.v[t].kind;
    return k == TK_KIND || k == TK_DIMT;
}

/* The unit of a quantity type: V, K, m^2. Empty for a plain number. */
static void unit_text(const Front *f, Buf *b, uint32_t type)
{
    Buf d = { 0 };
    qf_dim_format(f, &d, &f->dims.v[f->types.v[type].dimix].dim);
    qf_buf_puts(b, qi_unit(d.s));
    qf_buf_free(&d);
    if (!b->s) qf_buf_puts(b, "");
}

static void say_num(Buf *b, const char *op, int64_t v, int exp, const char *unit)
{
    qf_buf_puts(b, op);
    qf_dec_format(b, v, exp);
    if (*unit) qf_buf_printf(b, " %s", unit);
}

/* A side of a failure: 12 A, >= 20 V, %voltage, a quantity in W, "text", #Lamp. */
static void say_value(const Front *f, Buf *b, uint32_t type, QfVal v)
{
    const TypeInfo *ti = &f->types.v[type];
    if (ti->kind == TK_NUMBER) { qf_buf_puts(b, "a number"); return; }
    if (!is_quantity(f, type)) { describe_value(f, b, type, v); return; }
    Buf u = { 0 };
    unit_text(f, &u, type);
    if (open_val(v)) {
        if (ti->kind == TK_KIND) qf_buf_puts(b, ti->name);
        else qf_buf_printf(b, u.len ? "a quantity in %s" : "a number%s", u.s);
    } else {
        QfDec d = as_dec(f, type, v);
        bool lo = d.lo != Q_NINF, hi = d.hi != Q_INF;
        if (d.lo == d.hi) {
            say_num(b, "", d.lo, d.exp, u.s);
        } else if (lo && hi && !d.lo_open && !d.hi_open) {
            say_num(b, "the range ", d.lo, d.exp, u.s);
            say_num(b, " .. ", d.hi, d.exp, u.s);
        } else {
            if (lo && hi) qf_buf_puts(b, "the range ");
            if (lo) say_num(b, d.lo_open ? "> " : ">= ", d.lo, d.exp, u.s);
            if (lo && hi) qf_buf_puts(b, " and ");
            if (hi) say_num(b, d.hi_open ? "< " : "<= ", d.hi, d.exp, u.s);
        }
    }
    qf_buf_free(&u);
}

/* The side has one value, and is not only a type. */
static bool concrete(const Front *f, uint32_t type, QfVal v)
{
    TypeKind k = f->types.v[type].kind;
    if (k == TK_ATOM) return true;
    if (!is_value(f, type)) return false;
    return k == TK_TEXT || k == TK_CONSTRAINT ? v.lo == v.hi : !open_val(v);
}

/* "<value> is not ...": of the kind %voltage, a quantity in W, text, one of "p" | "q". */
static void say_is_not(const Front *f, Buf *b, const char *value, uint32_t type, const char *text)
{
    TypeKind k = f->types.v[type].kind;
    qf_buf_printf(b, "%s is not %s%s", value,
                  k == TK_KIND ? "of the kind " : k == TK_DIMT || k == TK_NUMBER || k == TK_TEXT ? ""
                  : strstr(text, " | ") ? "one of " : "of the type ", text);
}

/* v + 273.15, as text: the kelvin value for a number of degrees Celsius. False if it does not fit. */
static bool kelvin_text(Buf *b, QfDec d)
{
    int64_t v = d.lo, k = 27315;
    int e = d.exp, ke = -2;
    for (; e > ke; e--)
        if (__builtin_mul_overflow(v, (int64_t) 10, &v)) return false;
    for (; ke > e; ke--)
        if (__builtin_mul_overflow(k, (int64_t) 10, &k)) return false;
    if (__builtin_add_overflow(v, k, &v)) return false;
    qf_dec_format(b, v, e);
    return true;
}

/* Two quantities with different dimensions. */
static void explain_units(const Front *f, Buf *b, const char *l, const char *r, uint32_t lt, uint32_t rt, QfVal lv,
                          QfVal rv, const char *lu, const char *ru)
{
    bool lc = concrete(f, lt, lv), rc = concrete(f, rt, rv);
    if (lc == rc) {
        if (lc) qf_buf_printf(b, "%s and %s have different units", l, r);
        else qf_buf_printf(b, "%s and %s are different: the unit of the first is %s, and the unit of the second is %s", l, r,
                           *lu ? lu : "1", *ru ? ru : "1");
        return;
    }
    /* a value and a type */
    const char *val = lc ? l : r, *type = lc ? r : l, *vu = lc ? lu : ru, *tu = lc ? ru : lu;
    uint32_t tt = lc ? rt : lt;
    bool kind = f->types.v[tt].kind == TK_KIND;
    if (!*vu) {
        qf_buf_printf(b, "%s has no unit, and %s%s", val, kind ? type : "the field is ", kind ? "" : type);
        if (kind) qf_buf_printf(b, " has the unit %s", tu);
    } else if (!*tu) {
        qf_buf_printf(b, "%s has a unit, and %s%s", val, kind ? type : "the field is a number with no unit",
                      kind ? " is a number with no unit" : "");
    } else {
        say_is_not(f, b, val, tt, type);
        if (kind) qf_buf_printf(b, ". The unit of %s is %s", type, tu);
    }
    if (!strcmp(vu, "C") && !strcmp(tu, "K")) {
        QfDec d = as_dec(f, lc ? lt : rt, lc ? lv : rv);
        Buf k = { 0 }, c = { 0 };
        qf_buf_puts(b, ". The unit C is the coulomb. Write a temperature in kelvin");
        if (d.lo == d.hi && kelvin_text(&k, d)) {
            qf_dec_format(&c, d.lo, d.exp);
            qf_buf_printf(b, ": %s degrees Celsius is %sK", c.s, k.s);
        }
        qf_buf_free(&k);
        qf_buf_free(&c);
    }
}

/* A value that does not fit the steps of a kind. `why` is a reason of qf_dec_to_quanta. */
static bool explain_steps(const Front *f, Buf *b, const char *why, const char *val, QfDec d, uint32_t kt, const char *unit)
{
    const KindInfo *k = &f->kinds.v[f->types.v[kt].kindix];
    const char *name = f->types.v[kt].name;
    Buf step = { 0 };
    say_num(&step, "", 1, k->scale, unit);
    bool known = true;
    if (!strcmp(why, "the value is not a multiple of the smallest step of the kind")) {
        qf_buf_printf(b, "%s is not a multiple of the smallest step of %s, which is %s", val, name, step.s);
        if (!k->fixed) qf_buf_printf(b, ". For a smaller step, declare %s with scale: %d", name, qf_dec_norm(d).exp);
    } else if (!strcmp(why, "the range holds no multiple of the smallest step of the kind")) {
        qf_buf_printf(b, "no multiple of the smallest step of %s, which is %s, is in %s", name, step.s, val);
        if (!k->fixed) qf_buf_printf(b, ". For a smaller step, declare %s with a smaller scale than %d", name, k->scale);
    } else if (!strcmp(why, "the value does not fit in the range of the kind")) {
        qf_buf_printf(b, "%s is too large for %s, which has the smallest step %s", val, name, step.s);
        if (!k->fixed) qf_buf_printf(b, ". For larger values, declare %s with a larger scale than %d", name, k->scale);
    } else {
        known = false;
    }
    qf_buf_free(&step);
    return known;
}

void qf_say_fields(const Front *f, Buf *b, uint32_t type)
{
    const TypeInfo *ti = &f->types.v[type];
    if (!ti->feats.n) return;
    qf_buf_printf(b, ". The fields of %s are: ", ti->kind == TK_DEF ? ti->name : "this type");
    enum { MOST = 12 };
    for (size_t i = 0; i < ti->feats.n && i < MOST; i++)
        qf_buf_printf(b, "%s%s", i ? ", " : "", f->feats.v[ti->feats.v[i]].name);
    if (ti->feats.n > MOST) qf_buf_printf(b, ", and %zu more", ti->feats.n - MOST);
}

/* The failure as one sentence, with no path: "12 A is not of the kind
 * %voltage. The unit of %voltage is V". */
static void explain(QfOp *c, Buf *b)
{
    const Front *f = c->f;
    uint32_t lt = c->failraw.ltype, rt = c->failraw.rtype;
    QfVal lv = c->failraw.l, rv = c->failraw.r;
    const char *why = c->failraw.why;
    Buf l = { 0 }, r = { 0 }, lu = { 0 }, ru = { 0 };
    say_value(f, &l, lt, lv);
    if (!c->failraw.hasr) {
        qf_buf_printf(b, "%s %s", l.s, why ? why : "has no value");
        if (why && !strcmp(why, "has no field with this name")) qf_say_fields(f, b, lt);
        qf_buf_free(&l);
        return;
    }
    say_value(f, &r, rt, rv);
    bool lq = is_quantity(f, lt), rq = is_quantity(f, rt);
    bool lc = concrete(f, lt, lv), rc = concrete(f, rt, rv);
    bool lk = f->types.v[lt].kind == TK_KIND, rk = f->types.v[rt].kind == TK_KIND;
    if (lq) unit_text(f, &lu, lt);
    if (rq) unit_text(f, &ru, rt);
    bool far = why && !strcmp(why, "the number is out of range");
    if (lk && rk && lt != rt && (!lc || !rc || f->types.v[lt].dimix == f->types.v[rt].dimix)) {
        qf_buf_printf(b, "%s and %s are different kinds", f->types.v[lt].name, f->types.v[rt].name);
    } else if (lq && rq && f->types.v[lt].dimix != f->types.v[rt].dimix) {
        explain_units(f, b, l.s, r.s, lt, rt, lv, rv, lu.s, ru.s);
    } else if (far) {
        qf_buf_printf(b, "%s and %s are too far apart for a comparison", l.s, r.s);
    } else if (lq && rq && why && lk != rk &&
               explain_steps(f, b, why, lk ? r.s : l.s, val_dec(lk ? rv : lv), lk ? lt : rt, lk ? lu.s : ru.s)) {
        /* done */
    } else if (lc != rc && !(lq && rq)) {
        say_is_not(f, b, lc ? l.s : r.s, lc ? rt : lt, lc ? r.s : l.s);
    } else if (f->types.v[lt].kind == TK_CONSTRAINT && f->types.v[rt].kind == TK_CONSTRAINT && lc && rc) {
        qf_buf_printf(b, "the law has two texts: %s, and %s", l.s, r.s);
    } else if (lc && rc && (!lq || (lv.lo == lv.hi && rv.lo == rv.hi))) {
        qf_buf_printf(b, "%s and %s are different values", l.s, r.s);
    } else if (lq && rq) {
        qf_buf_printf(b, "%s and %s have no value in common", l.s, r.s);
    } else {
        qf_buf_printf(b, "a value cannot be both %s and %s", l.s, r.s);
    }
    if (why && !far && !(lq && rq && lk != rk)) qf_buf_printf(b, " (%s)", why);
    qf_buf_free(&l);
    qf_buf_free(&r);
    qf_buf_free(&lu);
    qf_buf_free(&ru);
}

QfFailure *fail_info(QfOp *c)
{
    if (c->failfmt) return &c->fail_;
    c->failfmt = true;
    memset(&c->fail_, 0, sizeof c->fail_);
    if (!c->failed) return &c->fail_;
    Buf p = { 0 }, l = { 0 }, r = { 0 };
    path_format_fit(c->f, &p, sizeof c->fail_.path, c->failpath, (size_t) c->faildepth);
    describe_value(c->f, &l, c->failraw.ltype, c->failraw.l);
    if (c->failraw.hasr) describe_value(c->f, &r, c->failraw.rtype, c->failraw.r);
    snprintf(c->fail_.path, sizeof c->fail_.path, "%s", p.s ? p.s : "");
    snprintf(c->fail_.left, sizeof c->fail_.left, "%s", l.s ? l.s : "");
    snprintf(c->fail_.right, sizeof c->fail_.right, "%s", r.s ? r.s : "");
    snprintf(c->fail_.why, sizeof c->fail_.why, "%s", c->failraw.why ? c->failraw.why : "");
    Buf t = { 0 };
    explain(c, &t);
    snprintf(c->fail_.text, sizeof c->fail_.text, "%s", t.s ? t.s : "");
    qf_buf_free(&t);
    qf_buf_free(&p);
    qf_buf_free(&l);
    qf_buf_free(&r);
    return &c->fail_;
}

/* --- unification ------------------------------------------------------------------------ */

static bool is_text(QfOp *c, const QfNode *n)
{
    return (n->flags & NF_VALUE) && n->type == c->f->ttext;
}

/* An enum atom meeting free text becomes text: its name, as a string. */
static QfNode *atom_as_text(QfOp *c, QfNode *atom)
{
    int64_t q = (int64_t) (intptr_t) c->f->types.v[atom->type].name;
    QfNode *t = leaf_new(c, &c->scratch, c->f->ttext, q, q);
    sx_make(c, atom)->fwd = t;
    return t;
}

bool unify1(QfOp *c, QfNode *a, QfNode *b)
{
    const Front *f = c->f;
    a = deref(c, a);
    b = deref(c, b);
    if (a == b) return true;
    if ((a->flags ^ b->flags) & NF_VALUE) {
        if (is_text(c, a) && f->types.v[b->type].kind == TK_ATOM) b = atom_as_text(c, b);
        else if (is_text(c, b) && f->types.v[a->type].kind == TK_ATOM) a = atom_as_text(c, a);
    }
    uint32_t t = op_meet(c, a->type, b->type);
    if (t == QF_BOTTOM) { fail_at(c, a, b, NULL); return false; }

    if (t != a->type && t != b->type) {
        /* A new, more specific type: its constraint must hold too. */
        QfNode *n = type_instance(c, t, &c->scratch);
        if (!n) { fail_at(c, a, b, "the constraint of the type cannot be expanded"); return false; }
        return unify1(c, n, a) && unify1(c, n, b);
    }

    QfNode *target, *src;
    if (t == a->type && t == b->type) {
        bool a_more = arc_count(c, a) >= arc_count(c, b);
        target = a_more ? a : b;
        src = a_more ? b : a;
    } else if (t == a->type) {
        target = a; src = b;
    } else {
        target = b; src = a;
    }

    if ((target->flags & NF_LAZY) && is_lazy(c, target) && arc_count(c, src) && f->types.v[target->type].xstate == 2) {
        /* structure is being added to a lazy node: apply its constraint first */
        QfNode *e = type_expansion(c, target->type);
        sx_make(c, target)->forced = true;
        if (e) {
            QfNode *m = copy_raw(c, &c->scratch, e);
            if (!unify1(c, m, target)) return false;
            target = deref(c, target);
        }
    }

    if (target->flags & NF_VALUE) {
        QfVal tv = node_val(c, target), sv = node_val(c, src), r;
        const char *why = NULL;
        if (!val_meet(c, t, target->type, tv, src->type, sv, &r, &why)) { fail_at(c, a, b, why); return false; }
        if (!same_val(r, tv)) {
            Scratch *s = sx_make(c, target);
            s->val = r;
            s->hasiv = true;
        }
    }

    sx_make(c, src)->fwd = target;

    /* src's own arcs never change during unification; its comp-arcs might (if
     * it is reached again as a target), so those are taken as a snapshot. */
    Scratch *ss = sx_get(c, src);
    uint32_t nown = (src->flags & NF_VALUE) ? 0 : src->narcs, ncomp = ss ? ss->ncarcs : 0;
    if (!nown && !ncomp) return true;
    Arc *comp = NULL;
    if (ncomp) {
        comp = qf_alloc_raw(c->scratch.arena, ncomp * sizeof(Arc));
        memcpy(comp, ss->carcs, ncomp * sizeof(Arc));
    }
    /* Both own arc lists are sorted by feature: walk them in step, and look in
     * the target's comp-arcs only for features its own arcs lack. */
    uint32_t tn = (target->flags & NF_VALUE) ? 0 : target->narcs, j = 0;
    for (uint32_t i = 0; i < nown + ncomp; i++) {
        const Arc *arc = i < nown ? &src->arcs[i] : &comp[i - nown];
        uint32_t ft = arc->feat;
        QfNode *tv = NULL;
        if (i < nown) {
            while (j < tn && target->arcs[j].feat < ft) j++;
            if (j < tn && target->arcs[j].feat == ft) tv = target->arcs[j].val;
        } else {
            tv = tn ? node_arc(target, ft) : NULL;
        }
        if (!tv) {
            Scratch *ts = sx_get(c, target);
            if (ts)
                for (uint32_t k = 0; k < ts->ncarcs; k++)
                    if (ts->carcs[k].feat == ft) { tv = ts->carcs[k].val; break; }
        }
        path_push(c, ft);
        if (tv) {
            if (!unify1(c, tv, arc->val)) return false;
        } else if (!approp(f, t, ft)) {
            fail_at(c, target, NULL, "has no field with this name");
            return false;
        } else {
            carc_add(c, target, ft, arc->val);
        }
        c->depth--;
    }
    return true;
}

/* --- copying ------------------------------------------------------------------------------ */

static int cmp_arc(const void *a, const void *b)
{
    uint32_t x = ((const Arc *) a)->feat, y = ((const Arc *) b)->feat;
    return x < y ? -1 : x > y;
}

bool is_lazy(QfOp *c, const QfNode *n)
{
    if (!(n->flags & NF_LAZY)) return false;
    Scratch *s = sx_get(c, n);
    return !(s && s->forced);
}

/* A lazy node as a full structure, in scratch: its type's expansion unified
 * with what it carries. Used by read-only passes that need its arcs. */
QfNode *materialize(QfOp *c, QfNode *n)
{
    if (!is_lazy(c, n) || c->f->types.v[n->type].xstate != 2) return n;
    QfNode *e = type_expansion(c, n->type);
    if (!e) return n;
    uint64_t saved = op_begin(c);
    bool savedfailed = c->failed;
    int saveddepth = c->depth;
    QfNode *m = copy_raw(c, &c->scratch, e);
    QfNode *r = n;
    if (unify1(c, m, n)) r = copy_out(c, &c->scratch, m, NULL);
    c->failed = savedfailed;
    c->depth = saveddepth;
    op_end(c, saved);
    return r;
}

static QfNode *copy_raw_rec(QfOp *c, QfStore *a, QfNode *n, uint64_t cg)
{
    Slot *sl = slot(c, n);
    if (sl->cpygen == cg) return sl->cpy;
    QfNode *m = node_new(c, a, n->type);
    m->flags |= n->flags & NF_LAZY;
    if (n->flags & NF_VALUE) set_val(m, own_val(n));
    sl->cpy = m;
    sl->cpygen = cg;
    if (n->flags & NF_VALUE) return m;
    m->narcs = n->narcs;
    if (n->narcs) {
        m->arcs = qf_alloc(a->arena, n->narcs * sizeof(Arc));
        for (uint32_t i = 0; i < n->narcs; i++) {
            m->arcs[i].feat = n->arcs[i].feat;
            m->arcs[i].val = copy_raw_rec(c, a, n->arcs[i].val, cg);
        }
    }
    return m;
}

/* A pristine copy, ignoring scratch state: used to instantiate type constraints. */
QfNode *copy_raw(QfOp *c, QfStore *a, QfNode *n)
{
    return copy_raw_rec(c, a, n, ++c->cgenctr);
}

#define INPROGRESS ((QfNode *) (uintptr_t) 1)

/* Copies the result of the current operation out of scratch, following forwards,
 * narrowed values, and comp-arcs. With `share`, a node from a long-lived
 * store that is unchanged, with unchanged descendants, is reused rather than
 * copied (structure sharing). */
static QfNode *copy_out_rec(QfOp *c, QfStore *a, QfNode *n, uint64_t cg, bool *ok, bool share)
{
    n = deref(c, n);
    Slot *sl = slot(c, n);
    if (sl->cpygen == cg) {
        if (sl->cpy == INPROGRESS) { c->cyclic = true; return n; }
        return sl->cpy;
    }
    sl->cpygen = cg;
    Scratch *s = sx_get(c, n);
    bool same = share && !(n->flags & NF_SCRATCH) && !(s && (s->ncarcs || s->hasiv));
    uint32_t nown = (n->flags & NF_VALUE) ? 0 : n->narcs;
    uint32_t na = nown + (s ? s->ncarcs : 0);
    Arc *arcs = NULL;
    Arc local[32];
    if (na) {
        sl->cpy = INPROGRESS;
        arcs = na <= 32 ? local : qf_alloc_raw(c->scratch.arena, na * sizeof(Arc));
        if (nown) memcpy(arcs, n->arcs, nown * sizeof(Arc));
        if (s && s->ncarcs) {
            memcpy(arcs + nown, s->carcs, s->ncarcs * sizeof(Arc));
            qsort(arcs, na, sizeof(Arc), cmp_arc);
        }
        for (uint32_t i = 0; i < na; i++) {
            QfNode *v = copy_out_rec(c, a, arcs[i].val, cg, ok, share);
            if (v != arcs[i].val) same = false;
            arcs[i].val = v;
        }
    }
    if (same) {
        sl->cpy = n;
        return n;
    }
    QfNode *m = node_new(c, a, n->type);
    if (is_lazy(c, n)) m->flags |= NF_LAZY;
    if (n->flags & NF_VALUE) set_val(m, node_val(c, n));
    if (na) {
        m->arcs = qf_alloc_raw(a->arena, na * sizeof(Arc));
        memcpy(m->arcs, arcs, na * sizeof(Arc));
        m->narcs = (uint16_t) na;
        for (uint32_t i = 0; i < na; i++)
            if (!approp(c->f, m->type, arcs[i].feat)) *ok = false;
    }
    sl->cpy = m;
    return m;
}

/* The copy of a node that the last copy_out made. */
QfNode *last_copy(QfOp *c, QfNode *n)
{
    return slot(c, deref(c, n))->cpy;
}

/* Copies several roots out of one operation together, so nodes they share
 * stay shared. */
void copy_out_many(QfOp *c, QfStore *a, QfNode **roots, size_t n)
{
    bool ok = true;
    c->cyclic = false;
    uint64_t cg = ++c->cgenctr;
    QfNode **in = qf_xmalloc(sizeof(QfNode *) * (n ? n : 1));
    memcpy(in, roots, sizeof(QfNode *) * n);
    for (size_t i = 0; i < n; i++) roots[i] = copy_out_rec(c, a, in[i], cg, &ok, true);
    if (c->cyclic) {
        c->cyclic = false;
        cg = ++c->cgenctr;
        for (size_t i = 0; i < n; i++) roots[i] = copy_out_rec(c, a, in[i], cg, &ok, false);
    }
    free(in);
}

QfNode *copy_out(QfOp *c, QfStore *a, QfNode *n, bool *closed_ok)
{
    bool ok = true;
    c->cyclic = false;
    QfNode *r = copy_out_rec(c, a, n, ++c->cgenctr, &ok, true);
    if (c->cyclic) {
        /* sharing can't decide on a cycle; copy everything instead */
        ok = true;
        c->cyclic = false;
        r = copy_out_rec(c, a, n, ++c->cgenctr, &ok, false);
    }
    if (closed_ok) *closed_ok = ok;
    return r;
}

/* --- extensional equality and subsumption --------------------------------------------------- */

/* Leaves are extensional: two nodes with the same maximal featureless type, or
 * the same exact value, count as the same value. */
bool ext_equal(const Front *f, const QfNode *a, const QfNode *b)
{
    if (a == b) return true;
    if (a->type != b->type) return false;
    if (a->flags & NF_VALUE)
        return a->lo == a->hi && b->lo == b->hi && a->lo == b->lo && a->exp == b->exp &&
               !((a->flags | b->flags) & (NF_LO_OPEN | NF_HI_OPEN));
    return is_leaf(f, a->type);
}

/* Every value of s is a value of g. The type of s is a subtype of the type of g. */
static bool val_within(const Front *f, const QfNode *g, const QfNode *s)
{
    QfVal gv = own_val(g), sv = own_val(s);
    TypeKind k = f->types.v[s->type].kind;
    if (!is_num(k)) return open_val(gv) || (gv.lo == sv.lo && gv.hi == sv.hi);
    if (k == TK_KIND) {
        int64_t lo, hi;
        if (as_quanta(f, s->type, g->type, gv, &lo, &hi)) return false;     /* g holds no value of the kind */
        return sv.lo >= lo && sv.hi <= hi;
    }
    QfDec m;
    bool range;
    if (!qf_dec_meet(val_dec(gv), val_dec(sv), &m, &range)) return false;
    return same_val(dec_val(m), dec_val(qf_dec_norm(val_dec(sv))));
}

typedef struct { bool fwd, bwd; } SubState;

static void subsume_rec(QfOp *c, QfNode *a, QfNode *b, SubState *s)
{
    const Front *f = c->f;
    /* A lazy node stands for its type's expansion with what it carries. Compare
     * against that, unless both are lazy (then their carried arcs decide). */
    if (is_lazy(c, a) != is_lazy(c, b)) {
        if (is_lazy(c, b) && a->narcs) b = materialize(c, b);
        else if (is_lazy(c, a) && b->narcs) a = materialize(c, a);
    }
    Scratch *sa = sx_make(c, a), *sb = sx_make(c, b);
    bool amapped = sa->fwd != NULL, bmapped = sb->bwd != NULL;
    if (amapped && bmapped && sa->fwd == b && sb->bwd == a) return;
    if (amapped && sa->fwd != b && !ext_equal(f, sa->fwd, b)) s->fwd = false;
    if (bmapped && sb->bwd != a && !ext_equal(f, sb->bwd, a)) s->bwd = false;
    if (!amapped) sa->fwd = b;
    if (!bmapped) sb->bwd = a;
    if (!is_subtype(f, b->type, a->type)) s->fwd = false;
    if (!is_subtype(f, a->type, b->type)) s->bwd = false;
    if ((a->flags & NF_VALUE) && (b->flags & NF_VALUE)) {
        if (s->fwd && !val_within(f, a, b)) s->fwd = false;
        if (s->bwd && !val_within(f, b, a)) s->bwd = false;
        return;
    }
    if ((a->flags | b->flags) & NF_VALUE) return;
    if (!s->fwd && !s->bwd) return;
    uint32_t i = 0, j = 0;
    while (i < a->narcs && j < b->narcs && (s->fwd || s->bwd)) {
        if (a->arcs[i].feat == b->arcs[j].feat) {
            subsume_rec(c, a->arcs[i].val, b->arcs[j].val, s);
            i++; j++;
        } else if (a->arcs[i].feat < b->arcs[j].feat) {
            i++;
        } else {
            j++;
        }
    }
}

void qf_subsumes(QfOp *c, const QfNode *a, const QfNode *b, bool *forward, bool *backward)
{
    ScratchMark m = scratch_mark(c);
    uint64_t saved = op_begin(c);
    SubState s = { true, true };
    subsume_rec(c, (QfNode *) a, (QfNode *) b, &s);
    op_end(c, saved);
    scratch_release(c, m);
    if (forward) *forward = s.fwd;
    if (backward) *backward = s.bwd;
}

/* --- paths ------------------------------------------------------------------------------------ */

QfNode *node_at(QfNode *n, const uint32_t *path, size_t len)
{
    for (size_t i = 0; i < len && n; i++) n = (n->flags & NF_VALUE) ? NULL : node_arc(n, path[i]);
    return n;
}

/* Like node_at, but a lazy node with no arc for the next feature stands for
 * its type's expansion (which lives as long as the content): continue there. */
const QfNode *qf_at(QfOp *c, const QfNode *node, const uint32_t *path, size_t len)
{
    QfNode *n = (QfNode *) node;
    for (size_t i = 0; i < len && n; i++) {
        if (n->flags & NF_VALUE) return NULL;
        QfNode *next = node_arc(n, path[i]);
        if (!next && (n->flags & NF_LAZY)) {
            QfNode *e = type_expansion(c, n->type);
            next = e ? node_arc(e, path[i]) : NULL;
        }
        n = next;
    }
    return n;
}

/* --- printing ------------------------------------------------------------------------------------ */

typedef struct { U64Map refs; U64Map tags; int ntags; } PrintState;

static void count_refs(const QfNode *n, U64Map *refs)
{
    uint64_t k = (uint64_t) (uintptr_t) n, v = 0;
    if (u64map_get(refs, k, &v)) { u64map_put(refs, k, v + 1); return; }
    u64map_put(refs, k, 1);
    if (n->flags & NF_VALUE) return;
    for (uint32_t i = 0; i < n->narcs; i++) count_refs(n->arcs[i].val, refs);
}

static void print_rec(const Front *f, Buf *b, const QfNode *n, PrintState *ps)
{
    uint64_t k = (uint64_t) (uintptr_t) n, v = 0;
    u64map_get(&ps->refs, k, &v);
    if (v > 1) {
        uint64_t tag;
        if (u64map_get(&ps->tags, k, &tag)) { qf_buf_printf(b, "$%llu", (unsigned long long) tag); return; }
        u64map_put(&ps->tags, k, (uint64_t) ++ps->ntags);
        qf_buf_printf(b, "$%d & ", ps->ntags);
    }
    const TypeInfo *ti = &f->types.v[n->type];
    if (n->flags & NF_VALUE) {
        describe_value(f, b, n->type, own_val(n));
        return;
    }
    if (ti->kind == TK_ANON) qf_buf_putc(b, '(');
    if (n->type != f->top || !n->narcs) qf_buf_puts(b, ti->name);
    if (ti->kind == TK_ANON) qf_buf_putc(b, ')');
    if (!n->narcs) return;
    qf_buf_puts(b, n->type != f->top ? " & { " : "{ ");
    for (uint32_t i = 0; i < n->narcs; i++) {
        if (i) qf_buf_puts(b, ", ");
        qf_buf_printf(b, "%s: ", f->feats.v[n->arcs[i].feat].name);
        print_rec(f, b, n->arcs[i].val, ps);
    }
    qf_buf_puts(b, " }");
}

void qf_node_print(const Front *f, Buf *b, const QfNode *n)
{
    PrintState ps = { 0 };
    count_refs(n, &ps.refs);
    print_rec(f, b, n, &ps);
    u64map_free(&ps.refs);
    u64map_free(&ps.tags);
}

/* --- public unification ----------------------------------------------------------------------------- */

const QfNode *qf_unify(QfOp *c, const QfNode *a, const QfNode *b, QfStore *out, QfFailure *failure)
{
    ScratchMark m = scratch_mark(c);
    uint64_t saved = op_begin(c);
    c->failed = false;
    c->depth = 0;
    QfNode *r = NULL;
    if (unify1(c, (QfNode *) a, (QfNode *) b)) {
        bool ok;
        r = copy_out(c, out, (QfNode *) a, &ok);
    } else if (failure) {
        *failure = *fail_info(c);
    }
    op_end(c, saved);
    scratch_release(c, m);
    return r;
}

/* A copy that shares nothing with its source (apart from type expansions a
 * lazy node stands for), keeping its own shared nodes shared. */
const QfNode *qf_copy(QfOp *c, const QfNode *node, QfStore *out)
{
    return copy_raw(c, out, (QfNode *) node);
}

/* --- generalisation of two structures ----------------------------------------------------------- */

/* A least upper bound of two types: a minimal common ancestor, preferring one
 * within `bound` (the type the context allows), then the highest ID, so the
 * choice is deterministic. */
static uint32_t join_type(const Front *f, uint32_t a, uint32_t b, uint32_t bound)
{
    if (is_subtype(f, a, b)) return b;
    if (is_subtype(f, b, a)) return a;
    U32Vec queue = { 0 }, common = { 0 };
    U64Map seen = { 0 };
    VEC_PUSH(queue, a);
    u64map_put(&seen, a, 1);
    for (size_t i = 0; i < queue.n; i++) {
        uint32_t t = queue.v[i];
        if (is_subtype(f, b, t)) { VEC_PUSH(common, t); continue; }   /* its ancestors are not minimal */
        for (size_t k = 0; k < f->types.v[t].parents.n; k++) {
            uint32_t p = f->types.v[t].parents.v[k];
            if (!u64map_get(&seen, p, NULL)) { u64map_put(&seen, p, 1); VEC_PUSH(queue, p); }
        }
    }
    uint32_t best = f->top;
    bool best_in = false;
    for (size_t i = 0; i < common.n; i++) {
        uint32_t t = common.v[i];
        bool minimal = true;
        for (size_t j = 0; j < common.n && minimal; j++)
            if (common.v[j] != t && is_subtype(f, common.v[j], t)) minimal = false;
        if (!minimal) continue;
        bool in = bound == QF_BOTTOM || is_subtype(f, t, bound);
        if ((in && !best_in) || (in == best_in && (best == f->top || t > best))) { best = t; best_in = in; }
    }
    VEC_FREE(queue);
    VEC_FREE(common);
    u64map_free(&seen);
    return best;
}

/* The smallest decimal interval that holds both. A bound that cannot be
 * written in one power of ten with the other becomes infinite. */
static QfDec dec_hull(QfDec a, QfDec b)
{
    a = qf_dec_norm(a);
    b = qf_dec_norm(b);
    int e = a.exp < b.exp ? a.exp : b.exp;
    int64_t v[4] = { a.lo, a.hi, b.lo, b.hi };
    int ex[4] = { a.exp, a.exp, b.exp, b.exp };
    for (int i = 0; i < 4; i++) {
        if (v[i] == Q_INF || v[i] == Q_NINF || !v[i]) continue;
        for (int k = ex[i] - e; k > 0; k--)
            if (__builtin_mul_overflow(v[i], (int64_t) 10, &v[i]) || v[i] == Q_INF || v[i] == Q_NINF) {
                v[i] = (i & 1) ? Q_INF : Q_NINF;
                break;
            }
    }
    QfDec r = { 0, 0, e, false, false };
    if (v[0] < v[2]) { r.lo = v[0]; r.lo_open = a.lo_open; }
    else if (v[2] < v[0]) { r.lo = v[2]; r.lo_open = b.lo_open; }
    else { r.lo = v[0]; r.lo_open = a.lo_open && b.lo_open; }
    if (v[1] > v[3]) { r.hi = v[1]; r.hi_open = a.hi_open; }
    else if (v[3] > v[1]) { r.hi = v[3]; r.hi_open = b.hi_open; }
    else { r.hi = v[1]; r.hi_open = a.hi_open && b.hi_open; }
    if (r.lo == Q_NINF) r.lo_open = false;
    if (r.hi == Q_INF) r.hi_open = false;
    return qf_dec_norm(r);
}

typedef struct { QfNode *a, *b, *r; } GenPair;
typedef struct { VEC(GenPair) pairs; U64Map idx; } GenState;

static QfNode *gen_rec(QfOp *c, QfStore *out, QfNode *a, QfNode *b, uint32_t bound, GenState *st)
{
    const Front *f = c->f;
    uint64_t key = qf_hash64((uint64_t) (uintptr_t) a * 31 + (uint64_t) (uintptr_t) b), v;
    for (uint64_t k = key; u64map_get(&st->idx, k, &v); k = qf_hash64(k)) {
        GenPair *gp = &st->pairs.v[v - 1];
        if (gp->a == a && gp->b == b) return gp->r;
    }
    if (a == b && !(a->flags & NF_SCRATCH)) return a;
    uint32_t t = join_type(f, a->type, b->type, bound);
    if (is_lazy(c, a) && is_lazy(c, b)) {
        /* both stand for their type's expansion: so does the result (what they
         * carry beyond it is dropped, which only generalises further) */
        QfNode *r = node_new(c, out, t);
        r->flags |= f->types.v[t].feats.n ? NF_LAZY : 0;
        return r;
    }
    if (is_lazy(c, a)) a = materialize(c, a);
    if (is_lazy(c, b)) b = materialize(c, b);
    QfNode *r = node_new(c, out, t);
    GenPair gp = { a, b, r };
    VEC_PUSH(st->pairs, gp);
    uint64_t k = key;
    while (u64map_get(&st->idx, k, NULL)) k = qf_hash64(k);
    u64map_put(&st->idx, k, st->pairs.n);
    if (r->flags & NF_VALUE) {
        bool both = (a->flags & NF_VALUE) && (b->flags & NF_VALUE);
        TypeKind tk = f->types.v[t].kind;
        if (!both || tk == TK_NUMBER) return r;                 /* any value of the type */
        if (tk == TK_KIND) {
            r->lo = a->lo < b->lo ? a->lo : b->lo;
            r->hi = a->hi > b->hi ? a->hi : b->hi;
        } else if (tk == TK_DIMT) {
            set_val(r, dec_val(dec_hull(as_dec(f, a->type, own_val(a)), as_dec(f, b->type, own_val(b)))));
        } else if (a->lo == a->hi && a->lo == b->lo && b->lo == b->hi) {
            r->lo = r->hi = a->lo;                             /* the same text, the same constraint */
        }
        return r;
    }
    const TypeInfo *ti = &f->types.v[t];
    if (!ti->feats.n || ((a->flags | b->flags) & NF_VALUE)) return r;
    QfNode *e = type_expansion(c, t);
    r->arcs = qf_alloc(out->arena, ti->feats.n * sizeof(Arc));
    for (size_t i = 0; i < ti->feats.n; i++) {
        uint32_t ft = ti->feats.v[i];
        QfNode *ev = e ? node_arc(e, ft) : NULL;
        QfNode *va = node_arc(a, ft), *vb = node_arc(b, ft);
        QfNode *val = va && vb ? gen_rec(c, out, va, vb, ev ? ev->type : QF_BOTTOM, st)
                    : ev ? copy_raw(c, out, ev) : NULL;
        if (!val) continue;
        r->arcs[r->narcs].feat = ft;
        r->arcs[r->narcs].val = val;
        r->narcs++;
    }
    return r;
}

/* The most specific structure that subsumes both (anti-unification). */
QfNode *fs_generalise(QfOp *c, QfStore *out, QfNode *a, QfNode *b)
{
    GenState st = { 0 };
    ScratchMark m = scratch_mark(c);
    QfNode *r = gen_rec(c, out, a, b, QF_BOTTOM, &st);
    if (out != &c->scratch) scratch_release(c, m);
    VEC_FREE(st.pairs);
    u64map_free(&st.idx);
    return r;
}

const QfNode *qf_generalize(QfOp *c, const QfNode *a, const QfNode *b, QfStore *out)
{
    return fs_generalise(c, out, (QfNode *) a, (QfNode *) b);
}

/* --- building a structure from handles (ADR 0015 section 8) --------------------------------------- */

/* A node of a type with the given arcs. The same value node at two features
 * is one shared node. The constraint of the type is not applied: unify the
 * result with qf_type_node(f, type) for that. NULL if a feature occurs twice
 * or the type is a value type. */
QfNode *qf_build_struct(QfOp *c, QfStore *s, uint32_t type, const uint32_t *feats, QfNode *const *vals, size_t n)
{
    if (is_value(c->f, type) || n > UINT16_MAX) return NULL;
    QfNode *r = node_new(c, s, type);
    if (!n) return r;
    r->arcs = qf_alloc(s->arena, n * sizeof(Arc));
    for (size_t i = 0; i < n; i++) {
        r->arcs[i].feat = feats[i];
        r->arcs[i].val = vals[i];
    }
    qsort(r->arcs, n, sizeof(Arc), cmp_arc);
    for (size_t i = 1; i < n; i++)
        if (r->arcs[i].feat == r->arcs[i - 1].feat) return NULL;
    r->narcs = (uint16_t) n;
    return r;
}

/* A quantity of a kind: an interval of quanta. NULL if the type is not a kind. */
QfNode *qf_build_quantity(QfOp *c, QfStore *s, uint32_t kind_type, QInterval v)
{
    if (kind_type >= c->f->types.n || c->f->types.v[kind_type].kind != TK_KIND || v.lo > v.hi) return NULL;
    return leaf_new(c, s, kind_type, v.lo, v.hi);
}

/* A text value. */
QfNode *qf_build_text(QfOp *c, QfStore *s, const char *bytes)
{
    int64_t q = (int64_t) (intptr_t) qf_string_canonical(c->f, bytes, strlen(bytes));
    return leaf_new(c, s, c->f->ttext, q, q);
}

/* The constraint of a type, as a structure of the content. */
const QfNode *qf_type_node(const Front *f, uint32_t type)
{
    return type < f->types.n ? f->types.v[type].expansion : NULL;
}
