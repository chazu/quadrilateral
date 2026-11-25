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

/* --- logic on truth values (ADR 0022 section 2) ------------------------------ */

static const uint64_t TRUTHS[3] = { F, T, F | T };   /* false, true, unknown */

/* Each operator, for each pair of false, true and unknown. No operator
 * changes an unknown value into false, and no input changes. */
void test_logic_tables(void)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            uint64_t a = TRUTHS[i], b = TRUTHS[j];
            QNet *net = qn_new();
            QnCell ca = set(net, 2, 2, a), cb = set(net, 2, 2, b);
            QnCell both = set(net, 2, 2, F | T), either = set(net, 2, 2, F | T), nota = set(net, 2, 2, F | T);
            CHECK(qn_and(net, both, ca, cb) == QN_OK);
            CHECK(qn_or(net, either, ca, cb) == QN_OK);
            CHECK(qn_not(net, nota, ca) == QN_OK);
            CHECK(qn_propagate(net) == QN_OK);
            CHECK(qn_get_set(net, both) == (a == F || b == F ? F : a == T && b == T ? T : (F | T)));
            CHECK(qn_get_set(net, either) == (a == T || b == T ? T : a == F && b == F ? F : (F | T)));
            CHECK(qn_get_set(net, nota) == (a == F ? T : a == T ? F : (F | T)));
            CHECK(qn_get_set(net, ca) == a && qn_get_set(net, cb) == b);
            qn_free(net);
        }
}

/* One propagator with the given values of r, a and b. *out gets the cells
 * after propagation. */
static QnStatus logic3(int op, uint64_t r, uint64_t a, uint64_t b, uint64_t out[3])
{
    QNet *net = qn_new();
    QnCell cr = set(net, 2, 2, r), ca = set(net, 2, 2, a), cb = set(net, 2, 2, b);
    CHECK((op == 0 ? qn_and(net, cr, ca, cb) : op == 1 ? qn_or(net, cr, ca, cb) : qn_not(net, cr, ca)) == QN_OK);
    QnStatus s = qn_propagate(net);
    out[0] = qn_get_set(net, cr);
    out[1] = qn_get_set(net, ca);
    out[2] = qn_get_set(net, cb);
    qn_free(net);
    return s;
}

/* From the result to the inputs. */
void test_logic_backward(void)
{
    const uint64_t U = F | T;
    uint64_t v[3];
    /* and: a true result makes the two inputs true */
    CHECK(logic3(0, T, U, U, v) == QN_OK && v[1] == T && v[2] == T);
    /* a false result with one true input makes the other input false */
    CHECK(logic3(0, F, T, U, v) == QN_OK && v[1] == T && v[2] == F);
    CHECK(logic3(0, F, U, T, v) == QN_OK && v[1] == F && v[2] == T);
    /* a false result alone says nothing about one input */
    CHECK(logic3(0, F, U, U, v) == QN_OK && v[1] == U && v[2] == U);
    CHECK(logic3(0, T, F, U, v) == QN_CONTRADICTION);
    CHECK(logic3(0, F, T, T, v) == QN_CONTRADICTION);
    /* or: the same with the other truth value */
    CHECK(logic3(1, F, U, U, v) == QN_OK && v[1] == F && v[2] == F);
    CHECK(logic3(1, T, F, U, v) == QN_OK && v[1] == F && v[2] == T);
    CHECK(logic3(1, T, U, F, v) == QN_OK && v[1] == T && v[2] == F);
    CHECK(logic3(1, T, U, U, v) == QN_OK && v[1] == U && v[2] == U);
    CHECK(logic3(1, F, T, U, v) == QN_CONTRADICTION);
    CHECK(logic3(1, T, F, F, v) == QN_CONTRADICTION);
    /* not */
    CHECK(logic3(2, T, U, U, v) == QN_OK && v[1] == F);
    CHECK(logic3(2, F, U, U, v) == QN_OK && v[1] == T);
    CHECK(logic3(2, U, U, U, v) == QN_OK && v[0] == U && v[1] == U);
    CHECK(logic3(2, T, T, U, v) == QN_CONTRADICTION);
    CHECK(logic3(2, F, F, U, v) == QN_CONTRADICTION);

    /* one cell twice: r <=> a && a is r <=> a */
    QNet *net = qn_new();
    QnCell r = set(net, 2, 2, U), a = set(net, 2, 2, U);
    CHECK(qn_and(net, r, a, a) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, r) == U);
    CHECK(qn_tell_set(net, a, T) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_get_set(net, r) == T);
    qn_free(net);
}

/* b <=> a set cell is in some of its atoms. */
void test_reify_set(void)
{
    enum { RED = 1, GREEN = 2, BLUE = 4 };
    static const struct { uint64_t color, b, want_color, want_b; } cases[] = {
        { RED, F | T, RED, T },                          /* each possible atom is in the atoms */
        { RED | GREEN, F | T, RED | GREEN, T },
        { BLUE, F | T, BLUE, F },                        /* no possible atom is in the atoms */
        { RED | BLUE, F | T, RED | BLUE, F | T },        /* unknown */
        { RED | GREEN | BLUE, T, RED | GREEN, T },       /* from the truth value to the set */
        { RED | GREEN | BLUE, F, BLUE, F },
        { GREEN | BLUE, T, GREEN, T },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        QNet *net = qn_new();
        QnCell color = set(net, 1, 3, cases[i].color), b = set(net, 2, 2, cases[i].b);
        CHECK(qn_reify_set(net, b, color, RED | GREEN) == QN_OK);
        CHECK(qn_propagate(net) == QN_OK);
        CHECK(qn_get_set(net, color) == cases[i].want_color);
        CHECK(qn_get_set(net, b) == cases[i].want_b);
        qn_free(net);
    }
    QNet *net = qn_new();
    QnCell color = set(net, 1, 3, 7), b = set(net, 2, 2, T), x = num(net, qi_top()), two = set(net, 3, 2, 3);
    CHECK(qn_reify_set(net, b, set(net, 1, 3, BLUE), RED | GREEN) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);
    /* the atoms are a part of the universe, and not all of it */
    CHECK(qn_reify_set(net, b, color, 0) == QN_EVALUE);
    CHECK(qn_reify_set(net, b, color, 7) == QN_EVALUE);
    CHECK(qn_reify_set(net, b, color, 8) == QN_EVALUE);
    CHECK(qn_reify_set(net, b, two, 3) == QN_EVALUE);
    CHECK(qn_reify_set(net, b, two, 1) == QN_OK);
    CHECK(qn_reify_set(net, color, b, 1) == QN_ETYPE);   /* the first cell is not a boolean */
    CHECK(qn_reify_set(net, b, x, 1) == QN_EDOMAIN);
    CHECK(qn_reify_set(net, x, color, 1) == QN_EDOMAIN);
    CHECK(qn_reify_set(net, b, 999, 1) == QN_ECELL);
    CHECK(qn_reify_set(net, 999, color, 1) == QN_ECELL);
    qn_free(net);
}

void test_logic_errors(void)
{
    QNet *net = qn_new();
    QnCell a = set(net, 2, 2, F | T), b = set(net, 2, 2, F | T), r = set(net, 2, 2, F | T);
    QnCell x = num(net, qi_top()), three = set(net, 1, 3, 7);
    typedef QnStatus (*Two)(QNet *, QnCell, QnCell, QnCell);
    Two ops[2] = { qn_and, qn_or };
    for (int k = 0; k < 2; k++) {
        CHECK(ops[k](net, x, a, b) == QN_EDOMAIN);
        CHECK(ops[k](net, r, x, b) == QN_EDOMAIN);
        CHECK(ops[k](net, r, a, x) == QN_EDOMAIN);
        CHECK(ops[k](net, three, a, b) == QN_ETYPE);
        CHECK(ops[k](net, r, three, b) == QN_ETYPE);
        CHECK(ops[k](net, r, a, three) == QN_ETYPE);
        CHECK(ops[k](net, 999, a, b) == QN_ECELL);
        CHECK(ops[k](net, r, 999, b) == QN_ECELL);
        CHECK(ops[k](net, r, a, 999) == QN_ECELL);
    }
    CHECK(qn_not(net, x, a) == QN_EDOMAIN);
    CHECK(qn_not(net, r, x) == QN_EDOMAIN);
    CHECK(qn_not(net, three, a) == QN_ETYPE);
    CHECK(qn_not(net, r, three) == QN_ETYPE);
    CHECK(qn_not(net, 999, a) == QN_ECELL);
    CHECK(qn_not(net, r, 999) == QN_ECELL);
    /* a call that fails adds no propagator */
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_runs(net) == 0);
    qn_free(net);
}

/* ADR 0022, acceptance test 6: a change of a cell that a condition does not
 * read runs no propagator of that condition. */
void test_logic_incremental(void)
{
    const uint64_t U = F | T;
    /* two conditions with no common cell, and the first one alone */
    QNet *both = qn_new(), *one = qn_new();
    QnCell a1 = set(both, 2, 2, U), b1 = set(both, 2, 2, T), r1 = set(both, 2, 2, U);
    QnCell a2 = set(both, 2, 2, U), b2 = set(both, 2, 2, U), n2 = set(both, 2, 2, U), r2 = set(both, 2, 2, U);
    QnCell x = num(both, iv(0, 100));
    CHECK(qn_and(both, r1, a1, b1) == QN_OK);
    CHECK(qn_not(both, n2, a2) == QN_OK);
    CHECK(qn_or(both, r2, n2, b2) == QN_OK);
    QnCell oa = set(one, 2, 2, U), ob = set(one, 2, 2, T), orr = set(one, 2, 2, U);
    CHECK(qn_and(one, orr, oa, ob) == QN_OK);
    CHECK(qn_propagate(both) == QN_OK && qn_propagate(one) == QN_OK);
    uint64_t before_both = qn_runs(both), before_one = qn_runs(one);

    /* a cell that no condition reads: no run */
    CHECK(qn_tell(both, x, iv(10, 20)) == QN_OK);
    CHECK(qn_propagate(both) == QN_OK);
    CHECK(qn_runs(both) == before_both);

    /* a cell of the first condition: the runs of the first condition alone */
    CHECK(qn_tell_set(both, a1, T) == QN_OK && qn_tell_set(one, oa, T) == QN_OK);
    CHECK(qn_propagate(both) == QN_OK && qn_propagate(one) == QN_OK);
    CHECK(qn_get_set(both, r1) == T && qn_get_set(both, r2) == U);
    CHECK(qn_runs(one) > before_one);
    CHECK(qn_runs(both) - before_both == qn_runs(one) - before_one);
    qn_free(both);
    qn_free(one);
}

typedef struct { int kind, r, a, b; QInterval range; uint64_t atoms; } LSpec;
enum { LB = 5, LI = 2, LS = 1 };   /* boolean cells 0..LB-1, then interval cells, then one set cell with 3 atoms */

static QnStatus lbuild(QNet *net, const LSpec *p)
{
    switch (p->kind) {
    case 0: return qn_and(net, (QnCell) p->r, (QnCell) p->a, (QnCell) p->b);
    case 1: return qn_or(net, (QnCell) p->r, (QnCell) p->a, (QnCell) p->b);
    case 2: return qn_not(net, (QnCell) p->r, (QnCell) p->a);
    case 3: return qn_reify_in(net, (QnCell) p->r, (QnCell) (LB + p->a % LI), p->range);
    default: return qn_reify_set(net, (QnCell) p->r, (QnCell) (LB + LI), p->atoms);
    }
}

/* ADR 0008 with logic: the fixed point does not depend on the order of the
 * propagators. */
void test_prop_logic_order_independence(void)
{
    uint64_t st = 31;
    int settled = 0, contradictions = 0;
    for (int trial = 0; trial < 600; trial++) {
        uint64_t bbase[LB], sbase = (uint64_t) t_range(&st, 1, 7);
        QInterval ibase[LI];
        for (int i = 0; i < LB; i++) bbase[i] = t_rand(&st) % 3 ? 3 : (uint64_t) t_range(&st, 1, 2);
        for (int i = 0; i < LI; i++) ibase[i] = t_rand(&st) % 4 ? rand_iv(&st, 30) : qi_top();
        int np = (int) t_range(&st, 1, 7);
        LSpec specs[7];
        for (int i = 0; i < np; i++) {
            LSpec *p = &specs[i];
            p->kind = (int) (t_rand(&st) % 5);
            p->r = (int) t_range(&st, 0, LB - 1);
            p->a = (int) t_range(&st, 0, LB - 1);
            p->b = (int) t_range(&st, 0, LB - 1);
            p->range = rand_iv(&st, 30);
            if (t_rand(&st) % 2) p->range.hi = Q_INF;
            else p->range.lo = Q_NINF;
            p->atoms = (uint64_t) t_range(&st, 1, 6);
        }
        QNet *nets[2] = { qn_new(), qn_new() };
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < LB; i++) set(nets[k], 2, 2, bbase[i]);
            for (int i = 0; i < LI; i++) num(nets[k], ibase[i]);
            set(nets[k], 1, 3, sbase);
        }
        for (int i = 0; i < np; i++) CHECK(lbuild(nets[0], &specs[i]) == QN_OK);
        for (int i = np - 1; i >= 0; i--) CHECK(lbuild(nets[1], &specs[i]) == QN_OK);
        QnStatus s0 = qn_propagate(nets[0]), s1 = qn_propagate(nets[1]);
        CHECK(s0 == s1);
        if (s0 == QN_OK && s1 == QN_OK) {
            settled++;
            for (QnCell c = 0; c < LB + LI + LS; c++) {
                QInterval a = qn_get(nets[0], c), b = qn_get(nets[1], c);
                CHECK(a.lo == b.lo && a.hi == b.hi);
                CHECK(qn_get_set(nets[0], c) == qn_get_set(nets[1], c));
            }
        } else contradictions++;
        qn_free(nets[0]);
        qn_free(nets[1]);
    }
    CHECK(settled > 200 && contradictions > 20);
}

/* Soundness: the propagators never exclude the values of a real case. */
void test_prop_logic_soundness(void)
{
    uint64_t st = 32;
    for (int trial = 0; trial < 600; trial++) {
        /* the real case: three truth values, a quantity and a color */
        bool p = t_rand(&st) % 2, q = t_rand(&st) % 2, s = t_rand(&st) % 2;
        int64_t x = t_range(&st, -30, 30);
        int color = (int) t_range(&st, 0, 2);
        QInterval range = rand_iv(&st, 30);
        if (t_rand(&st) % 2) range.hi = Q_INF;
        else range.lo = Q_NINF;
        uint64_t atoms = (uint64_t) t_range(&st, 1, 6);
        bool in = qi_contains(range, x), has = atoms >> color & 1;
        bool vals[9] = { p, q, s, in, has, p && q, !s, (p && q) || !s, ((p && q) || !s) && (in || has) };

        QNet *net = qn_new();
        QnCell c[9];
        /* each cell knows its value, or it is unknown */
        for (int i = 0; i < 9; i++) c[i] = set(net, 2, 2, t_rand(&st) % 3 ? 3 : (uint64_t) (vals[i] ? T : F));
        QnCell xc = num(net, t_rand(&st) % 3 ? iv(x - t_range(&st, 0, 10), x + t_range(&st, 0, 10)) : qi_top());
        QnCell cc = set(net, 1, 3, ((uint64_t) 1 << color) | (t_rand(&st) & 7));
        QnCell either = set(net, 2, 2, 3);
        CHECK(qn_reify_in(net, c[3], xc, range) == QN_OK);
        CHECK(qn_reify_set(net, c[4], cc, atoms) == QN_OK);
        CHECK(qn_and(net, c[5], c[0], c[1]) == QN_OK);
        CHECK(qn_not(net, c[6], c[2]) == QN_OK);
        CHECK(qn_or(net, c[7], c[5], c[6]) == QN_OK);
        CHECK(qn_or(net, either, c[3], c[4]) == QN_OK);
        CHECK(qn_and(net, c[8], c[7], either) == QN_OK);
        CHECK(qn_propagate(net) == QN_OK);
        for (int i = 0; i < 9; i++) CHECK(qn_get_set(net, c[i]) & (vals[i] ? T : F));
        CHECK(qn_get_set(net, either) & (in || has ? T : F));
        CHECK(qi_contains(qn_get(net, xc), x));
        CHECK(qn_get_set(net, cc) >> color & 1);
        qn_free(net);
    }
}
