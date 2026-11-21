#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL

/* The phase 5 experiment: roomA is one aggregate room. roomB has two models:
 * "agg", one stock, and "fine", n cells in a row with internal diffusion. The
 * wall joins roomA to roomB's port "west", which is the whole room in "agg"
 * and cell c0 in "fine". Both rooms have 10000 J/K; roomA starts at 30 °C and
 * roomB at 20 °C. */
typedef struct {
    QWorld   *w;
    QwPhen    a, b, wall;
    QwKind    energy, temp, cap, cond, ratio;
    QkKernel *diff, *refine, *abstract;
    unsigned  n;
} Mixed;

static void room_cells(Mixed *m, const char *prefix, int64_t cap_mJK, int64_t energy_mJ)
{
    char path[128];
    QnCell e, c, t;
    snprintf(path, sizeof path, "%s.energy", prefix);
    OK(qw_cell(m->w, path, m->energy, qi_point(energy_mJ), &e));
    snprintf(path, sizeof path, "%s.capacity", prefix);
    OK(qw_cell(m->w, path, m->cap, qi_point(cap_mJK), &c));
    snprintf(path, sizeof path, "%s.temperature", prefix);
    OK(qw_cell(m->w, path, m->temp, qi_top(), &t));
    CHECK(qn_product(qw_net(m->w), e, c, t, NULL) == QN_OK);
}

static Mixed make_mixed(uint64_t seed, unsigned n, int64_t g_int_WK, int64_t fine_tol_e9, int64_t spread_uK)
{
    Mixed m;
    memset(&m, 0, sizeof m);
    m.n = n;
    m.w = qw_new(seed, -6);
    OK(qw_kind(m.w, "energy", "J", -3, true, &m.energy));
    OK(qw_kind(m.w, "temperature", "K", -6, false, &m.temp));
    OK(qw_kind(m.w, "capacity", "J/K", -3, false, &m.cap));
    OK(qw_kind(m.w, "conductance", "W/K", -3, false, &m.cond));
    OK(qw_kind(m.w, "ratio", "", -9, false, &m.ratio));
    OK(qw_phenomenon(m.w, "roomA", &m.a));
    OK(qw_phenomenon(m.w, "roomB", &m.b));
    OK(qw_phenomenon(m.w, "wall", &m.wall));

    const int64_t cap = 10000 * 1000;   /* 10000 J/K in mJ/K */
    room_cells(&m, "roomA", cap, cap / 100 * 30315);   /* 303.15 K */

    OK(qw_model(m.w, m.b, "agg"));
    OK(qw_model(m.w, m.b, "fine"));
    OK(qw_model_begin(m.w, m.b, "agg"));
    room_cells(&m, "roomB.agg", cap, cap / 100 * 29315);   /* 293.15 K */
    qw_model_end(m.w);
    OK(qw_model_begin(m.w, m.b, "fine"));
    for (unsigned i = 0; i < n; i++) {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "roomB.fine.c%u", i);
        room_cells(&m, prefix, cap / n, 0);
    }
    qw_model_end(m.w);
    OK(qw_cell(m.w, "roomB.fine.conductance", m.cond, qi_point(g_int_WK * 1000), NULL));
    OK(qw_cell(m.w, "roomB.fine.tolerance", m.ratio, qi_point(fine_tol_e9), NULL));
    OK(qw_cell(m.w, "roomB.spread", m.temp, qi_point(spread_uK), NULL));
    OK(qw_port(m.w, m.b, "west", "agg", "roomB.agg"));
    OK(qw_port(m.w, m.b, "west", "fine", "roomB.fine.c0"));
    m.diff = qk_diffusion_new(n);
    m.refine = qk_refine_new(n);
    m.abstract = qk_abstract_new(n);
    OK(qw_model_attach(m.w, m.b, "fine", m.diff));

    OK(qw_cell(m.w, "wall.conductance", m.cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(m.w, "wall.tolerance", m.ratio, qi_point(10000000), NULL));
    OK(qw_connect(m.w, m.wall, "hot", "roomA"));
    OK(qw_connect(m.w, m.wall, "cold", "roomB.west"));
    OK(qw_attach(m.w, m.wall, qk_conduction()));
    return m;
}

static void free_mixed(Mixed *m)
{
    qw_free(m->w);
    qk_free(m->diff);
    qk_free(m->refine);
    qk_free(m->abstract);
}

static int64_t stock(Mixed *m, const char *path) { return qw_get(m->w, path).lo; }

static int64_t fine_sum(Mixed *m)
{
    int64_t s = 0;
    for (unsigned i = 0; i < m->n; i++) {
        char path[64];
        snprintf(path, sizeof path, "roomB.fine.c%u.energy", i);
        s += stock(m, path);
    }
    return s;
}

/* roomB's mean temperature in K, from its total energy */
static double room_b_mean(Mixed *m) { return (double) (stock(m, "roomB.agg.energy") + fine_sum(m)) / 1e7; }

static double temp(Mixed *m, const char *path)
{
    QInterval t = qw_get(m->w, path);
    return ((double) t.lo + (double) t.hi) / 2e6;
}

void test_models_switch_conservation(void)
{
    Mixed m = make_mixed(7, 4, 100, 50000000, 50000);
    int64_t total = qw_total(m.w, m.energy);
    CHECK(!strcmp(qw_active_model(m.w, m.b), "agg"));

    OK(qw_run(m.w, 200 * SEC));
    int64_t agg = stock(&m, "roomB.agg.energy");
    OK(qw_switch(m.w, m.b, "fine", m.refine));
    CHECK(!strcmp(qw_active_model(m.w, m.b), "fine"));
    CHECK_I64(stock(&m, "roomB.agg.energy"), 0);
    CHECK_I64(fine_sum(&m), agg);
    CHECK_I64(qw_total(m.w, m.energy), total);
    /* the port now leads to the first cell */
    QInterval west = qw_get(m.w, "roomB.west.temperature"), c0 = qw_get(m.w, "roomB.fine.c0.temperature");
    CHECK(west.lo == c0.lo && west.hi == c0.hi);

    OK(qw_run(m.w, 600 * SEC));
    CHECK_I64(qw_total(m.w, m.energy), total);
    int64_t fine = fine_sum(&m);
    OK(qw_switch(m.w, m.b, "agg", m.abstract));
    CHECK_I64(fine_sum(&m), 0);
    CHECK_I64(stock(&m, "roomB.agg.energy"), fine);
    CHECK_I64(qw_total(m.w, m.energy), total);
    OK(qw_run(m.w, 1000 * SEC));
    CHECK_I64(qw_total(m.w, m.energy), total);
    CHECK_I64((int64_t) qw_switches(m.w), 2);
    free_mixed(&m);
}

/* The same state and seed give the same detail (design section 14.4). */
void test_models_reproducible(void)
{
    Mixed a = make_mixed(7, 6, 100, 50000000, 50000), b = make_mixed(7, 6, 100, 50000000, 50000),
          c = make_mixed(8, 6, 100, 50000000, 50000);
    Mixed *all[3] = { &a, &b, &c };
    for (int i = 0; i < 3; i++) {
        OK(qw_run(all[i]->w, 100 * SEC));
        OK(qw_switch(all[i]->w, all[i]->b, "fine", all[i]->refine));
    }
    bool differs = false;
    for (unsigned i = 0; i < 6; i++) {
        char path[64];
        snprintf(path, sizeof path, "roomB.fine.c%u.energy", i);
        CHECK_I64(stock(&a, path), stock(&b, path));
        if (stock(&a, path) != stock(&c, path)) differs = true;
    }
    CHECK(differs);   /* another seed, other detail */
    CHECK_I64(fine_sum(&a), fine_sum(&c));   /* but the same total */
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    for (int i = 0; i < 3; i++) free_mixed(all[i]);
}

/* A well-mixed room gives nearly the same result at every level of detail.
 * A poorly mixed room does not, which is why detail can matter. */
void test_models_accuracy(void)
{
    double ta[3], tb[3];
    for (int mode = 0; mode < 3; mode++) {   /* agg only, mixed, fine only */
        Mixed m = make_mixed(7, 4, 1000, 50000000, mode == 2 ? 0 : 20000);
        if (mode == 2) OK(qw_switch(m.w, m.b, "fine", m.refine));
        if (mode == 1) {
            OK(qw_run(m.w, 200 * SEC));
            OK(qw_switch(m.w, m.b, "fine", m.refine));
            OK(qw_run(m.w, 600 * SEC));
            OK(qw_switch(m.w, m.b, "agg", m.abstract));
        }
        OK(qw_run(m.w, 1000 * SEC));
        ta[mode] = temp(&m, "roomA.temperature");
        tb[mode] = room_b_mean(&m);
        free_mixed(&m);
    }
    CHECK(fabs(ta[1] - ta[0]) < 0.05);
    CHECK(fabs(tb[1] - tb[0]) < 0.05);
    CHECK(fabs(ta[2] - ta[0]) < 0.05);
    CHECK(fabs(tb[2] - tb[0]) < 0.05);

    /* poorly mixed: heat collects in c0, so less heat leaves roomA */
    Mixed agg = make_mixed(7, 4, 1, 50000000, 0), fine = make_mixed(7, 4, 1, 50000000, 0);
    OK(qw_switch(fine.w, fine.b, "fine", fine.refine));
    OK(qw_run(agg.w, 1000 * SEC));
    OK(qw_run(fine.w, 1000 * SEC));
    CHECK(temp(&fine, "roomA.temperature") - temp(&agg, "roomA.temperature") > 0.5);
    free_mixed(&agg);
    free_mixed(&fine);
}

/* The coarse model costs less, and a dormant model costs nothing. */
void test_models_cost(void)
{
    Mixed agg = make_mixed(7, 4, 1000, 50000000, 0), fine = make_mixed(7, 4, 1000, 50000000, 0);
    OK(qw_switch(fine.w, fine.b, "fine", fine.refine));
    uint64_t runs_agg0 = qn_runs(qw_net(agg.w)), runs_fine0 = qn_runs(qw_net(fine.w));
    OK(qw_run(agg.w, 400 * SEC));
    OK(qw_run(fine.w, 400 * SEC));
    uint64_t adv_agg = qw_advances(agg.w), adv_fine = qw_advances(fine.w);
    uint64_t runs_agg = qn_runs(qw_net(agg.w)) - runs_agg0, runs_fine = qn_runs(qw_net(fine.w)) - runs_fine0;
    CHECK(adv_fine > 10 * adv_agg);
    CHECK(runs_fine > 10 * runs_agg);
    /* the fine model's propagators never ran in the aggregate world */
    QInterval t = qw_get(agg.w, "roomB.fine.c0.temperature");
    CHECK(t.lo == Q_NINF && t.hi == Q_INF);
    free_mixed(&agg);
    free_mixed(&fine);
}

/* After a switch, the wall reads through the port again. */
void test_models_wall_rebinds(void)
{
    Mixed m = make_mixed(7, 4, 100, 50000000, 0);
    OK(qw_run(m.w, 102 * SEC));   /* not on a boundary of the wall: its prediction is still valid */
    uint64_t before = qw_predicts(m.w, m.wall);
    OK(qw_switch(m.w, m.b, "fine", m.refine));
    OK(qw_run(m.w, 103 * SEC));
    CHECK(qw_predicts(m.w, m.wall) > before);
    /* the wall's step now follows c0's capacity, a quarter of the room's */
    uint64_t b0 = qw_boundaries(m.w);
    OK(qw_run(m.w, 203 * SEC));
    CHECK(qw_boundaries(m.w) - b0 > 20);
    free_mixed(&m);
}

/* A map that leaves energy behind is refused, and nothing changes. */
enum { L_FROM, L_TO, L_NSLOTS };
static const QkSlot leaky_slots[L_NSLOTS] = {
    [L_FROM] = { "from.energy", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
    [L_TO]   = { "to.c0.energy", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
};
static const QkTransfer leaky_transfers[] = { { "half", L_FROM, L_TO, true } };

struct QkInstance { const QkHost *host; };

static QkResult leaky_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    *out = i;
    return QK_OK;
}

static void leaky_destroy(QkInstance *i) { free(i); }

static QkResult leaky_map(QkInstance *i, QkCall *call)
{
    i->host->transfer(call, 0, i->host->read(call, L_FROM).lo / 2);
    return QK_OK;
}

static const QkKernel leaky = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.leaky",
    .flags = QK_MAP | QK_STATELESS,
    .slots = leaky_slots,
    .nslots = L_NSLOTS,
    .transfers = leaky_transfers,
    .ntransfers = 1,
    .create = leaky_create,
    .destroy = leaky_destroy,
    .map = leaky_map,
};

void test_models_failed_switch(void)
{
    Mixed m = make_mixed(7, 4, 100, 50000000, 0);
    OK(qw_run(m.w, 50 * SEC));
    uint64_t h = qw_hash(m.w);
    CHECK(qw_switch(m.w, m.b, "fine", &leaky) == QW_ESTOCK);
    CHECK(strstr(qw_error(m.w), "roomB.agg.energy") != NULL);
    CHECK(qw_hash(m.w) == h);
    CHECK(!strcmp(qw_active_model(m.w, m.b), "agg"));

    /* a map for another cell count cannot bind */
    QkKernel *five = qk_abstract_new(5);
    OK(qw_switch(m.w, m.b, "fine", m.refine));
    h = qw_hash(m.w);
    CHECK(qw_switch(m.w, m.b, "agg", five) == QW_ENAME);
    CHECK(strstr(qw_error(m.w), "roomB.fine.c4.energy") != NULL);
    CHECK(qw_hash(m.w) == h);
    CHECK(!strcmp(qw_active_model(m.w, m.b), "fine"));
    OK(qw_switch(m.w, m.b, "agg", m.abstract));
    OK(qw_run(m.w, 100 * SEC));
    qk_free(five);
    free_mixed(&m);
}

void test_models_errors(void)
{
    Mixed m = make_mixed(7, 4, 100, 50000000, 0);
    CHECK(qw_switch(m.w, m.b, "medium", m.refine) == QW_ENAME);
    CHECK(qw_switch(m.w, m.b, "agg", m.refine) == QW_EDUP);
    CHECK(qw_switch(m.w, m.b, "fine", qk_conduction()) == QW_EKERNEL);
    CHECK(qw_cell(m.w, "roomB.fine.extra", m.energy, qi_point(5), NULL) == QW_ESTOCK);
    CHECK(qw_cell(m.w, "roomB.fine", m.temp, qi_top(), NULL) == QW_ENAME);
    CHECK(qw_model(m.w, m.b, "agg") == QW_EDUP);
    CHECK(qw_model(m.w, m.a, "energy") == QW_EDUP);   /* roomA.energy exists */
    CHECK(qw_model_attach(m.w, m.b, "medium", m.diff) == QW_ENAME);
    CHECK(qw_model_attach(m.w, m.b, "fine", m.diff) == QW_EKERNEL);   /* already has one */
    CHECK(qw_attach(m.w, m.a, m.refine) == QW_EKERNEL);               /* a map kernel */
    CHECK(qw_port(m.w, m.b, "west", "agg", "roomB.agg") == QW_EDUP);
    CHECK(qw_port(m.w, m.b, "east", "medium", "roomB.agg") == QW_ENAME);
    CHECK(qw_active_model(m.w, m.a) == NULL);
    free_mixed(&m);
}

void test_models_determinism(void)
{
    uint64_t hashes[2];
    for (int i = 0; i < 2; i++) {
        Mixed m = make_mixed(7, 4, 100, 50000000, 30000);
        OK(qw_run(m.w, 200 * SEC));
        OK(qw_switch(m.w, m.b, "fine", m.refine));
        OK(qw_run(m.w, 400 * SEC));
        OK(qw_switch(m.w, m.b, "agg", m.abstract));
        OK(qw_run(m.w, 500 * SEC));
        hashes[i] = qw_hash(m.w);
        free_mixed(&m);
    }
    CHECK(hashes[0] == hashes[1]);
}
