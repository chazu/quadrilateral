#include <stdlib.h>
#include <string.h>

#include "quad/network.h"
#include "test.h"

static QInterval iv(int64_t lo, int64_t hi) { return (QInterval) { lo, hi }; }

#define CHECK_IV(r, l, h)          \
    do {                           \
        QInterval r_ = (r);        \
        CHECK_I64(r_.lo, (l));     \
        CHECK_I64(r_.hi, (h));     \
    } while (0)

static QDim dim4(int l, int m, int t, int k)
{
    QDim d = qdim_none();
    d.exp[Q_BASE_LENGTH] = (int8_t) l;
    d.exp[Q_BASE_MASS] = (int8_t) m;
    d.exp[Q_BASE_TIME] = (int8_t) t;
    d.exp[Q_BASE_TEMPERATURE] = (int8_t) k;
    return d;
}

static QnCell cell(QNet *net, uint32_t kind, QDim dim, int scale, QInterval base)
{
    QType t = qtype_make(kind, dim, (int8_t) scale);
    QnCell c = 0;
    CHECK(qn_cell(net, &t, base, &c) == QN_OK);
    return c;
}

static QnCell num(QNet *net, int scale, QInterval base) { return cell(net, 0, qdim_none(), scale, base); }

/* Design section 8.1: distance = speed × time. */
void test_net_distance(void)
{
    QNet *net = qn_new();
    QnCell d = cell(net, 1, dim4(1, 0, 0, 0), -4, iv(100 * 10000, 200 * 10000));
    QnCell s = cell(net, 2, dim4(1, 0, -1, 0), -3, iv(10 * 1000, 20 * 1000));
    QnCell t = cell(net, 3, dim4(0, 0, 1, 0), -6, qi_top());
    CHECK(qn_product(net, d, s, t, NULL) == QN_OK);

    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, t), 5000000, 20000000);

    CHECK(qn_tell(net, d, qi_point(150 * 10000)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, t), 7500000, 15000000);

    CHECK(qn_tell(net, t, qi_point(10 * 1000000)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, s), 15000, 15000);
    qn_free(net);
}

/* Design sections 9.2 and 9.4: P = G × (Th − Tc), forward and backward. */
void test_net_wall(void)
{
    QNet *net = qn_new();
    QDim temp = dim4(0, 0, 0, 1);
    QnCell th = cell(net, 1, temp, -2, iv(30115, 30515));   /* 28..32 °C */
    QnCell tc = cell(net, 1, temp, -2, iv(29215, 29415));   /* 19..21 °C */
    QnCell dt = cell(net, 1, temp, -2, qi_top());
    QnCell g = cell(net, 2, dim4(2, 1, -3, -1), -3, qi_point(10000));   /* 10 W/K */
    QnCell p = cell(net, 3, dim4(2, 1, -3, 0), -3, qi_top());
    CHECK(qn_difference(net, dt, th, tc, NULL) == QN_OK);
    CHECK(qn_product(net, p, g, dt, NULL) == QN_OK);

    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, dt), 700, 1300);
    CHECK_IV(qn_get(net, p), 70000, 130000);   /* 70..130 W */

    /* a later observation narrows the hot room, and the transfer narrows too */
    CHECK(qn_tell(net, th, iv(30215, 30315)) == QN_OK);   /* 29..30 °C */
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, p), 80000, 110000);

    /* next epoch: the cold room is exactly 20 °C, the hot room unknown, and
     * the transfer is observed at 100 W; the hot room is then 30 °C */
    CHECK(qn_stage(net, tc, qi_point(29315)) == QN_OK);
    CHECK(qn_stage(net, th, qi_top()) == QN_OK);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_tell(net, p, qi_point(100000)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, dt), 1000, 1000);
    CHECK_IV(qn_get(net, th), 30315, 30315);
    qn_free(net);
}

/* Design section 8.1: X + Y = 10 with both in 0..10 has many solutions, and
 * propagation does not narrow it. */
void test_net_many_solutions(void)
{
    QNet *net = qn_new();
    QnCell x = num(net, 0, iv(0, 10)), y = num(net, 0, iv(0, 10)), ten = num(net, 0, qi_point(10));
    CHECK(qn_sum(net, ten, x, y, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, x), 0, 10);
    CHECK_IV(qn_get(net, y), 0, 10);
    qn_free(net);
}

void test_net_contradiction(void)
{
    QNet *net = qn_new();
    QnCell x = num(net, 0, iv(0, 10)), y = num(net, 0, iv(0, 10)), z = num(net, 0, iv(25, 30));
    CHECK(qn_sum(net, z, x, y, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);
    QnCell where = 99;
    CHECK(qn_contradicted(net, &where));
    CHECK(where == x || where == y || where == z);
    CHECK(qn_tell(net, x, iv(1, 2)) == QN_CONTRADICTION);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);

    CHECK(qn_stage(net, z, iv(5, 6)) == QN_OK);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(!qn_contradicted(net, NULL));
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, z), 5, 6);
    CHECK_IV(qn_get(net, x), 0, 6);
    qn_free(net);
}

/* x = y + 1 and y = x − 2 contradict each other, and interval propagation
 * finds that one quantum at a time. */
static QNet *slow_net(QnCell *x, QnCell *y)
{
    QNet *net = qn_new();
    *x = num(net, 0, iv(-10000, 10000));
    *y = num(net, 0, iv(-10000, 10000));
    QnCell one = num(net, 0, qi_point(1)), two = num(net, 0, qi_point(2));
    CHECK(qn_sum(net, *x, *y, one, NULL) == QN_OK);
    CHECK(qn_difference(net, *y, *x, two, NULL) == QN_OK);
    return net;
}

void test_net_budget(void)
{
    QnCell x1, y1, x2, y2;
    QNet *a = slow_net(&x1, &y1), *b = slow_net(&x2, &y2);
    qn_set_budget(a, 100);
    qn_set_budget(b, 100);
    CHECK(qn_propagate(a) == QN_BUDGET);
    CHECK(qn_propagate(b) == QN_BUDGET);
    CHECK_I64(qn_runs(a), 100);
    /* the same network and budget give the same result */
    CHECK(!memcmp(&(QInterval[2]) { qn_get(a, x1), qn_get(a, y1) }, &(QInterval[2]) { qn_get(b, x2), qn_get(b, y2) },
                  2 * sizeof(QInterval)));
    CHECK(qn_get(a, x1).hi < 10000);   /* it narrowed */

    /* propagation continues where it stopped, and finds the contradiction */
    qn_set_budget(a, 0);
    CHECK(qn_propagate(a) == QN_CONTRADICTION);
    qn_free(a);
    qn_free(b);
}

static QnStatus inner_status[3];

static void misbehave(QNet *net, void *ctx, const QnCell *cells, size_t n)
{
    (void) ctx;
    (void) n;
    inner_status[0] = qn_stage(net, cells[0], qi_point(1));
    inner_status[1] = qn_begin_epoch(net);
    inner_status[2] = qn_propagate(net);
}

void test_net_epochs(void)
{
    QNet *net = qn_new();
    QnCell a = num(net, 0, iv(0, 100)), b = num(net, 0, qi_top());
    CHECK(qn_equal(net, a, b, NULL) == QN_OK);
    CHECK_I64(qn_epoch(net), 0);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 0, 100);

    /* a tell is knowledge for this epoch only */
    CHECK(qn_tell(net, a, iv(10, 20)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 10, 20);
    CHECK_IV(qn_base(net, a), 0, 100);
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK_I64(qn_epoch(net), 1);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 0, 100);

    /* a staged value is committed at the boundary; one writer per cell */
    CHECK(qn_stage(net, a, qi_point(42)) == QN_OK);
    CHECK(qn_stage(net, a, qi_point(43)) == QN_ESTAGED);
    CHECK_IV(qn_get(net, a), 0, 100);   /* not visible before the boundary */
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 42, 42);
    CHECK(qn_stage(net, a, qi_point(44)) == QN_OK);   /* a new boundary, a new stage */

    /* non-monotonic calls are refused during propagation */
    QnCell cs[1] = { b };
    CHECK(qn_custom(net, cs, 1, misbehave, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(inner_status[0] == QN_EPROPAGATING);
    CHECK(inner_status[1] == QN_EPROPAGATING);
    CHECK(inner_status[2] == QN_EPROPAGATING);
    CHECK_I64(qn_epoch(net), 2);

    CHECK(qn_stage(net, 99, qi_point(1)) == QN_ECELL);
    CHECK(qn_stage(net, b, iv(3, 1)) == QN_EVALUE);
    CHECK(qn_tell(net, b, iv(3, 1)) == QN_EVALUE);
    qn_free(net);
}

/* A custom propagator: a <= b. */
static void at_most(QNet *net, void *ctx, const QnCell *c, size_t n)
{
    (void) ctx;
    (void) n;
    QInterval a = qn_get(net, c[0]), b = qn_get(net, c[1]);
    qn_tell(net, c[0], iv(Q_NINF, b.hi));
    qn_tell(net, c[1], iv(a.lo, Q_INF));
}

void test_net_custom(void)
{
    QNet *net = qn_new();
    QnCell a = num(net, 0, iv(0, 100)), b = num(net, 0, iv(-50, 40)), c = num(net, 0, qi_top());
    QnCell ab[2] = { a, b };
    CHECK(qn_custom(net, ab, 2, at_most, NULL) == QN_OK);
    CHECK(qn_equal(net, b, c, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, a), 0, 40);
    CHECK_IV(qn_get(net, b), 0, 40);
    CHECK_IV(qn_get(net, c), 0, 40);
    /* narrowing c reaches a through b */
    CHECK(qn_tell(net, c, iv(0, 5)) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, a), 0, 5);
    qn_free(net);
}

void test_net_types(void)
{
    QNet *net = qn_new();
    QnCell len = cell(net, 1, dim4(1, 0, 0, 0), -4, qi_top());
    QnCell mass = cell(net, 2, dim4(0, 1, 0, 0), -3, qi_top());
    QnCell len2 = cell(net, 3, dim4(1, 0, 0, 0), -4, qi_top());
    QStatus why = Q_OK;
    CHECK(qn_sum(net, len, len, mass, &why) == QN_ETYPE);
    CHECK(why == Q_EDIM);
    CHECK(qn_product(net, len, len, mass, &why) == QN_ETYPE);
    CHECK(why == Q_EDIM);
    CHECK(qn_equal(net, len, len2, &why) == QN_ETYPE);
    CHECK(why == Q_EKIND);
    CHECK(qn_sum(net, len, len, 99, NULL) == QN_ECELL);

    QType wide = qtype_make(1, qdim_none(), 0);
    wide.width = 128;
    QnCell out;
    CHECK(qn_cell(net, &wide, qi_top(), &out) == QN_ETYPE);
    QType ok = qtype_make(1, qdim_none(), 0);
    CHECK(qn_cell(net, &ok, iv(2, 1), &out) == QN_EVALUE);
    CHECK_I64((int64_t) qn_ncells(net), 3);
    for (int s = QN_OK; s <= QN_ENOMEM; s++) CHECK(qn_status_str((QnStatus) s) != NULL);
    qn_free(net);
}

void test_net_mixed_scales(void)
{
    QNet *net = qn_new();
    QnCell x = num(net, -3, qi_point(1234));   /* 1.234 */
    QnCell y = num(net, -2, qi_point(56));     /* 0.56 */
    QnCell s = num(net, -4, qi_top());
    QnCell p = num(net, -4, qi_top());
    CHECK(qn_sum(net, s, x, y, NULL) == QN_OK);
    CHECK(qn_product(net, p, x, y, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, s), 17940, 17940);   /* 1.794 */
    CHECK_IV(qn_get(net, p), 6910, 6911);     /* 0.69104 lies inside */

    /* backward through the sum: y = s − x, exactly */
    QNet *n2 = qn_new();
    QnCell x2 = num(n2, -3, qi_point(1234)), y2 = num(n2, -2, qi_top()), s2 = num(n2, -4, qi_point(17940));
    CHECK(qn_sum(n2, s2, x2, y2, NULL) == QN_OK);
    CHECK(qn_propagate(n2) == QN_OK);
    CHECK_IV(qn_get(n2, y2), 56, 56);
    qn_free(net);
    qn_free(n2);
}

/* --- properties ------------------------------------------------------------- */

typedef struct { int kind, a, b, c; } Spec;   /* kind: 0 sum, 1 product, 2 equal, 3 difference */

static QnStatus build(QNet *net, const Spec *p)
{
    switch (p->kind) {
    case 0: return qn_sum(net, (QnCell) p->a, (QnCell) p->b, (QnCell) p->c, NULL);
    case 1: return qn_product(net, (QnCell) p->a, (QnCell) p->b, (QnCell) p->c, NULL);
    case 2: return qn_equal(net, (QnCell) p->a, (QnCell) p->b, NULL);
    default: return qn_difference(net, (QnCell) p->a, (QnCell) p->b, (QnCell) p->c, NULL);
    }
}

/* ADR 0008: propagation to a fixed point gives the same result in every
 * order. The same random network is built with its propagators in two
 * orders. */
void test_prop_net_order_independence(void)
{
    uint64_t st = 11;
    int compared = 0;
    enum { TRIALS = 400, MAXC = 8, MAXP = 10 };
    for (int trial = 0; trial < TRIALS; trial++) {
        int nc = (int) t_range(&st, 3, MAXC), np = (int) t_range(&st, 1, MAXP);
        int scales[MAXC];
        QInterval bases[MAXC];
        for (int i = 0; i < nc; i++) {
            scales[i] = (int) t_range(&st, -2, 0);
            int64_t a = t_range(&st, -60, 60), b = t_range(&st, -60, 60);
            bases[i] = iv(a < b ? a : b, a < b ? b : a);
            if (t_rand(&st) % 5 == 0) bases[i] = qi_top();
        }
        Spec specs[MAXP];
        for (int i = 0; i < np; i++)
            specs[i] = (Spec) { (int) (t_rand(&st) % 4), (int) t_range(&st, 0, nc - 1), (int) t_range(&st, 0, nc - 1),
                                (int) t_range(&st, 0, nc - 1) };

        QNet *fw = qn_new(), *bw = qn_new();
        for (int i = 0; i < nc; i++) {
            num(fw, scales[i], bases[i]);
            num(bw, scales[i], bases[i]);
        }
        for (int i = 0; i < np; i++) build(fw, &specs[i]);
        for (int i = np - 1; i >= 0; i--) build(bw, &specs[i]);
        qn_set_budget(fw, 100000);
        qn_set_budget(bw, 100000);
        QnStatus sf = qn_propagate(fw), sb = qn_propagate(bw);
        if (sf != QN_BUDGET && sb != QN_BUDGET) {
            compared++;
            CHECK(sf == sb);
            if (sf == QN_OK && sb == QN_OK)
                for (int i = 0; i < nc; i++) {
                    QInterval a = qn_get(fw, (QnCell) i), b = qn_get(bw, (QnCell) i);
                    CHECK(a.lo == b.lo && a.hi == b.hi);
                }
        }
        qn_free(fw);
        qn_free(bw);
    }
    CHECK(compared > TRIALS * 3 / 4);
}

/* Soundness: build a random network around a known exact solution. Every
 * cell must still contain its exact value after propagation, and the
 * network must not report a contradiction. */
void test_prop_net_soundness(void)
{
    uint64_t st = 12;
    enum { TRIALS = 500, MAXC = 14 };
    for (int trial = 0; trial < TRIALS; trial++) {
        QNet *net = qn_new();
        int64_t exact[MAXC];
        int nc = 0;
        int leaves = (int) t_range(&st, 2, 4);
        for (; nc < leaves; nc++) exact[nc] = t_range(&st, -20, 20);
        typedef struct { int kind, z, x, y; } Rel;
        Rel rels[MAXC];
        int nr = 0;
        while (nc < MAXC) {
            int x = (int) t_range(&st, 0, nc - 1), y = (int) t_range(&st, 0, nc - 1);
            int kind = (int) (t_rand(&st) % 4);
            int64_t v;
            if (kind == 0) v = exact[x] + exact[y];
            else if (kind == 1) v = exact[x] * exact[y];
            else if (kind == 2) v = exact[x] - exact[y];
            else {
                if (exact[y] == 0 || exact[x] % exact[y]) continue;
                v = exact[x] / exact[y];
            }
            if (v > 1000000 || v < -1000000) continue;
            exact[nc] = v;
            rels[nr++] = (Rel) { kind, nc, x, y };
            nc++;
        }
        for (int i = 0; i < nc; i++) {
            QInterval base;
            switch (t_rand(&st) % 3) {
            case 0: base = qi_top(); break;
            case 1: base = qi_point(exact[i]); break;
            default: base = iv(exact[i] - t_range(&st, 0, 30), exact[i] + t_range(&st, 0, 30)); break;
            }
            num(net, 0, base);
        }
        for (int i = 0; i < nr; i++) {
            Rel r = rels[i];
            QnStatus s = r.kind == 0   ? qn_sum(net, (QnCell) r.z, (QnCell) r.x, (QnCell) r.y, NULL)
                         : r.kind == 1 ? qn_product(net, (QnCell) r.z, (QnCell) r.x, (QnCell) r.y, NULL)
                         : r.kind == 2 ? qn_difference(net, (QnCell) r.z, (QnCell) r.x, (QnCell) r.y, NULL)
                                       : qn_quotient(net, (QnCell) r.z, (QnCell) r.x, (QnCell) r.y, NULL);
            CHECK(s == QN_OK);
        }
        qn_set_budget(net, 50000);
        QnStatus s = qn_propagate(net);
        CHECK(s != QN_CONTRADICTION);
        for (int i = 0; i < nc; i++) CHECK(qi_contains(qn_get(net, (QnCell) i), exact[i]));
        qn_free(net);
    }
}

static QnStatus group_inner;
static QnGroup group_id;

static void toggles_group(QNet *net, void *ctx, const QnCell *cells, size_t n)
{
    (void) ctx;
    (void) cells;
    (void) n;
    group_inner = qn_group_enable(net, group_id, false);
}

void test_net_groups(void)
{
    QNet *net = qn_new();
    QnCell a = num(net, 0, iv(0, 10)), b = num(net, 0, qi_top());
    QnGroup g;
    CHECK(qn_new_group(net, false, &g) == QN_OK);
    CHECK(!qn_group_enabled(net, g));
    qn_set_group(net, g);
    CHECK(qn_equal(net, a, b, NULL) == QN_OK);
    qn_set_group(net, 0);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), Q_NINF, Q_INF);   /* disabled: never ran */
    CHECK(qn_begin_epoch(net) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), Q_NINF, Q_INF);

    CHECK(qn_group_enable(net, g, true) == QN_OK);   /* enabling puts it on the agenda */
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 0, 10);
    CHECK(qn_group_enable(net, 0, false) == QN_EGROUP);
    CHECK(qn_group_enable(net, 99, true) == QN_EGROUP);

    /* disabling takes queued propagators off the agenda */
    CHECK(qn_tell(net, a, iv(2, 3)) == QN_OK);
    CHECK(qn_group_enable(net, g, false) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, b), 0, 10);

    /* not during propagation */
    group_id = g;
    QnCell cs[1] = { a };
    CHECK(qn_custom(net, cs, 1, toggles_group, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(group_inner == QN_EPROPAGATING);
    qn_free(net);
}

/* a <= b and a < b (ADR 0018): the upper bound of a and the lower bound of b */
void test_net_less_equal(void)
{
    QNet *net = qn_new();
    QDim len = dim4(1, 0, 0, 0);
    QnCell a = cell(net, 1, len, -3, iv(2000, 9000)), b = cell(net, 1, len, -3, iv(1000, 5000));
    CHECK(qn_less_equal(net, a, b, false, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, a), 2000, 5000);
    CHECK_IV(qn_get(net, b), 2000, 5000);

    /* strict: one quantum of the cell that the propagator narrows */
    QnCell c = cell(net, 1, len, -3, iv(2000, 9000)), d = cell(net, 1, len, -3, iv(1000, 5000));
    CHECK(qn_less_equal(net, c, d, true, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, c), 2000, 4999);
    CHECK_IV(qn_get(net, d), 2001, 5000);

    /* two scales: metres and millimetres. A bound converts outward, so no value is lost. */
    QnCell m = cell(net, 1, len, 0, qi_top()), mm = cell(net, 1, len, -3, iv(1500, 2500));
    CHECK(qn_less_equal(net, m, mm, false, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, m), Q_NINF, 3);                 /* at most 2.5 m: 3 m in whole metres, outward */
    QnCell lo = cell(net, 1, len, 0, iv(2, 2));
    CHECK(qn_less_equal(net, lo, mm, true, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, mm), 2001, 2500);               /* more than 2 m */

    /* an infinite bound says nothing */
    QnCell x = cell(net, 1, len, -3, qi_top()), y = cell(net, 1, len, -3, qi_top());
    CHECK(qn_less_equal(net, x, y, true, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK_IV(qn_get(net, x), Q_NINF, Q_INF);
    CHECK_IV(qn_get(net, y), Q_NINF, Q_INF);

    /* kinds and dimensions must fit, as for an equality */
    QStatus why = Q_OK;
    QnCell t = cell(net, 2, dim4(0, 0, 0, 1), -3, qi_top()), other = cell(net, 3, len, -3, qi_top());
    CHECK(qn_less_equal(net, a, t, false, &why) == QN_ETYPE && why == Q_EDIM);
    CHECK(qn_less_equal(net, a, other, false, &why) == QN_ETYPE && why == Q_EKIND);
    CHECK(qn_less_equal(net, a, 9999, false, NULL) == QN_ECELL);

    /* a contradiction: 6 <= 5 */
    QnCell six = cell(net, 1, len, -3, iv(6000, 6000)), five = cell(net, 1, len, -3, iv(5000, 5000));
    CHECK(qn_less_equal(net, six, five, false, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);
    qn_free(net);

    /* 5 < 5 is a contradiction, and 5 <= 5 is not */
    net = qn_new();
    QnCell p = cell(net, 1, len, -3, iv(5000, 5000)), q = cell(net, 1, len, -3, iv(5000, 5000));
    CHECK(qn_less_equal(net, p, q, false, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_OK);
    CHECK(qn_less_equal(net, p, q, true, NULL) == QN_OK);
    CHECK(qn_propagate(net) == QN_CONTRADICTION);
    qn_free(net);
}
