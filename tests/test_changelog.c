#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* The change log of ADR 0011 section 6, and commit by touched cells. */

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL

static bool same(QInterval a, QInterval b) { return a.lo == b.lo && a.hi == b.hi; }

static QnCell cell_of(QWorld *w, const char *path)
{
    QnCell c = 0;
    OK(qw_find(w, path, &c));
    return c;
}

/* Entries must be in order of epoch, then of cell index, with one entry for
 * each cell in an epoch. */
static void check_order(const QwChange *e, size_t n)
{
    for (size_t i = 1; i < n; i++)
        CHECK(e[i - 1].epoch < e[i].epoch || (e[i - 1].epoch == e[i].epoch && e[i - 1].cell < e[i].cell));
}

/* A stock entry must have after = before + amount, as exact points. */
static void check_stock(const QwChange *e)
{
    CHECK(e->kind == QW_CHANGE_STOCK);
    CHECK(e->before.lo == e->before.hi && e->after.lo == e->after.hi);
    CHECK_I64(e->after.lo, e->before.lo + e->amount);
    CHECK(e->amount != 0);
}

/* --- two rooms and a wall, as in test_world.c ------------------------------------------ */

typedef struct {
    QWorld *w;
    QwPhen  a, b, wall;
    QwKind  energy, temp, cap, cond, ratio;
} Rooms;

static Rooms make_rooms(uint64_t seed)
{
    Rooms r;
    memset(&r, 0, sizeof r);
    r.w = qw_new(seed, -6);
    OK(qw_kind(r.w, "energy", "J", -3, true, &r.energy));
    OK(qw_kind(r.w, "temperature", "K", -6, false, &r.temp));
    OK(qw_kind(r.w, "capacity", "J/K", -3, false, &r.cap));
    OK(qw_kind(r.w, "conductance", "W/K", -3, false, &r.cond));
    OK(qw_kind(r.w, "ratio", "", -9, false, &r.ratio));
    OK(qw_phenomenon(r.w, "roomA", &r.a));
    OK(qw_phenomenon(r.w, "roomB", &r.b));
    OK(qw_phenomenon(r.w, "wall", &r.wall));
    const char *names[2] = { "roomA", "roomB" };
    double temps[2] = { 303.15, 293.15 };
    for (int i = 0; i < 2; i++) {
        char path[64];
        QnCell e, c, t;
        int64_t cq = 10000 * 1000;
        snprintf(path, sizeof path, "%s.energy", names[i]);
        OK(qw_cell(r.w, path, r.energy, qi_point((int64_t) llround((double) cq * temps[i])), &e));
        snprintf(path, sizeof path, "%s.capacity", names[i]);
        OK(qw_cell(r.w, path, r.cap, qi_point(cq), &c));
        snprintf(path, sizeof path, "%s.temperature", names[i]);
        OK(qw_cell(r.w, path, r.temp, qi_top(), &t));
        CHECK(qn_product(qw_net(r.w), e, c, t, NULL) == QN_OK);
    }
    OK(qw_cell(r.w, "wall.conductance", r.cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(r.w, "wall.tolerance", r.ratio, qi_point(10000000), NULL));
    OK(qw_connect(r.w, r.wall, "hot", "roomA"));
    OK(qw_connect(r.w, r.wall, "cold", "roomB"));
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    return r;
}

void test_changelog_rooms(void)
{
    Rooms r = make_rooms(1);
    qw_log_changes(r.w, true);
    QnCell ea = cell_of(r.w, "roomA.energy"), eb = cell_of(r.w, "roomB.energy");
    uint64_t e0 = qn_epoch(qw_net(r.w));
    OK(qw_run(r.w, 100 * SEC));
    uint64_t nb = qw_boundaries(r.w);
    CHECK(nb >= 10);

    size_t n;
    const QwChange *e = qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, (int64_t) (2 * nb));
    check_order(e, n);
    for (size_t i = 0; i + 1 < n; i += 2) {
        const QwChange *x = &e[i], *y = &e[i + 1];
        /* one boundary: roomA.energy, then roomB.energy, in the next epoch */
        CHECK(x->epoch == y->epoch);
        CHECK_I64((int64_t) x->epoch, (int64_t) (e0 + 1 + i / 2));
        CHECK(x->cell == ea && y->cell == eb);
        check_stock(x);
        check_stock(y);
        CHECK_I64(x->amount, -y->amount);
        CHECK(x->amount < 0);   /* heat leaves the warm room */
        /* each entry starts where the last one for that cell ended */
        if (i >= 2) {
            CHECK(same(x->before, e[i - 2].after));
            CHECK(same(y->before, e[i - 1].after));
        }
    }
    CHECK_I64((int64_t) e[n - 1].epoch, (int64_t) qn_epoch(qw_net(r.w)));
    CHECK(same(e[n - 2].after, qn_base(qw_net(r.w), ea)));
    CHECK(same(e[n - 1].after, qn_base(qw_net(r.w), eb)));
    qw_free(r.w);
}

/* --- a world with models, as in test_models.c ------------------------------------------- */

typedef struct {
    QWorld   *w;
    QwPhen    a, b, wall;
    QwKind    energy, temp, cap, cond, ratio;
    QkKernel *diff, *refine, *abstract;
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

/* roomA, and roomB with the models "agg" (one stock) and "fine" (4 cells). */
static Mixed make_mixed(uint64_t seed)
{
    const unsigned n = 4;
    Mixed m;
    memset(&m, 0, sizeof m);
    m.w = qw_new(seed, -6);
    OK(qw_kind(m.w, "energy", "J", -3, true, &m.energy));
    OK(qw_kind(m.w, "temperature", "K", -6, false, &m.temp));
    OK(qw_kind(m.w, "capacity", "J/K", -3, false, &m.cap));
    OK(qw_kind(m.w, "conductance", "W/K", -3, false, &m.cond));
    OK(qw_kind(m.w, "ratio", "", -9, false, &m.ratio));
    OK(qw_phenomenon(m.w, "roomA", &m.a));
    OK(qw_phenomenon(m.w, "roomB", &m.b));
    OK(qw_phenomenon(m.w, "wall", &m.wall));
    const int64_t cap = 10000 * 1000;
    room_cells(&m, "roomA", cap, cap / 100 * 30315);
    OK(qw_model(m.w, m.b, "agg"));
    OK(qw_model(m.w, m.b, "fine"));
    OK(qw_model_begin(m.w, m.b, "agg"));
    room_cells(&m, "roomB.agg", cap, cap / 100 * 29315);
    qw_model_end(m.w);
    OK(qw_model_begin(m.w, m.b, "fine"));
    for (unsigned i = 0; i < n; i++) {
        char prefix[64];
        snprintf(prefix, sizeof prefix, "roomB.fine.c%u", i);
        room_cells(&m, prefix, cap / n, 0);
    }
    qw_model_end(m.w);
    OK(qw_cell(m.w, "roomB.fine.conductance", m.cond, qi_point(100 * 1000), NULL));
    OK(qw_cell(m.w, "roomB.fine.tolerance", m.ratio, qi_point(50000000), NULL));
    OK(qw_cell(m.w, "roomB.spread", m.temp, qi_point(30000), NULL));
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

/* Runs, switches, takes an input, and switches back. */
static void mixed_script(Mixed *m)
{
    OK(qw_run(m->w, 100 * SEC));
    OK(qw_switch(m->w, m->b, "fine", m->refine));
    OK(qw_run(m->w, 200 * SEC));
    OK(qw_input(m->w, "wall.conductance", qi_point(20 * 1000)));
    OK(qw_run(m->w, 300 * SEC));
    OK(qw_switch(m->w, m->b, "agg", m->abstract));
    OK(qw_run(m->w, 400 * SEC));
}

/* Applying the logged values in order to the initial bases gives every final
 * base exactly. */
void test_changelog_replay(void)
{
    Mixed m = make_mixed(7);
    QNet *net = qw_net(m.w);
    size_t nc = qn_ncells(net);
    QInterval *bases = malloc(nc * sizeof *bases);
    for (size_t c = 0; c < nc; c++) bases[c] = qn_base(net, (QnCell) c);
    qw_log_changes(m.w, true);
    mixed_script(&m);

    size_t n;
    const QwChange *e = qw_changes(m.w, &n);
    CHECK(n > 0);
    check_order(e, n);
    bool kinds[2] = { false, false };
    for (size_t i = 0; i < n; i++) {
        CHECK(e[i].cell < nc);
        CHECK(same(e[i].before, bases[e[i].cell]));   /* the log has no gaps */
        bases[e[i].cell] = e[i].after;
        kinds[e[i].kind] = true;
        if (e[i].kind == QW_CHANGE_STOCK) check_stock(&e[i]);
    }
    CHECK(kinds[QW_CHANGE_VALUE] && kinds[QW_CHANGE_STOCK]);
    for (size_t c = 0; c < nc; c++) CHECK(same(bases[c], qn_base(net, (QnCell) c)));
    free(bases);
    free_mixed(&m);
}

/* The leaky map of test_models.c: it leaves half the energy behind. */
enum { L_FROM, L_TO, L_NSLOTS };
static const QkSlot leaky_slots[L_NSLOTS] = {
    [L_FROM] = { "from.energy", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
    [L_TO]   = { "to.c0.energy", QK_STOCK, "J", -3, { Q_NINF, Q_INF } },
};
static const QkTransfer leaky_transfers[] = { { "half", L_FROM, L_TO, true } };

struct QkInstance { const QkHost *host; };

static QkResult inst_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    *out = i;
    return QK_OK;
}

static void inst_destroy(QkInstance *i) { free(i); }

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
    .create = inst_create,
    .destroy = inst_destroy,
    .map = leaky_map,
};

void test_changelog_input_and_switch(void)
{
    Mixed m = make_mixed(7);
    QNet *net = qw_net(m.w);
    qw_log_changes(m.w, true);
    OK(qw_run(m.w, 50 * SEC));
    size_t n0, n;
    const QwChange *e = qw_changes(m.w, &n0);

    /* an input: one value entry in the new epoch */
    QnCell g = cell_of(m.w, "wall.conductance");
    OK(qw_input(m.w, "wall.conductance", qi_point(20 * 1000)));
    e = qw_changes(m.w, &n);
    CHECK_I64((int64_t) n, (int64_t) n0 + 1);
    CHECK(e[n - 1].kind == QW_CHANGE_VALUE);
    CHECK(e[n - 1].cell == g);
    CHECK(same(e[n - 1].before, qi_point(10 * 1000)));
    CHECK(same(e[n - 1].after, qi_point(20 * 1000)));
    CHECK_I64(e[n - 1].amount, 0);
    CHECK_I64((int64_t) e[n - 1].epoch, (int64_t) qn_epoch(net));
    /* an input of the same value changes nothing */
    OK(qw_input(m.w, "wall.conductance", qi_point(20 * 1000)));
    qw_changes(m.w, &n0);
    CHECK_I64((int64_t) n0, (int64_t) n);

    /* a failed switch records nothing */
    CHECK(qw_switch(m.w, m.b, "fine", &leaky) == QW_ESTOCK);
    qw_changes(m.w, &n);
    CHECK_I64((int64_t) n, (int64_t) n0);

    /* a successful switch: the aggregate stock goes to zero and the four
     * cells take its energy, all in the epoch of the switch */
    QnCell agg = cell_of(m.w, "roomB.agg.energy");
    int64_t agg_energy = qn_base(net, agg).lo;
    OK(qw_switch(m.w, m.b, "fine", m.refine));
    e = qw_changes(m.w, &n);
    CHECK_I64((int64_t) (n - n0), 5);
    check_order(e, n);
    int64_t sum = 0;
    for (size_t i = n0; i < n; i++) {
        CHECK_I64((int64_t) e[i].epoch, (int64_t) qn_epoch(net));
        check_stock(&e[i]);
        sum += e[i].amount;
    }
    CHECK(e[n0].cell == agg);
    CHECK_I64(e[n0].amount, -agg_energy);
    CHECK_I64(e[n0].after.lo, 0);
    for (unsigned i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof path, "roomB.fine.c%u.energy", i);
        CHECK(e[n0 + 1 + i].cell == cell_of(m.w, path));
    }
    CHECK_I64(sum, 0);
    free_mixed(&m);
}

/* --- a failed step ------------------------------------------------------------------- */

/* An evolution kernel that adds 5 mJ to its store at each step, and fails
 * after the transfer while `fail_step` is set. */
static bool fail_step;
enum { F_STORE, F_NSLOTS };
static const QkSlot fail_slots[F_NSLOTS] = { [F_STORE] = { "store", QK_STOCK, "J", -3, { Q_NINF, Q_INF } } };
static const QkTransfer fail_transfers[] = { { "in", QK_NONE, F_STORE, false } };

static QkResult fail_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    out->horizon = i->host->now(call) + SEC;
    out->event_lo = out->event_hi = QK_NO_TIME;
    return QK_OK;
}

static QkResult fail_advance(QkInstance *i, QkCall *call, int64_t to)
{
    (void) to;
    i->host->transfer(call, 0, 5);
    return fail_step ? QK_ERROR : QK_OK;
}

static const QkKernel failing = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.failing",
    .flags = QK_EVOLVE,
    .slots = fail_slots,
    .nslots = F_NSLOTS,
    .transfers = fail_transfers,
    .ntransfers = 1,
    .create = inst_create,
    .destroy = inst_destroy,
    .predict = fail_predict,
    .advance = fail_advance,
};

void test_changelog_failed_step(void)
{
    Rooms r = make_rooms(1);
    QwPhen z;
    OK(qw_phenomenon(r.w, "z", &z));   /* after the wall, so the wall has staged its transfer */
    OK(qw_cell(r.w, "z.store", r.energy, qi_point(0), NULL));
    OK(qw_attach(r.w, z, &failing));
    qw_log_changes(r.w, true);
    OK(qw_run(r.w, 2 * SEC));
    size_t n0, n;
    qw_changes(r.w, &n0);
    CHECK(n0 > 0);
    uint64_t h = qw_hash(r.w);

    fail_step = true;
    CHECK(qw_run(r.w, 3 * SEC) == QW_EKERNEL);
    fail_step = false;
    qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, (int64_t) n0);
    CHECK(qw_hash(r.w) == h);

    /* the next step starts from clear staging: three entries, and the store
     * gets 5 mJ, not 10 */
    OK(qw_run(r.w, 3 * SEC));
    const QwChange *e = qw_changes(r.w, &n);
    CHECK_I64((int64_t) (n - n0), 3);
    check_order(e, n);
    CHECK(e[n - 1].cell == cell_of(r.w, "z.store"));
    CHECK_I64(e[n - 1].amount, 5);
    CHECK_I64(e[n0].amount, -e[n0 + 1].amount);
    qw_free(r.w);
}

/* --- logging off, and discarding ------------------------------------------------------ */

void test_changelog_off_and_discard(void)
{
    Rooms r = make_rooms(1);
    size_t n;
    OK(qw_run(r.w, 20 * SEC));
    OK(qw_input(r.w, "wall.conductance", qi_point(20 * 1000)));
    CHECK(qw_changes(r.w, &n) == NULL || n == 0);
    CHECK_I64((int64_t) n, 0);

    qw_log_changes(r.w, true);
    OK(qw_run(r.w, 60 * SEC));
    const QwChange *e = qw_changes(r.w, &n);
    CHECK(n >= 8);
    QwChange *copy = malloc(n * sizeof *copy);
    memcpy(copy, e, n * sizeof *copy);
    size_t n_all = n;

    /* turning logging off keeps the entries and records no new ones */
    qw_log_changes(r.w, false);
    OK(qw_run(r.w, 80 * SEC));
    qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, (int64_t) n_all);

    qw_changes_discard(r.w, 0);
    qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, (int64_t) n_all);

    uint64_t cut = copy[n_all / 2].epoch;
    size_t keep = 0;
    for (size_t i = 0; i < n_all; i++)
        if (copy[i].epoch >= cut) keep++;
    CHECK(keep > 0 && keep < n_all);
    qw_changes_discard(r.w, cut);
    e = qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, (int64_t) keep);
    for (size_t i = 0; i < n; i++) {
        const QwChange *x = &e[i], *y = &copy[n_all - keep + i];
        CHECK(x->epoch == y->epoch && x->cell == y->cell && x->amount == y->amount);
        CHECK(same(x->before, y->before) && same(x->after, y->after));
    }

    qw_changes_discard(r.w, UINT64_MAX);
    qw_changes(r.w, &n);
    CHECK_I64((int64_t) n, 0);
    free(copy);
    qw_free(r.w);
}

/* --- determinism ----------------------------------------------------------------- */

void test_changelog_determinism(void)
{
    Mixed a = make_mixed(7), b = make_mixed(7);
    qw_log_changes(a.w, true);
    qw_log_changes(b.w, true);
    mixed_script(&a);
    mixed_script(&b);
    size_t na, nb;
    const QwChange *ea = qw_changes(a.w, &na), *eb = qw_changes(b.w, &nb);
    CHECK(na > 0);
    CHECK_I64((int64_t) na, (int64_t) nb);
    for (size_t i = 0; i < na && i < nb; i++) {
        CHECK(ea[i].epoch == eb[i].epoch);
        CHECK(ea[i].cell == eb[i].cell);
        CHECK(ea[i].kind == eb[i].kind);
        CHECK(same(ea[i].before, eb[i].before));
        CHECK(same(ea[i].after, eb[i].after));
        CHECK(ea[i].amount == eb[i].amount);
    }
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    free_mixed(&a);
    free_mixed(&b);
}
