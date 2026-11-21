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
    }
    return "unknown status";
}

/* --- storage ----------------------------------------------------------------- */

typedef enum { P_SUM, P_PRODUCT, P_EQUAL, P_CUSTOM } PKind;

typedef struct {
    PKind  kind;
    bool   queued;
    QnCell c[3];          /* sum, product: z, x, y. equal: a, b. */
    QConv  k[6];          /* plans; see the run functions */
    QnFn   fn;            /* custom */
    void  *ctx;
    size_t first, n;      /* custom: cells in net->ccells[first .. first + n) */
} Prop;

typedef struct { uint32_t *ids; size_t n, cap; } List;

typedef struct {
    QInterval base, cur, staged;
    bool      has_staged;
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

    uint32_t *queue;      /* a ring of propagator ids, capacity capprops */
    size_t    qhead, qcount;

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
    return calloc(1, sizeof(QNet));
}

void qn_free(QNet *net)
{
    if (!net) return;
    for (size_t i = 0; i < net->ncells; i++) free(net->cells[i].watchers.ids);
    free(net->cells);
    free(net->props);
    free(net->ccells);
    free(net->queue);
    free(net);
}

/* --- the agenda ----------------------------------------------------------------- */

static void enqueue(QNet *net, uint32_t p)
{
    if (net->props[p].queued) return;
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

QnStatus qn_tell(QNet *net, QnCell c, QInterval v)
{
    if (!valid_cell(net, c)) return QN_ECELL;
    if (!qi_valid(v)) return QN_EVALUE;
    if (net->contradicted) return QN_CONTRADICTION;
    Cell *cell = &net->cells[c];
    QInterval m;
    if (!qi_meet(cell->cur, v, &m)) {
        net->contradicted = true;
        net->conflict = c;
        clear_queue(net);
        return QN_CONTRADICTION;
    }
    if (m.lo == cell->cur.lo && m.hi == cell->cur.hi) return QN_OK;
    cell->cur = m;
    for (size_t i = 0; i < cell->watchers.n; i++) enqueue(net, cell->watchers.ids[i]);
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

static void run(QNet *net, const Prop *p)
{
    switch (p->kind) {
    case P_SUM:     run_sum(net, p); break;
    case P_PRODUCT: run_product(net, p); break;
    case P_EQUAL:   run_equal(net, p); break;
    case P_CUSTOM:  p->fn(net, p->ctx, net->ccells + p->first, p->n); break;
    }
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

/* --- epochs ------------------------------------------------------------------------- */

uint64_t qn_epoch(const QNet *net) { return net->epoch; }

QnStatus qn_stage(QNet *net, QnCell c, QInterval v)
{
    if (net->propagating) return QN_EPROPAGATING;
    if (!valid_cell(net, c)) return QN_ECELL;
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
