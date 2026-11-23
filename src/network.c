#include "quad/network.h"

#include <stdlib.h>
#include <string.h>

const char *qn_status_str(QnStatus s)
{
    switch (s) {
    case QN_OK:           return "ok";
    case QN_CONTRADICTION: return "contradiction";
    case QN_BUDGET:       return "propagation budget used";
    case QN_ETYPE:        return "quantity types do not fit";
    case QN_ECELL:        return "no such cell";
    case QN_EVALUE:       return "invalid interval";
    case QN_ESTAGED:      return "cell already staged for this boundary";
    case QN_EPROPAGATING: return "non-monotonic call during propagation";
    case QN_ENOMEM:       return "out of memory";
    case QN_EGROUP:       return "no such group";
    case QN_EDOMAIN:      return "operation does not fit the cell's domain";
    }
    return "unknown status";
}

/* --- storage ----------------------------------------------------------------- */

typedef enum { P_SUM, P_PRODUCT, P_EQUAL, P_CUSTOM, P_SETEQ, P_ELEMENT, P_REIFY, P_LE } PKind;

typedef struct {
    PKind  kind;
    bool   queued;
    QnGroup group;
    QnCell c[3];          /* sum, product: z, x, y. equal: a, b. */
    QConv  k[6];          /* plans; see the run functions */
    QnFn   fn;            /* custom */
    void  *ctx;
    size_t first, n;      /* custom: cells in net->ccells[first .. first + n);
                             element: net->tables[first .. first + n) */
    QInterval range;      /* reify */
    bool   strict;        /* less-or-equal: a < b */
} Prop;

typedef struct { uint32_t *ids; size_t n, cap; } List;

enum { D_INTERVAL, D_SET };

typedef struct {
    QInterval base, cur, staged;
    bool      has_staged;
    uint8_t   domain;
    QType     type;
    List      watchers;   /* propagators, in the order they were added */
} Cell;

struct QNet {
    Cell    *cells;
    size_t   ncells, capcells;
    Prop    *props;
    size_t   nprops, capprops;
    QnCell  *ccells;      /* the cell lists of custom propagators */
    size_t   nccells, capccells;
    QInterval *tables;    /* the tables of element propagators */
    size_t   ntables, captables;

    uint32_t *queue;      /* a ring of propagator ids, capacity capprops */
    size_t    qhead, qcount;

    bool     *groups;     /* enabled, for each group */
    size_t    ngroups, capgroups;
    QnGroup   current;

    uint64_t epoch, runs, budget;
    bool     propagating, contradicted;
    QnCell   conflict;
};

static bool grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 8;
    while (n < need) n *= 2;
    void *q = realloc(*p, n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

QNet *qn_new(void)
{
    QNet *net = calloc(1, sizeof(QNet));
    if (!net) return NULL;
    if (!grow((void **) &net->groups, &net->capgroups, 1, sizeof *net->groups)) {
        free(net);
        return NULL;
    }
    net->groups[0] = true;
    net->ngroups = 1;
    return net;
}

void qn_free(QNet *net)
{
    if (!net) return;
    for (size_t i = 0; i < net->ncells; i++) free(net->cells[i].watchers.ids);
    free(net->cells);
    free(net->props);
    free(net->ccells);
    free(net->tables);
    free(net->queue);
    free(net->groups);
    free(net);
}

/* --- the agenda ----------------------------------------------------------------- */

static void enqueue(QNet *net, uint32_t p)
{
    if (net->props[p].queued || !net->groups[net->props[p].group]) return;
    net->props[p].queued = true;
    net->queue[(net->qhead + net->qcount++) % net->capprops] = p;
}

static uint32_t dequeue(QNet *net)
{
    uint32_t p = net->queue[net->qhead];
    net->qhead = (net->qhead + 1) % net->capprops;
    net->qcount--;
    net->props[p].queued = false;
    return p;
}

static void clear_queue(QNet *net)
{
    while (net->qcount) dequeue(net);
    net->qhead = 0;
}

/* Grows the props array and the ring together, keeping the queue order. */
static bool reserve_prop(QNet *net)
{
    if (net->nprops < net->capprops) return true;
    size_t cap = net->capprops;
    if (!grow((void **) &net->props, &cap, net->nprops + 1, sizeof(Prop))) return false;
    uint32_t *q = malloc(cap * sizeof *q);
    if (!q) return false;
    for (size_t i = 0; i < net->qcount; i++) q[i] = net->queue[(net->qhead + i) % net->capprops];
    free(net->queue);
    net->queue = q;
    net->qhead = 0;
    net->capprops = cap;
    return true;
}

/* --- cells ------------------------------------------------------------------------ */

static bool valid_cell(const QNet *net, QnCell c) { return c < net->ncells; }

QnStatus qn_cell(QNet *net, const QType *type, QInterval base, QnCell *out)
{
    if (!qi_valid(base)) return QN_EVALUE;
    if (type->width != 64) return QN_ETYPE;
    if (!grow((void **) &net->cells, &net->capcells, net->ncells + 1, sizeof(Cell))) return QN_ENOMEM;
    Cell *c = &net->cells[net->ncells];
    memset(c, 0, sizeof *c);
    c->base = c->cur = base;
    c->type = *type;
    *out = (QnCell) net->ncells++;
    return QN_OK;
}

size_t qn_ncells(const QNet *net) { return net->ncells; }
QInterval qn_get(const QNet *net, QnCell c) { return valid_cell(net, c) ? net->cells[c].cur : qi_top(); }
QInterval qn_base(const QNet *net, QnCell c) { return valid_cell(net, c) ? net->cells[c].base : qi_top(); }
const QType *qn_type(const QNet *net, QnCell c) { return valid_cell(net, c) ? &net->cells[c].type : NULL; }

/* A set cell's storage: the mask in lo, the universe in hi, copied bit for bit. */
static uint64_t word(int64_t v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    return u;
}

static QInterval set_value(uint64_t mask, uint64_t universe)
{
    QInterval v;
    memcpy(&v.lo, &mask, sizeof mask);
    memcpy(&v.hi, &universe, sizeof universe);
    return v;
}

QInterval qn_set_value(uint64_t mask, uint64_t universe) { return set_value(mask, universe); }

static bool is_set(const QNet *net, QnCell c) { return net->cells[c].domain == D_SET; }

static void contradict(QNet *net, QnCell c)
{
    net->contradicted = true;
    net->conflict = c;
    clear_queue(net);
}

static void wake(QNet *net, const Cell *cell)
{
    for (size_t i = 0; i < cell->watchers.n; i++) enqueue(net, cell->watchers.ids[i]);
}

QnStatus qn_set_cell(QNet *net, uint32_t kind, unsigned natoms, uint64_t base, QnCell *out)
{
    if (natoms < 1 || natoms > QN_MAXATOMS) return QN_EVALUE;
    uint64_t universe = natoms == 64 ? ~(uint64_t) 0 : ((uint64_t) 1 << natoms) - 1;
    if (!base || (base & ~universe)) return QN_EVALUE;
    if (!grow((void **) &net->cells, &net->capcells, net->ncells + 1, sizeof(Cell))) return QN_ENOMEM;
    Cell *c = &net->cells[net->ncells];
    memset(c, 0, sizeof *c);
    c->base = c->cur = set_value(base, universe);
    c->type = qtype_make(kind, qdim_none(), 0);
    c->domain = D_SET;
    *out = (QnCell) net->ncells++;
    return QN_OK;
}

bool qn_is_set(const QNet *net, QnCell c) { return valid_cell(net, c) && is_set(net, c); }
uint64_t qn_get_set(const QNet *net, QnCell c) { return qn_is_set(net, c) ? word(net->cells[c].cur.lo) : 0; }
uint64_t qn_base_set(const QNet *net, QnCell c) { return qn_is_set(net, c) ? word(net->cells[c].base.lo) : 0; }
uint64_t qn_universe(const QNet *net, QnCell c) { return qn_is_set(net, c) ? word(net->cells[c].cur.hi) : 0; }

QnStatus qn_tell_set(QNet *net, QnCell c, uint64_t allowed)
{
    if (!valid_cell(net, c)) return QN_ECELL;
    if (!is_set(net, c)) return QN_EDOMAIN;
    if (net->contradicted) return QN_CONTRADICTION;
    Cell *cell = &net->cells[c];
    uint64_t now = word(cell->cur.lo), m = now & allowed;
    if (!m) {
        contradict(net, c);
        return QN_CONTRADICTION;
    }
    if (m == now) return QN_OK;
    cell->cur = set_value(m, word(cell->cur.hi));
    wake(net, cell);
    return QN_OK;
}

QnStatus qn_tell(QNet *net, QnCell c, QInterval v)
{
    if (!valid_cell(net, c)) return QN_ECELL;
    if (is_set(net, c)) return QN_EDOMAIN;
    if (!qi_valid(v)) return QN_EVALUE;
    if (net->contradicted) return QN_CONTRADICTION;
    Cell *cell = &net->cells[c];
    QInterval m;
    if (!qi_meet(cell->cur, v, &m)) {
        contradict(net, c);
        return QN_CONTRADICTION;
    }
    if (m.lo == cell->cur.lo && m.hi == cell->cur.hi) return QN_OK;
    cell->cur = m;
    wake(net, cell);
    return QN_OK;
}

bool qn_contradicted(const QNet *net, QnCell *where)
{
    if (net->contradicted && where) *where = net->conflict;
    return net->contradicted;
}

/* --- adding propagators ---------------------------------------------------------- */

static QnStatus watch(QNet *net, QnCell c, uint32_t p)
{
    List *w = &net->cells[c].watchers;
    if (w->n && w->ids[w->n - 1] == p) return QN_OK;   /* a cell named twice */
    if (!grow((void **) &w->ids, &w->cap, w->n + 1, sizeof *w->ids)) return QN_ENOMEM;
    w->ids[w->n++] = p;
    return QN_OK;
}

/* Adds a propagator whose cells are already checked, watches its cells, and
 * puts it on the agenda. */
static QnStatus add_prop(QNet *net, const Prop *p, const QnCell *cells, size_t n)
{
    if (!reserve_prop(net)) return QN_ENOMEM;
    uint32_t id = (uint32_t) net->nprops;
    net->props[net->nprops++] = *p;
    net->props[id].queued = false;
    net->props[id].group = net->current;
    for (size_t i = 0; i < n; i++) {
        QnStatus s = watch(net, cells[i], id);
        if (s) return s;
    }
    if (!net->contradicted) enqueue(net, id);
    return QN_OK;
}

static QnStatus type_error(QStatus q, QStatus *why)
{
    if (why) *why = q;
    return QN_ETYPE;
}

static int8_t scale_of(const QNet *net, QnCell c) { return net->cells[c].type.scale; }

static QConv plan(int k) { return (QConv) { (int16_t) k, Q_ROUND_HALF_EVEN }; }

/* Sum plans: k[i] converts cell i to the working scale w (the finest of the
 * three, so exact unless it overflows), and k[3 + i] converts back. */
QnStatus qn_sum(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why)
{
    if (!valid_cell(net, z) || !valid_cell(net, x) || !valid_cell(net, y)) return QN_ECELL;
    QType t1, t2;
    QStatus q;
    if ((q = qtype_sum(&net->cells[x].type, &net->cells[y].type, &t1))) return type_error(q, why);
    if ((q = qtype_sum(&t1, &net->cells[z].type, &t2))) return type_error(q, why);
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_SUM;
    p.c[0] = z;
    p.c[1] = x;
    p.c[2] = y;
    int w = t2.scale;
    for (int i = 0; i < 3; i++) {
        p.k[i] = plan(scale_of(net, p.c[i]) - w);
        p.k[3 + i] = plan(w - scale_of(net, p.c[i]));
    }
    return add_prop(net, &p, p.c, 3);
}

QnStatus qn_difference(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why)
{
    return qn_sum(net, x, z, y, why);   /* x = z + y */
}

/* Product plans: k[0] for z = x * y, k[1] for x = z / y, k[2] for y = z / x. */
QnStatus qn_product(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why)
{
    if (!valid_cell(net, z) || !valid_cell(net, x) || !valid_cell(net, y)) return QN_ECELL;
    const QType *tz = &net->cells[z].type, *tx = &net->cells[x].type, *ty = &net->cells[y].type;
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_PRODUCT;
    p.c[0] = z;
    p.c[1] = x;
    p.c[2] = y;
    QStatus q;
    if ((q = qtype_mul_plan(tx, ty, tz, &p.k[0]))) return type_error(q, why);
    if ((q = qtype_div_plan(tz, ty, tx, &p.k[1]))) return type_error(q, why);
    if ((q = qtype_div_plan(tz, tx, ty, &p.k[2]))) return type_error(q, why);
    return add_prop(net, &p, p.c, 3);
}

QnStatus qn_quotient(QNet *net, QnCell z, QnCell x, QnCell y, QStatus *why)
{
    return qn_product(net, x, z, y, why);   /* x = z * y */
}

/* Equal plans: k[0] converts b to a, k[1] converts a to b. */
QnStatus qn_equal(QNet *net, QnCell a, QnCell b, QStatus *why)
{
    if (!valid_cell(net, a) || !valid_cell(net, b)) return QN_ECELL;
    QType t;
    QStatus q;
    if ((q = qtype_sum(&net->cells[a].type, &net->cells[b].type, &t))) return type_error(q, why);
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_EQUAL;
    p.c[0] = a;
    p.c[1] = b;
    p.k[0] = plan(scale_of(net, b) - scale_of(net, a));
    p.k[1] = plan(scale_of(net, a) - scale_of(net, b));
    return add_prop(net, &p, p.c, 2);
}

/* Less-or-equal plans: k[0] converts b to a's scale, k[1] converts a to b's. */
QnStatus qn_less_equal(QNet *net, QnCell a, QnCell b, bool strict, QStatus *why)
{
    if (!valid_cell(net, a) || !valid_cell(net, b)) return QN_ECELL;
    QType t;
    QStatus q;
    if ((q = qtype_sum(&net->cells[a].type, &net->cells[b].type, &t))) return type_error(q, why);
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_LE;
    p.c[0] = a;
    p.c[1] = b;
    p.k[0] = plan(scale_of(net, b) - scale_of(net, a));
    p.k[1] = plan(scale_of(net, a) - scale_of(net, b));
    p.strict = strict;
    return add_prop(net, &p, p.c, 2);
}

QnStatus qn_custom(QNet *net, const QnCell *cells, size_t n, QnFn fn, void *ctx)
{
    for (size_t i = 0; i < n; i++)
        if (!valid_cell(net, cells[i])) return QN_ECELL;
    if (!grow((void **) &net->ccells, &net->capccells, net->nccells + n, sizeof *net->ccells)) return QN_ENOMEM;
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_CUSTOM;
    p.fn = fn;
    p.ctx = ctx;
    p.first = net->nccells;
    p.n = n;
    memcpy(net->ccells + net->nccells, cells, n * sizeof *cells);
    net->nccells += n;
    return add_prop(net, &p, cells, n);
}

/* --- running propagators ------------------------------------------------------------ */

/* Each run reads its cells once and tells every cell. If a tell narrows a
 * cell, every watcher of that cell is queued again, this propagator too, so a
 * propagator does not need to be idempotent. */

static void run_sum(QNet *net, const Prop *p)
{
    QInterval v[3];
    for (int i = 0; i < 3; i++) v[i] = qi_conv(net->cells[p->c[i]].cur, &p->k[i]);
    QInterval z = v[0], x = v[1], y = v[2];
    qn_tell(net, p->c[0], qi_conv(qi_add(x, y), &p->k[3]));
    qn_tell(net, p->c[1], qi_conv(qi_sub(z, y), &p->k[4]));
    qn_tell(net, p->c[2], qi_conv(qi_sub(z, x), &p->k[5]));
}

static void run_product(QNet *net, const Prop *p)
{
    QInterval z = net->cells[p->c[0]].cur, x = net->cells[p->c[1]].cur, y = net->cells[p->c[2]].cur;
    qn_tell(net, p->c[0], qi_mul(x, y, &p->k[0]));
    qn_tell(net, p->c[1], qi_div(z, y, &p->k[1]));
    qn_tell(net, p->c[2], qi_div(z, x, &p->k[2]));
}

static void run_equal(QNet *net, const Prop *p)
{
    QInterval a = net->cells[p->c[0]].cur, b = net->cells[p->c[1]].cur;
    qn_tell(net, p->c[0], qi_conv(b, &p->k[0]));
    qn_tell(net, p->c[1], qi_conv(a, &p->k[1]));
}

/* a <= b: a is at most the upper bound of b, and b is at least the lower
 * bound of a. A conversion rounds outward, so no value is lost. For a < b
 * the bound moves by one quantum of the cell that it narrows. */
static void run_le(QNet *net, const Prop *p)
{
    QInterval a = net->cells[p->c[0]].cur, b = net->cells[p->c[1]].cur;
    int64_t upper = qi_conv(b, &p->k[0]).hi, lower = qi_conv(a, &p->k[1]).lo;
    if (p->strict && upper != Q_INF && upper > Q_FMIN) upper--;
    if (p->strict && lower != Q_NINF && lower < Q_FMAX) lower++;
    if (upper != Q_INF) qn_tell(net, p->c[0], (QInterval) { Q_NINF, upper });
    if (lower != Q_NINF) qn_tell(net, p->c[1], (QInterval) { lower, Q_INF });
}

static void run_seteq(QNet *net, const Prop *p)
{
    uint64_t a = qn_get_set(net, p->c[0]), b = qn_get_set(net, p->c[1]);
    qn_tell_set(net, p->c[0], b);
    qn_tell_set(net, p->c[1], a);
}

/* x = table[a] */
static void run_element(QNet *net, const Prop *p)
{
    QnCell x = p->c[0], a = p->c[1];
    const QInterval *table = net->tables + p->first;
    QInterval xv = qn_get(net, x);
    uint64_t mask = qn_get_set(net, a), keep = 0;
    QInterval hull = { 0, 0 };
    bool any = false;
    for (size_t i = 0; i < p->n; i++) {
        if (!(mask >> i & 1)) continue;
        if (!any) hull = table[i];
        else {
            if (table[i].lo < hull.lo) hull.lo = table[i].lo;
            if (table[i].hi > hull.hi) hull.hi = table[i].hi;
        }
        any = true;
        QInterval m;
        if (qi_meet(table[i], xv, &m)) keep |= (uint64_t) 1 << i;
    }
    if (any) qn_tell(net, x, hull);
    qn_tell_set(net, a, keep);   /* keep == 0 is a contradiction */
}

/* b <=> x in range; atom 0 is false and atom 1 is true */
static void run_reify(QNet *net, const Prop *p)
{
    QnCell b = p->c[0], x = p->c[1];
    QInterval r = p->range, xv = qn_get(net, x), m;
    if (qi_subset(xv, r)) qn_tell_set(net, b, 2);
    else if (!qi_meet(xv, r, &m)) qn_tell_set(net, b, 1);
    uint64_t bv = qn_get_set(net, b);
    if (bv == 2) {
        qn_tell(net, x, r);
    } else if (bv == 1) {
        /* x is outside r: narrow x only when the complement of r is one interval */
        if (r.lo == Q_NINF && r.hi == Q_INF) qn_tell_set(net, b, 0);
        else if (r.lo == Q_NINF) {
            if (r.hi >= Q_FMAX) qn_tell_set(net, b, 0);
            else qn_tell(net, x, (QInterval) { r.hi + 1, Q_INF });
        } else if (r.hi == Q_INF) {
            if (r.lo <= Q_FMIN) qn_tell_set(net, b, 0);
            else qn_tell(net, x, (QInterval) { Q_NINF, r.lo - 1 });
        }
    }
}

static void run(QNet *net, const Prop *p)
{
    switch (p->kind) {
    case P_SUM:     run_sum(net, p); break;
    case P_PRODUCT: run_product(net, p); break;
    case P_EQUAL:   run_equal(net, p); break;
    case P_CUSTOM:  p->fn(net, p->ctx, net->ccells + p->first, p->n); break;
    case P_SETEQ:   run_seteq(net, p); break;
    case P_ELEMENT: run_element(net, p); break;
    case P_REIFY:   run_reify(net, p); break;
    case P_LE:      run_le(net, p); break;
    }
}

QnStatus qn_set_equal(QNet *net, QnCell a, QnCell b)
{
    if (!valid_cell(net, a) || !valid_cell(net, b)) return QN_ECELL;
    if (!is_set(net, a) || !is_set(net, b)) return QN_EDOMAIN;
    if (net->cells[a].type.kind != net->cells[b].type.kind || qn_universe(net, a) != qn_universe(net, b))
        return QN_ETYPE;
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_SETEQ;
    p.c[0] = a;
    p.c[1] = b;
    return add_prop(net, &p, p.c, 2);
}

QnStatus qn_element(QNet *net, QnCell x, QnCell a, const QInterval *table, size_t n)
{
    if (!valid_cell(net, x) || !valid_cell(net, a)) return QN_ECELL;
    if (is_set(net, x) || !is_set(net, a)) return QN_EDOMAIN;
    if (n == 0 || n > QN_MAXATOMS || qn_universe(net, a) != (n == 64 ? ~(uint64_t) 0 : ((uint64_t) 1 << n) - 1))
        return QN_EVALUE;
    for (size_t i = 0; i < n; i++)
        if (!qi_valid(table[i])) return QN_EVALUE;
    if (!grow((void **) &net->tables, &net->captables, net->ntables + n, sizeof *net->tables)) return QN_ENOMEM;
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_ELEMENT;
    p.c[0] = x;
    p.c[1] = a;
    p.first = net->ntables;
    p.n = n;
    memcpy(net->tables + net->ntables, table, n * sizeof *table);
    net->ntables += n;
    return add_prop(net, &p, p.c, 2);
}

QnStatus qn_reify_in(QNet *net, QnCell b, QnCell x, QInterval range)
{
    if (!valid_cell(net, b) || !valid_cell(net, x)) return QN_ECELL;
    if (!is_set(net, b) || is_set(net, x)) return QN_EDOMAIN;
    if (qn_universe(net, b) != 3) return QN_ETYPE;
    if (!qi_valid(range)) return QN_EVALUE;
    Prop p;
    memset(&p, 0, sizeof p);
    p.kind = P_REIFY;
    p.c[0] = b;
    p.c[1] = x;
    p.range = range;
    return add_prop(net, &p, p.c, 2);
}

QnStatus qn_propagate(QNet *net)
{
    if (net->contradicted) return QN_CONTRADICTION;
    if (net->propagating) return QN_EPROPAGATING;
    net->propagating = true;
    uint64_t used = 0;
    QnStatus s = QN_OK;
    while (net->qcount) {
        if (net->budget && used == net->budget) {
            s = QN_BUDGET;
            break;
        }
        Prop p = net->props[dequeue(net)];   /* a copy: a custom run can add propagators */
        run(net, &p);
        used++;
        net->runs++;
        if (net->contradicted) {
            s = QN_CONTRADICTION;
            break;
        }
    }
    net->propagating = false;
    return s;
}

void qn_set_budget(QNet *net, uint64_t runs) { net->budget = runs; }
uint64_t qn_runs(const QNet *net) { return net->runs; }

/* --- groups ------------------------------------------------------------------------- */

QnStatus qn_new_group(QNet *net, bool enabled, QnGroup *out)
{
    if (!grow((void **) &net->groups, &net->capgroups, net->ngroups + 1, sizeof *net->groups)) return QN_ENOMEM;
    net->groups[net->ngroups] = enabled;
    *out = (QnGroup) net->ngroups++;
    return QN_OK;
}

void qn_set_group(QNet *net, QnGroup g)
{
    if (g < net->ngroups) net->current = g;
}

QnStatus qn_group_enable(QNet *net, QnGroup g, bool enabled)
{
    if (net->propagating) return QN_EPROPAGATING;
    if (g >= net->ngroups || g == 0) return QN_EGROUP;
    if (net->groups[g] == enabled) return QN_OK;
    net->groups[g] = enabled;
    if (enabled) {
        if (!net->contradicted)
            for (size_t p = 0; p < net->nprops; p++)
                if (net->props[p].group == g) enqueue(net, (uint32_t) p);
    } else {
        /* take the group's propagators off the agenda, keeping the order of the rest */
        size_t n = net->qcount, kept = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t p = net->queue[(net->qhead + i) % net->capprops];
            if (net->props[p].group == g) net->props[p].queued = false;
            else net->queue[(net->qhead + kept++) % net->capprops] = p;
        }
        net->qcount = kept;
    }
    return QN_OK;
}

bool qn_group_enabled(const QNet *net, QnGroup g) { return g < net->ngroups && net->groups[g]; }

/* --- epochs ------------------------------------------------------------------------- */

uint64_t qn_epoch(const QNet *net) { return net->epoch; }

QnStatus qn_stage_set(QNet *net, QnCell c, uint64_t mask)
{
    if (net->propagating) return QN_EPROPAGATING;
    if (!valid_cell(net, c)) return QN_ECELL;
    if (!is_set(net, c)) return QN_EDOMAIN;
    Cell *cell = &net->cells[c];
    uint64_t universe = word(cell->base.hi);
    if (!mask || (mask & ~universe)) return QN_EVALUE;
    if (cell->has_staged) return QN_ESTAGED;
    cell->staged = set_value(mask, universe);
    cell->has_staged = true;
    return QN_OK;
}

QnStatus qn_stage(QNet *net, QnCell c, QInterval v)
{
    if (net->propagating) return QN_EPROPAGATING;
    if (!valid_cell(net, c)) return QN_ECELL;
    if (is_set(net, c)) return QN_EDOMAIN;
    if (!qi_valid(v)) return QN_EVALUE;
    Cell *cell = &net->cells[c];
    if (cell->has_staged) return QN_ESTAGED;
    cell->staged = v;
    cell->has_staged = true;
    return QN_OK;
}

QnStatus qn_begin_epoch(QNet *net)
{
    if (net->propagating) return QN_EPROPAGATING;
    for (size_t i = 0; i < net->ncells; i++) {
        Cell *c = &net->cells[i];
        if (c->has_staged) {
            c->base = c->staged;
            c->has_staged = false;
        }
        c->cur = c->base;
    }
    net->epoch++;
    net->contradicted = false;
    clear_queue(net);
    for (size_t p = 0; p < net->nprops; p++) enqueue(net, (uint32_t) p);
    return QN_OK;
}
