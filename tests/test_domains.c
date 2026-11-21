#include <string.h>

#include "quad/network.h"
#include "test.h"

/* Finite-set domains (ADR 0012). */

static QInterval iv(int64_t lo, int64_t hi) { return (QInterval) { lo, hi }; }

#define CHECK_IV(r, l, h)          \
    do {                           \
        QInterval r_ = (r);        \
        CHECK_I64(r_.lo, (l));     \
        CHECK_I64(r_.hi, (h));     \
    } while (0)

static QnCell num(QNet *net, QInterval base)
{
    QType t = qtype_make(0, qdim_none(), 0);
    QnCell c = 0;
    CHECK(qn_cell(net, &t, base, &c) == QN_OK);
    return c;
}

static QnCell set(QNet *net, uint32_t kind, unsigned natoms, uint64_t base)
{
    QnCell c = 0;
    CHECK(qn_set_cell(net, kind, natoms, base, &c) == QN_OK);
    return c;
}

enum { OPEN = 1, CLOSED = 2 };   /* masks of the door's atoms 0 and 1 */
enum { F = 1, T = 2 };           /* masks of false and true */

void test_set_cells(void)
{
    QNet *net = qn_new();
    QnCell door = set(net, 1, 2, OPEN | CLOSED), x = num(net, iv(0, 10));
    CHECK(qn_is_set(net, door));
    CHECK(!qn_is_set(net, x));
    CHECK(qn_get_set(net, door) == (OPEN | CLOSED));
    CHECK(qn_universe(net, door) == 3);
    CHECK(qn_get_set(net, x) == 0);

    /* the storage is 16 bytes: mask in lo, universe in hi */
    QInterval raw = qn_get(net, door);
    CHECK_I64(raw.lo, 3);
    CHECK_I64(raw.hi, 3);

    CHECK(qn_tell_set(net, door, OPEN) == QN_OK);
    CHECK(qn_get_set(net, door) == OPEN);
    CHECK(qn_tell_set(net, door, CLOSED) == QN_CONTRADICTION);
    QnCell where;
    CHECK(qn_contradicted(net, &where) && where == door);

    /* a new epoch restores the base; a stage replaces it */
    CHECK(qn_stage_set(net, door, CLOSED) == QN_OK);
    CHECK(qn_stage_set(net, door, OPEN) == QN_ESTAGED);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_get_set(net, door) == CLOSED);
    CHECK(qn_base_set(net, door) == CLOSED);

    /* a 64-atom kind */
    QnCell big = set(net, 2, 64, ~(uint64_t) 0);
    CHECK(qn_universe(net, big) == ~(uint64_t) 0);
    CHECK(qn_tell_set(net, big, (uint64_t) 1 << 63) == QN_OK);
    CHECK(qn_get_set(net, big) == (uint64_t) 1 << 63);

    /* errors */
    QnCell bad;
    CHECK(qn_set_cell(net, 1, 0, 1, &bad) == QN_EVALUE);
    CHECK(qn_set_cell(net, 1, 65, 1, &bad) == QN_EVALUE);
    CHECK(qn_set_cell(net, 1, 2, 0, &bad) == QN_EVALUE);   /* empty */
    CHECK(qn_set_cell(net, 1, 2, 4, &bad) == QN_EVALUE);   /* outside the universe */
    CHECK(qn_tell(net, door, iv(0, 1)) == QN_EDOMAIN);
    CHECK(qn_stage(net, door, iv(0, 1)) == QN_EDOMAIN);
    CHECK(qn_tell_set(net, x, 1) == QN_EDOMAIN);
    CHECK(qn_stage_set(net, x, 1) == QN_EDOMAIN);
    CHECK(qn_stage_set(net, big, 0) == QN_EVALUE);
    CHECK(qn_tell_set(net, 999, 1) == QN_ECELL);
    CHECK(!strcmp(qn_status_str(QN_EDOMAIN), "operation does not fit the cell's domain"));
    qn_free(net);
}

void test_set_equal(void)
{
    QNet *net = qn_new();
    QnCell a = set(net, 1, 3, 7), b = set(net, 1, 3, 3), c = set(net, 2, 3, 7), d = set(net, 1, 4, 15);
    CHECK(qn_set_equal(net, a, b) == QN_OK);
    CHECK(qn_set_equal(net, a, c) == QN_ETYPE);   /* another kind */
    CHECK(qn_set_equal(net, a, d) == QN_ETYPE);   /* another universe */
    QnCell x = num(net, qi_top());
    CHECK(qn_set_equal(net, a, x) == QN_EDOMAIN);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, a) == 3);
    CHECK(qn_tell_set(net, b, 2) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, a) == 2);
    qn_free(net);
}

/* A door's state selects a wall's conductance, and the reverse. */
void test_element(void)
{
    QNet *net = qn_new();
    QnCell door = set(net, 1, 2, OPEN | CLOSED), g = num(net, qi_top());
    QInterval table[2] = { qi_point(20000), qi_point(10000) };   /* open: 20 W/K, closed: 10 W/K */
    CHECK(qn_element(net, g, door, table, 2) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, g), 10000, 20000);   /* the hull while the door is unknown */

    CHECK(qn_tell_set(net, door, OPEN) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, g), 20000, 20000);

    /* backward: a measured conductance decides the door */
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_tell(net, g, iv(15000, 25000)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, door) == OPEN);

    /* no atom fits: a contradiction */
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_tell(net, g, iv(0, 5000)) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);

    QnCell three = set(net, 2, 3, 7);
    CHECK(qn_element(net, g, three, table, 2) == QN_EVALUE);   /* the table must cover every atom */
    CHECK(qn_element(net, door, g, table, 2) == QN_EDOMAIN);
    QInterval bad[2] = { iv(5, 1), qi_point(0) };
    CHECK(qn_element(net, g, door, bad, 2) == QN_EVALUE);
    qn_free(net);
}

/* burning <=> temperature >= ignition */
void test_reify(void)
{
    QNet *net = qn_new();
    QnCell burning = set(net, 1, 2, F | T), temp = num(net, iv(600, 700));
    QInterval hot = iv(500, Q_INF);
    CHECK(qn_reify_in(net, burning, temp, hot) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, burning) == T);

    CHECK(qn_stage(net, temp, iv(100, 200)) == QN_OK);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, burning) == F);

    /* from the boolean to the quantity */
    CHECK(qn_stage(net, temp, qi_top()) == QN_OK);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_tell_set(net, burning, F) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, temp), Q_NINF, 499);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_tell_set(net, burning, T) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, temp), 500, Q_INF);

    /* the complement of a lower half-line */
    QnCell cold = set(net, 1, 2, F), u = num(net, qi_top());
    CHECK(qn_reify_in(net, cold, u, iv(Q_NINF, 100)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, u), 101, Q_INF);

    /* an interval with two finite bounds has no one-interval complement */
    QnCell inside = set(net, 1, 2, F), y = num(net, iv(0, 100));
    CHECK(qn_reify_in(net, inside, y, iv(40, 60)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, y), 0, 100);

    /* "x is outside everything" is a contradiction */
    QnCell never = set(net, 1, 2, F), z = num(net, iv(0, 1));
    CHECK(qn_reify_in(net, never, z, qi_top()) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);

    QNet *n2 = qn_new();
    QnCell three = set(n2, 1, 3, 7), w = num(n2, qi_top());
    CHECK(qn_reify_in(n2, three, w, hot) == QN_ETYPE);   /* not a boolean */
    CHECK(qn_reify_in(n2, w, w, hot) == QN_EDOMAIN);
    qn_free(n2);
    qn_free(net);
}

/* --- properties ------------------------------------------------------------- */

typedef struct { int kind, a, b; QInterval table[4]; QInterval range; } Spec;
enum { NI = 4, NS = 3, NATOMS = 4 };   /* interval cells 0..NI-1, set cells NI..NI+NS-1 */

static QnStatus build(QNet *net, const Spec *p)
{
    switch (p->kind) {
    case 0: return qn_sum(net, (QnCell) p->a, (QnCell) p->b, (QnCell) ((p->a + 1) % NI), NULL);
    case 1: return qn_element(net, (QnCell) p->a, (QnCell) (NI + p->b), p->table, NATOMS);
    case 2: return qn_reify_in(net, (QnCell) (NI + NS), (QnCell) p->a, p->range);
    default: return qn_set_equal(net, (QnCell) (NI + p->a % NS), (QnCell) (NI + p->b));
    }
}

static QInterval rand_iv(uint64_t *st, int64_t lim)
{
    int64_t a = t_range(st, -lim, lim), b = t_range(st, -lim, lim);
    return iv(a < b ? a : b, a < b ? b : a);
}

/* ADR 0008 with mixed domains: the fixed point does not depend on order. */
void test_prop_domains_order_independence(void)
{
    uint64_t st = 21;
    int compared = 0;
    for (int trial = 0; trial < 400; trial++) {
        QInterval ibase[NI];
        uint64_t sbase[NS + 1];
        for (int i = 0; i < NI; i++) ibase[i] = t_rand(&st) % 5 ? rand_iv(&st, 40) : qi_top();
        for (int i = 0; i < NS; i++) sbase[i] = t_range(&st, 1, 15);
        sbase[NS] = t_range(&st, 1, 3);   /* the boolean */
        int np = (int) t_range(&st, 1, 8);
        Spec specs[8];
        for (int i = 0; i < np; i++) {
            Spec *p = &specs[i];
            p->kind = (int) (t_rand(&st) % 4);
            p->a = (int) t_range(&st, 0, NI - 1);
            p->b = (int) t_range(&st, 0, NS - 1);
            for (int j = 0; j < NATOMS; j++) p->table[j] = rand_iv(&st, 40);
            p->range = rand_iv(&st, 40);
            if (t_rand(&st) % 3 == 0) p->range.hi = Q_INF;
            else if (t_rand(&st) % 3 == 0) p->range.lo = Q_NINF;
        }
        QNet *fw = qn_new(), *bw = qn_new();
        QNet *nets[2] = { fw, bw };
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < NI; i++) num(nets[k], ibase[i]);
            for (int i = 0; i < NS; i++) set(nets[k], 1, NATOMS, sbase[i]);
            set(nets[k], 2, 2, sbase[NS]);
            qn_set_budget(nets[k], 100000);
        }
        for (int i = 0; i < np; i++) build(fw, &specs[i]);
        for (int i = np - 1; i >= 0; i--) build(bw, &specs[i]);
        QnStatus sf = qn_propagate(fw), sb = qn_propagate(bw);
        if (sf != QN_BUDGET && sb != QN_BUDGET) {
            compared++;
            CHECK(sf == sb);
            if (sf == QN_OK && sb == QN_OK)
                for (QnCell c = 0; c < NI + NS + 1; c++) {
                    QInterval a = qn_get(fw, c), b = qn_get(bw, c);
                    CHECK(a.lo == b.lo && a.hi == b.hi);
                }
        }
        qn_free(fw);
        qn_free(bw);
    }
    CHECK(compared > 300);
}

/* Soundness: a known solution is never excluded. */
void test_prop_domains_soundness(void)
{
    uint64_t st = 22;
    for (int trial = 0; trial < 500; trial++) {
        QNet *net = qn_new();
        int atom = (int) t_range(&st, 0, NATOMS - 1);
        QInterval table[NATOMS];
        for (int j = 0; j < NATOMS; j++) table[j] = rand_iv(&st, 50);
        int64_t x = t_range(&st, table[atom].lo, table[atom].hi);
        QInterval range = rand_iv(&st, 50);
        if (t_rand(&st) % 2) range.hi = Q_INF;
        else range.lo = Q_NINF;
        bool in = qi_contains(range, x);

        /* cells: a (set), xc (interval), b (boolean), a2 (set equal to a) */
        uint64_t amask = ((uint64_t) 1 << atom) | (t_rand(&st) & 15);
        QnCell a = set(net, 1, NATOMS, amask);
        QnCell xc = num(net, t_rand(&st) % 3 ? iv(x - t_range(&st, 0, 20), x + t_range(&st, 0, 20)) : qi_top());
        QnCell b = set(net, 2, 2, t_rand(&st) % 3 ? (uint64_t) (in ? T : F) : (F | T));
        QnCell a2 = set(net, 1, NATOMS, ((uint64_t) 1 << atom) | (t_rand(&st) & 15));
        CHECK(qn_element(net, xc, a, table, NATOMS) == QN_OK);
        CHECK(qn_reify_in(net, b, xc, range) == QN_OK);
        CHECK(qn_set_equal(net, a, a2) == QN_OK);
        CHECK(qn_propagate(net) == QN_OK);
        CHECK(qn_get_set(net, a) >> atom & 1);
        CHECK(qn_get_set(net, a2) >> atom & 1);
        CHECK(qi_contains(qn_get(net, xc), x));
        CHECK(qn_get_set(net, b) & (in ? T : F));
        qn_free(net);
    }
}
