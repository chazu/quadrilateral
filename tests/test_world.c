#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

static QInterval iv(int64_t lo, int64_t hi) { return (QInterval) { lo, hi }; }

#define OK(x) CHECK((x) == QW_OK)

/* --- dimension text ------------------------------------------------------------- */

static QDim dimof(const char *s, QStatus *st)
{
    QDim d = qdim_none();
    const char *names[] = { "currency", "bit" };
    *st = q_parse_dim(s, strlen(s), names, 2, &d);
    return d;
}

void test_parse_dim(void)
{
    QStatus st;
    QDim j = dimof("J", &st), a = dimof("kg m^2 s^-2", &st), b = dimof("kg*m^2/s^2", &st), c = dimof("W s", &st);
    CHECK(st == Q_OK);
    CHECK(qdim_eq(&j, &a) && qdim_eq(&a, &b) && qdim_eq(&b, &c));
    QDim wk = dimof("W/K", &st);
    CHECK(st == Q_OK);
    CHECK_I64(wk.exp[Q_BASE_TEMPERATURE], -1);
    CHECK_I64(wk.exp[Q_BASE_TIME], -3);
    QDim none = dimof("", &st), one = dimof("1", &st);
    CHECK(qdim_is_none(&none) && qdim_is_none(&one));
    QDim price = dimof("currency/kg", &st);
    CHECK(st == Q_OK);
    CHECK_I64(price.exp[Q_NSI], 1);
    CHECK_I64(price.exp[Q_BASE_MASS], -1);
    QDim hz = dimof("Hz", &st), per_s = dimof("s^-1", &st);
    CHECK(qdim_eq(&hz, &per_s));
    QDim pa = dimof("Pa", &st), npm = dimof("N/m^2", &st);
    CHECK(qdim_eq(&pa, &npm));

    dimof("furlong", &st);
    CHECK(st == Q_EDIM);
    const char *bad[] = { "kg^", "kg^x", "/s", "m/", "m/s/s", "2m", "m^-" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        dimof(bad[i], &st);
        CHECK(st == Q_ESYNTAX);
    }
}

/* --- two rooms and a wall ------------------------------------------------------------ */

typedef struct {
    QWorld *w;
    QwPhen  a, b, wall;
    QwKind  energy, temp, cap, cond, ratio;
} Rooms;

/* Two lumped rooms at temperatures ta, tb (K) with capacities ca, cb (J/K),
 * joined by a wall of conductance g (W/K) and the given tolerance. */
static Rooms make_rooms(uint64_t seed, double ta, double tb, int64_t ca, int64_t cb, int64_t g, int64_t tol_e9)
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
    double temps[2] = { ta, tb };
    int64_t caps[2] = { ca, cb };
    for (int i = 0; i < 2; i++) {
        char path[64];
        QnCell e, c, t;
        int64_t cq = caps[i] * 1000;                                 /* mJ/K */
        int64_t eq = (int64_t) llround((double) cq * temps[i]);      /* mJ */
        snprintf(path, sizeof path, "%s.energy", names[i]);
        OK(qw_cell(r.w, path, r.energy, qi_point(eq), &e));
        snprintf(path, sizeof path, "%s.capacity", names[i]);
        OK(qw_cell(r.w, path, r.cap, qi_point(cq), &c));
        snprintf(path, sizeof path, "%s.temperature", names[i]);
        OK(qw_cell(r.w, path, r.temp, qi_top(), &t));
        CHECK(qn_product(qw_net(r.w), e, c, t, NULL) == QN_OK);   /* energy = capacity × temperature */
    }
    OK(qw_cell(r.w, "wall.conductance", r.cond, qi_point(g * 1000), NULL));
    OK(qw_cell(r.w, "wall.tolerance", r.ratio, qi_point(tol_e9), NULL));
    OK(qw_connect(r.w, r.wall, "hot", "roomA"));
    OK(qw_connect(r.w, r.wall, "cold", "roomB"));
    return r;
}

static double temp_of(QWorld *w, const char *path)
{
    QInterval t = qw_get(w, path);
    return ((double) t.lo + (double) t.hi) / 2 * 1e-6;
}

static int64_t energy_of(QWorld *w, const char *path) { return qw_get(w, path).lo; }

/* The exact solution for two lumped capacities joined by a conductance. */
static void exact(double ta, double tb, double ca, double cb, double g, double t, double *xa, double *xb)
{
    double tm = (ca * ta + cb * tb) / (ca + cb);
    double tau = ca * cb / (g * (ca + cb));
    double d = (ta - tb) * exp(-t / tau);
    *xa = tm + cb / (ca + cb) * d;
    *xb = tm - ca / (ca + cb) * d;
}

/* The largest temperature error over the run, sampled every `every` seconds. */
static double run_error(Rooms *r, double ta, double tb, int64_t ca, int64_t cb, int64_t g, int seconds, int every)
{
    double worst = 0;
    for (int s = every; s <= seconds; s += every) {
        CHECK(qw_run(r->w, (int64_t) s * 1000000) == QW_OK);
        double xa, xb;
        exact(ta, tb, (double) ca, (double) cb, (double) g, s, &xa, &xb);
        double ea = fabs(temp_of(r->w, "roomA.temperature") - xa), eb = fabs(temp_of(r->w, "roomB.temperature") - xb);
        if (ea > worst) worst = ea;
        if (eb > worst) worst = eb;
    }
    return worst;
}

void test_two_rooms_analytic(void)
{
    /* τ = 500 s; the tolerance 0.01 gives steps of 5 s */
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    int64_t total = energy_of(r.w, "roomA.energy") + energy_of(r.w, "roomB.energy");
    double err = run_error(&r, 303.15, 293.15, 10000, 10000, 10, 1000, 50);
    CHECK(err < 0.05);
    CHECK_I64((int64_t) qw_boundaries(r.w), 200);
    /* conservation is exact */
    CHECK_I64(energy_of(r.w, "roomA.energy") + energy_of(r.w, "roomB.energy"), total);
    /* the rooms approach each other: after 2τ the difference is about 10 e^-2 K */
    double diff = temp_of(r.w, "roomA.temperature") - temp_of(r.w, "roomB.temperature");
    CHECK(diff > 1.3 && diff < 1.4);
    qw_free(r.w);

    /* unequal rooms: τ = 400 s */
    r = make_rooms(1, 310.0, 280.0, 5000, 20000, 10, 10000000);
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    total = energy_of(r.w, "roomA.energy") + energy_of(r.w, "roomB.energy");
    err = run_error(&r, 310.0, 280.0, 5000, 20000, 10, 2000, 100);
    CHECK(err < 0.15);
    CHECK_I64(energy_of(r.w, "roomA.energy") + energy_of(r.w, "roomB.energy"), total);
    qw_free(r.w);
}

/* Explicit coupling has first-order error: half the step, about half the error. */
void test_two_rooms_convergence(void)
{
    double errs[2];
    int64_t tols[2] = { 20000000, 10000000 };
    for (int i = 0; i < 2; i++) {
        Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, tols[i]);
        OK(qw_attach(r.w, r.wall, qk_conduction()));
        errs[i] = run_error(&r, 303.15, 293.15, 10000, 10000, 10, 1000, 100);
        qw_free(r.w);
    }
    double ratio = errs[1] / errs[0];
    CHECK(ratio > 0.4 && ratio < 0.6);
}

/* ADR 0004: the same inputs give the same state, and a run in pieces that
 * meet at boundaries gives the same state as one run. */
void test_two_rooms_determinism(void)
{
    Rooms a = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    Rooms b = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    OK(qw_attach(a.w, a.wall, qk_conduction()));
    OK(qw_attach(b.w, b.wall, qk_conduction()));
    OK(qw_run(a.w, 600000000));
    OK(qw_run(b.w, 300000000));
    OK(qw_run(b.w, 600000000));
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    CHECK(qw_hash(a.w) != 0);
    qw_free(a.w);
    qw_free(b.w);
}

/* Predictions last until their horizon, and an input that breaks an
 * assumption invalidates them early. */
void test_two_rooms_invalidation(void)
{
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);   /* steps of 5 s */
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    OK(qw_run(r.w, 2500000));
    CHECK_I64((int64_t) qw_predicts(r.w, r.wall), 1);
    OK(qw_run(r.w, 5000000));
    CHECK_I64((int64_t) qw_predicts(r.w, r.wall), 1);   /* the same prediction reached its horizon */
    OK(qw_run(r.w, 7500000));
    CHECK_I64((int64_t) qw_predicts(r.w, r.wall), 2);

    /* doubling the conductance breaks the prediction's assumption */
    OK(qw_input(r.w, "wall.conductance", qi_point(20000)));
    uint64_t before = qw_boundaries(r.w);
    OK(qw_run(r.w, 10000000));
    CHECK_I64((int64_t) qw_predicts(r.w, r.wall), 3);
    CHECK_I64((int64_t) (qw_boundaries(r.w) - before), 1);   /* the new step is 2.5 s: one boundary */
    OK(qw_run(r.w, 12500000));
    CHECK_I64((int64_t) (qw_boundaries(r.w) - before), 2);
    qw_free(r.w);
}

void test_world_build_errors(void)
{
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    QnCell c;
    CHECK(qw_cell(r.w, "nowhere.x", r.temp, qi_top(), &c) == QW_ENAME);
    CHECK(strstr(qw_error(r.w), "nowhere.x") != NULL);
    CHECK(qw_cell(r.w, "roomA", r.temp, qi_top(), &c) == QW_ENAME);
    CHECK(qw_cell(r.w, "roomA.energy", r.energy, qi_point(1), &c) == QW_EDUP);
    CHECK(qw_cell(r.w, "roomA.spare", r.energy, iv(1, 2), &c) == QW_ESTOCK);
    CHECK(qw_input(r.w, "roomA.energy", qi_point(5)) == QW_ESTOCK);
    CHECK(qw_input(r.w, "roomA.missing", qi_point(5)) == QW_ENAME);
    QwKind k;
    CHECK(qw_kind(r.w, "energy", "J", -3, true, &k) == QW_EDUP);
    CHECK(qw_kind(r.w, "weird", "furlong", 0, false, &k) == QW_ETYPE);
    QwPhen p;
    CHECK(qw_phenomenon(r.w, "roomA", &p) == QW_EDUP);
    CHECK(qw_phenomenon(r.w, "bad.name", &p) == QW_ENAME);
    CHECK(qw_connect(r.w, r.wall, "hot", "roomB") == QW_EDUP);

    /* a kernel with another interface version */
    QkKernel other = *qk_conduction();
    other.abi_version = 2;
    CHECK(qw_attach(r.w, r.wall, &other) == QW_EKERNEL);
    qw_free(r.w);

    /* a port that is not connected leaves slots without cells */
    QWorld *w = qw_new(1, -6);
    QwKind e, t, cap, g, ratio;
    QwPhen a, wall;
    OK(qw_kind(w, "energy", "J", -3, true, &e));
    OK(qw_kind(w, "temperature", "K", -6, false, &t));
    OK(qw_kind(w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(w, "conductance", "W/K", -3, false, &g));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "roomA", &a));
    OK(qw_phenomenon(w, "wall", &wall));
    OK(qw_cell(w, "roomA.energy", e, qi_point(1000), NULL));
    OK(qw_cell(w, "roomA.capacity", cap, qi_point(1000), NULL));
    OK(qw_cell(w, "roomA.temperature", t, qi_top(), NULL));
    OK(qw_cell(w, "wall.conductance", g, qi_point(1), NULL));
    OK(qw_cell(w, "wall.tolerance", ratio, qi_point(1), NULL));
    OK(qw_connect(w, wall, "hot", "roomA"));
    CHECK(qw_attach(w, wall, qk_conduction()) == QW_ENAME);
    CHECK(strstr(qw_error(w), "wall.cold.temperature") != NULL);
    qw_free(w);
}

/* The world checks each slot against the cell it binds. */
void test_world_slot_types(void)
{
    struct { const char *energy_dim; int energy_scale; bool conserved; const char *cap_dim; QwStatus want; } cases[] = {
        { "J", -3, true, "J/K", QW_OK },
        { "J", -6, true, "J/K", QW_ETYPE },    /* a link must have the stock's scale */
        { "J", -3, false, "J/K", QW_ETYPE },   /* a link must be a stock */
        { "J", -3, true, "J", QW_ETYPE },      /* capacity with the wrong dimension */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        QWorld *w = qw_new(1, -6);
        QwKind e, t, cap, g, ratio;
        QwPhen a, b, wall;
        OK(qw_kind(w, "energy", cases[i].energy_dim, cases[i].energy_scale, cases[i].conserved, &e));
        OK(qw_kind(w, "temperature", "K", -6, false, &t));
        OK(qw_kind(w, "capacity", cases[i].cap_dim, -3, false, &cap));
        OK(qw_kind(w, "conductance", "W/K", -3, false, &g));
        OK(qw_kind(w, "ratio", "", -9, false, &ratio));
        OK(qw_phenomenon(w, "roomA", &a));
        OK(qw_phenomenon(w, "roomB", &b));
        OK(qw_phenomenon(w, "wall", &wall));
        const char *rooms[2] = { "roomA", "roomB" };
        for (int j = 0; j < 2; j++) {
            char path[64];
            snprintf(path, sizeof path, "%s.energy", rooms[j]);
            OK(qw_cell(w, path, e, qi_point(1000), NULL));
            snprintf(path, sizeof path, "%s.capacity", rooms[j]);
            OK(qw_cell(w, path, cap, qi_point(1000), NULL));
            snprintf(path, sizeof path, "%s.temperature", rooms[j]);
            OK(qw_cell(w, path, t, qi_top(), NULL));
        }
        OK(qw_cell(w, "wall.conductance", g, qi_point(1), NULL));
        OK(qw_cell(w, "wall.tolerance", ratio, qi_point(1), NULL));
        OK(qw_connect(w, wall, "hot", "roomA"));
        OK(qw_connect(w, wall, "cold", "roomB"));
        QwStatus s = qw_attach(w, wall, qk_conduction());
        CHECK_I64(s, cases[i].want);
        if (s != QW_OK) CHECK(strstr(qw_error(w), "slot") != NULL);
        qw_free(w);
    }
}

void test_world_inapplicable(void)
{
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    OK(qw_input(r.w, "wall.tolerance", qi_point(0)));   /* outside (0, 1] */
    CHECK(qw_run(r.w, 1000000) == QW_EINAPPLICABLE);
    CHECK(strstr(qw_error(r.w), "wall.tolerance") != NULL);
    qw_free(r.w);
}

void test_world_contradiction(void)
{
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    QnCell t;
    OK(qw_find(r.w, "roomA.temperature", &t));
    QnCell x;
    OK(qw_cell(r.w, "roomA.expected", r.temp, iv(0, 1000), &x));
    CHECK(qn_equal(qw_net(r.w), t, x, NULL) == QN_OK);
    CHECK(qw_run(r.w, 1000) == QW_ECONTRADICTION);
    CHECK(strstr(qw_error(r.w), "roomA.") != NULL);
    qw_free(r.w);
}

/* --- a test kernel for the host's rules ----------------------------------------------- */

enum { C_COUNT, C_PERIOD, C_STORE, C_NOISE, C_OTHER, C_NSLOTS };

typedef enum {
    MB_NONE, MB_WRITE_IN_PREDICT, MB_WRITE_INPUT, MB_BAD_TRANSFER, MB_ASSUME_STATE, MB_TELL, MB_READ_LINKLESS,
    MB_ASSUME_WIDE, MB_EVENT,
} Misbehave;

static Misbehave misbehave;

struct QkInstance { const QkHost *host; };

static const QkSlot counter_slots[C_NSLOTS] = {
    [C_COUNT]  = { "count",  QK_STATE, "",  0,  { Q_NINF, Q_INF } },
    [C_PERIOD] = { "period", QK_IN,    "s", -6, { 1, Q_INF } },
    [C_STORE]  = { "store",  QK_STOCK, "kg", -3, { Q_NINF, Q_INF } },
    [C_NOISE]  = { "noise",  QK_STATE, "",  0,  { Q_NINF, Q_INF } },
    [C_OTHER]  = { "other",  QK_IN,    "",  0,  { Q_NINF, Q_INF } },
};

static const QkTransfer counter_transfers[] = {
    { "make", QK_NONE, C_STORE, false },
};

static QkResult c_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    *out = i;
    return QK_OK;
}

static void c_destroy(QkInstance *i) { free(i); }

static QkResult c_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    const QkHost *h = i->host;
    int64_t period = h->read(call, C_PERIOD).lo, now = h->now(call);
    out->horizon = now + period;
    out->event_lo = out->event_hi = QK_NO_TIME;
    switch (misbehave) {
    case MB_WRITE_IN_PREDICT: h->write(call, C_COUNT, 7); break;
    case MB_ASSUME_STATE: h->assume(call, C_COUNT, iv(0, 1)); break;
    case MB_ASSUME_WIDE: h->assume(call, C_PERIOD, iv(0, 1000000000)); break;
    case MB_EVENT: out->event_lo = out->event_hi = now + period / 4; break;
    default: break;
    }
    return QK_OK;
}

static QkResult c_advance(QkInstance *i, QkCall *call, int64_t to)
{
    (void) to;
    const QkHost *h = i->host;
    h->write(call, C_COUNT, h->read(call, C_COUNT).lo + 1);
    h->transfer(call, 0, 5);
    QRCursor cur = h->random(call, "noise");
    h->write(call, C_NOISE, (int64_t) qr_below(&cur, 1000000));
    switch (misbehave) {
    case MB_WRITE_INPUT: h->write(call, C_PERIOD, 1); break;
    case MB_BAD_TRANSFER: h->transfer(call, 9, 1); break;
    case MB_TELL: h->tell(call, C_COUNT, iv(0, 5)); break;
    default: break;
    }
    return QK_OK;
}

static const QkKernel counter = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.counter",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = counter_slots,
    .nslots = C_NSLOTS,
    .transfers = counter_transfers,
    .ntransfers = 1,
    .create = c_create,
    .destroy = c_destroy,
    .predict = c_predict,
    .advance = c_advance,
};

static QWorld *counter_world(uint64_t seed, QwPhen *out)
{
    QWorld *w = qw_new(seed, -6);
    QwKind count, dur, mass;
    QwPhen c, other;
    OK(qw_kind(w, "count", "", 0, false, &count));
    OK(qw_kind(w, "duration", "s", -6, false, &dur));
    OK(qw_kind(w, "mass", "kg", -3, true, &mass));
    OK(qw_phenomenon(w, "ctr", &c));
    OK(qw_phenomenon(w, "env", &other));
    OK(qw_cell(w, "ctr.count", count, qi_point(0), NULL));
    OK(qw_cell(w, "ctr.period", dur, qi_point(1000000), NULL));   /* 1 s */
    OK(qw_cell(w, "ctr.store", mass, qi_point(0), NULL));
    OK(qw_cell(w, "ctr.noise", count, qi_point(0), NULL));
    OK(qw_cell(w, "env.other", count, qi_point(3), NULL));
    OK(qw_connect(w, c, "other", "env"));   /* not used by a slot path; "other" is a plain slot */
    *out = c;
    return w;
}

void test_host_counter(void)
{
    misbehave = MB_NONE;
    QwPhen c;
    QWorld *w = counter_world(5, &c);
    /* the slot "other" binds to ctr.other, which does not exist */
    CHECK(qw_attach(w, c, &counter) == QW_ENAME);
    qw_free(w);

    QWorld *a = qw_new(5, -6);
    QwKind count, dur, mass;
    OK(qw_kind(a, "count", "", 0, false, &count));
    OK(qw_kind(a, "duration", "s", -6, false, &dur));
    OK(qw_kind(a, "mass", "kg", -3, true, &mass));
    OK(qw_phenomenon(a, "ctr", &c));
    OK(qw_cell(a, "ctr.count", count, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.period", dur, qi_point(1000000), NULL));
    OK(qw_cell(a, "ctr.store", mass, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.noise", count, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.other", count, qi_point(3), NULL));
    OK(qw_attach(a, c, &counter));
    OK(qw_run(a, 10000000));
    CHECK_I64(qw_get(a, "ctr.count").lo, 10);
    CHECK_I64(qw_get(a, "ctr.store").lo, 50);
    CHECK_I64((int64_t) qw_boundaries(a), 10);
    qw_free(a);
}

static QWorld *counter_ok(uint64_t seed, QwPhen *c)
{
    QWorld *a = qw_new(seed, -6);
    QwKind count, dur, mass;
    OK(qw_kind(a, "count", "", 0, false, &count));
    OK(qw_kind(a, "duration", "s", -6, false, &dur));
    OK(qw_kind(a, "mass", "kg", -3, true, &mass));
    OK(qw_phenomenon(a, "ctr", c));
    OK(qw_cell(a, "ctr.count", count, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.period", dur, qi_point(1000000), NULL));
    OK(qw_cell(a, "ctr.store", mass, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.noise", count, qi_point(0), NULL));
    OK(qw_cell(a, "ctr.other", count, qi_point(3), NULL));
    OK(qw_attach(a, *c, &counter));
    return a;
}

void test_host_random(void)
{
    misbehave = MB_NONE;
    QwPhen c;
    QWorld *a = counter_ok(5, &c), *b = counter_ok(5, &c), *d = counter_ok(6, &c);
    int64_t seen[5];
    bool varied = false;
    for (int s = 1; s <= 5; s++) {
        OK(qw_run(a, (int64_t) s * 1000000));
        OK(qw_run(b, (int64_t) s * 1000000));
        OK(qw_run(d, (int64_t) s * 1000000));
        seen[s - 1] = qw_get(a, "ctr.noise").lo;
        CHECK_I64(qw_get(b, "ctr.noise").lo, seen[s - 1]);   /* same seed, same draws */
        if (s > 1 && seen[s - 1] != seen[s - 2]) varied = true;   /* each event draws anew */
    }
    CHECK(varied);
    CHECK(qw_hash(a) == qw_hash(b));
    CHECK(qw_hash(a) != qw_hash(d));
    qw_free(a);
    qw_free(b);
    qw_free(d);
}

void test_host_refusals(void)
{
    struct { Misbehave m; const char *msg; } cases[] = {
        { MB_WRITE_IN_PREDICT, "outside advance" },
        { MB_WRITE_INPUT, "not an output" },
        { MB_BAD_TRANSFER, "out of range" },
        { MB_ASSUME_STATE, "not an input" },
        { MB_TELL, "outside narrow" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        misbehave = cases[i].m;
        QwPhen c;
        QWorld *w = counter_ok(5, &c);
        CHECK(qw_run(w, 5000000) == QW_EKERNEL);
        CHECK(strstr(qw_error(w), cases[i].msg) != NULL);
        qw_free(w);
    }
    misbehave = MB_NONE;
}

void test_host_assumptions_and_events(void)
{
    /* a read during predict is an assumption: a changed input means a new prediction */
    misbehave = MB_NONE;
    QwPhen c;
    QWorld *w = counter_ok(5, &c);
    OK(qw_run(w, 500000));
    CHECK_I64((int64_t) qw_predicts(w, c), 1);
    OK(qw_input(w, "ctr.period", qi_point(1000001)));
    OK(qw_run(w, 600000));
    CHECK_I64((int64_t) qw_predicts(w, c), 2);
    qw_free(w);

    /* an explicit, wider assumption tolerates the change */
    misbehave = MB_ASSUME_WIDE;
    w = counter_ok(5, &c);
    OK(qw_run(w, 500000));
    OK(qw_input(w, "ctr.period", qi_point(1000001)));
    OK(qw_run(w, 600000));
    CHECK_I64((int64_t) qw_predicts(w, c), 1);
    qw_free(w);

    /* a predicted event sets the boundary */
    misbehave = MB_EVENT;
    w = counter_ok(5, &c);
    OK(qw_run(w, 1000000));
    CHECK_I64((int64_t) qw_boundaries(w), 4);   /* events every 0.25 s */
    qw_free(w);
    misbehave = MB_NONE;
}

/* --- a narrowing kernel ------------------------------------------------------------- */

enum { N_LOW, N_HIGH, N_NSLOTS };
static bool narrow_misbehave;

static const QkSlot nslots[N_NSLOTS] = {
    [N_LOW]  = { "low",  QK_IN, "", 0, { Q_NINF, Q_INF } },
    [N_HIGH] = { "high", QK_IN, "", 0, { Q_NINF, Q_INF } },
};

/* low <= high */
static void n_narrow(QkInstance *i, QkCall *call)
{
    const QkHost *h = i->host;
    QInterval lo = h->read(call, N_LOW), hi = h->read(call, N_HIGH);
    h->tell(call, N_LOW, iv(Q_NINF, hi.hi));
    h->tell(call, N_HIGH, iv(lo.lo, Q_INF));
    if (narrow_misbehave) h->write(call, N_LOW, 0);
}

static const QkKernel narrower = {
    .abi_version = QK_ABI_VERSION,
    .size = sizeof(QkKernel),
    .qr_version = QR_VERSION,
    .name = "test.order",
    .flags = QK_NARROW | QK_STATELESS | QK_PORTABLE,
    .slots = nslots,
    .nslots = N_NSLOTS,
    .create = c_create,
    .destroy = c_destroy,
    .narrow = n_narrow,
};

void test_host_narrow(void)
{
    for (int bad = 0; bad < 2; bad++) {
        narrow_misbehave = bad;
        QWorld *w = qw_new(1, -6);
        QwKind n;
        QwPhen p;
        OK(qw_kind(w, "n", "", 0, false, &n));
        OK(qw_phenomenon(w, "o", &p));
        OK(qw_cell(w, "o.low", n, iv(0, 100), NULL));
        OK(qw_cell(w, "o.high", n, iv(-50, 40), NULL));
        OK(qw_attach(w, p, &narrower));
        QwStatus s = qw_run(w, 1000);
        if (!bad) {
            CHECK(s == QW_OK);
            CHECK(qw_get(w, "o.low").hi == 40);
            CHECK(qw_get(w, "o.high").lo == 0);
        } else {
            CHECK(s == QW_EKERNEL);
            CHECK(strstr(qw_error(w), "outside advance") != NULL);
        }
        qw_free(w);
    }
    narrow_misbehave = false;
}

/* An alias is a second path for a cell of the same phenomenon (ADR 0016). */
void test_world_alias(void)
{
    Rooms plain = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    OK(qw_attach(plain.w, plain.wall, qk_conduction()));

    /* the same world, but the wall connects to a port of each room */
    QWorld *w = qw_new(1, -6);
    QwKind energy, temp, cap, cond, ratio;
    QwPhen a, b, wall;
    OK(qw_kind(w, "energy", "J", -3, true, &energy));
    OK(qw_kind(w, "temperature", "K", -6, false, &temp));
    OK(qw_kind(w, "capacity", "J/K", -3, false, &cap));
    OK(qw_kind(w, "conductance", "W/K", -3, false, &cond));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "roomA", &a));
    OK(qw_phenomenon(w, "roomB", &b));
    OK(qw_phenomenon(w, "wall", &wall));
    const char *names[2] = { "roomA", "roomB" };
    int64_t energies[2] = { 3031500000, 2931500000 };
    for (int i = 0; i < 2; i++) {
        const char *fields[3] = { "energy", "capacity", "temperature" };
        QwKind kinds[3] = { energy, cap, temp };
        QInterval bases[3] = { qi_point(energies[i]), qi_point(10000000), qi_top() };
        QnCell c[3];
        for (int k = 0; k < 3; k++) {
            char path[64], alias[64];
            snprintf(path, sizeof path, "%s.%s", names[i], fields[k]);
            snprintf(alias, sizeof alias, "%s.west.%s", names[i], fields[k]);
            OK(qw_cell(w, path, kinds[k], bases[k], &c[k]));
            OK(qw_alias(w, alias, path));
        }
        CHECK(qn_product(qw_net(w), c[0], c[1], c[2], NULL) == QN_OK);
    }
    OK(qw_cell(w, "wall.conductance", cond, qi_point(10000), NULL));
    OK(qw_cell(w, "wall.tolerance", ratio, qi_point(10000000), NULL));
    OK(qw_connect(w, wall, "hot", "roomA.west"));
    OK(qw_connect(w, wall, "cold", "roomB.west"));
    OK(qw_attach(w, wall, qk_conduction()));

    /* an alias is a name, not state: the hash is the hash of the world without aliases */
    CHECK(qw_hash(w) == qw_hash(plain.w));
    OK(qw_run(w, 1000000000));
    OK(qw_run(plain.w, 1000000000));
    CHECK(qw_hash(w) == qw_hash(plain.w));
    CHECK_I64((int64_t) qw_boundaries(w), 200);

    QnCell direct, through;
    OK(qw_find(w, "roomA.energy", &direct));
    OK(qw_find(w, "roomA.west.energy", &through));
    CHECK(direct == through);
    CHECK(!strcmp(qw_cell_path(w, through), "roomA.energy"));
    CHECK_I64(qw_get(w, "roomA.west.temperature").lo, qw_get(w, "roomA.temperature").lo);
    /* an input through an alias changes the cell */
    OK(qw_input(w, "roomA.west.capacity", qi_point(20000000)));
    CHECK_I64(qw_get(w, "roomA.capacity").lo, 20000000);
    CHECK(qw_input(w, "roomA.west.energy", qi_point(5)) == QW_ESTOCK);

    CHECK(qw_alias(w, "roomA.west.energy", "roomA.energy") == QW_EDUP);       /* the alias exists */
    CHECK(qw_alias(w, "roomA.capacity", "roomA.energy") == QW_EDUP);          /* the path of a cell */
    CHECK(qw_alias(w, "roomA.other", "roomA.missing") == QW_ENAME);
    CHECK(strstr(qw_error(w), "roomA.missing") != NULL);
    CHECK(qw_alias(w, "nowhere.x", "roomA.energy") == QW_ENAME);
    CHECK(qw_alias(w, "roomA", "roomA.energy") == QW_ENAME);
    CHECK(qw_alias(w, "roomB.stolen", "roomA.energy") == QW_EOWNER);          /* a view stays in its phenomenon */
    CHECK(qw_alias(w, "roomA.again", "roomA.west.energy") == QW_OK);          /* an alias of an alias names the cell */
    OK(qw_find(w, "roomA.again", &through));
    CHECK(direct == through);
    CHECK(qw_cell(w, "roomA.again", energy, qi_point(0), NULL) == QW_EDUP);   /* a cell cannot take the path of an alias */
    qw_free(w);
    qw_free(plain.w);
}

/* A derived cell is not state, and an anonymous kind has no name (ADR 0018). */
void test_world_derived(void)
{
    Rooms plain = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    Rooms r = make_rooms(1, 303.15, 293.15, 10000, 10000, 10, 10000000);
    QwKind dt, dt2, power, coarse;
    OK(qw_anonymous_kind(r.w, "K", -6, &dt));
    OK(qw_anonymous_kind(r.w, "K", -6, &dt2));
    OK(qw_anonymous_kind(r.w, "W", -3, &power));
    OK(qw_anonymous_kind(r.w, "K", -3, &coarse));
    CHECK(dt == dt2 && dt != power && dt != coarse);     /* one kind for one dimension and scale */
    CHECK(qw_anonymous_kind(r.w, "furlong", 0, &dt2) == QW_ETYPE);
    CHECK(qw_anonymous_kind(r.w, "K", 300, &dt2) == QW_ETYPE);
    QType t;
    OK(qw_kind_type(r.w, dt, &t));
    CHECK(t.kind == 0 && t.scale == -6);

    /* the wall's law by hand: power = conductance × (T hot − T cold) */
    QnCell diff, p, hot, cold, g;
    OK(qw_derived_cell(r.w, "wall.law.1", dt, qi_top(), &diff));
    OK(qw_derived_cell(r.w, "wall.law.2", power, qi_top(), &p));
    OK(qw_find(r.w, "roomA.temperature", &hot));
    OK(qw_find(r.w, "roomB.temperature", &cold));
    OK(qw_find(r.w, "wall.conductance", &g));
    CHECK(qn_difference(qw_net(r.w), diff, hot, cold, NULL) == QN_OK);    /* an anonymous kind fits each kind of its dimension */
    CHECK(qn_product(qw_net(r.w), p, g, diff, NULL) == QN_OK);
    OK(qw_attach(r.w, r.wall, qk_conduction()));
    OK(qw_attach(plain.w, plain.wall, qk_conduction()));

    /* a derived cell is not state: the hash is the hash of the world without it */
    CHECK(qw_hash(r.w) == qw_hash(plain.w));
    OK(qw_run(r.w, 0));
    CHECK_I64(qw_get(r.w, "wall.law.1").lo, 10000000);            /* 10 K */
    CHECK_I64(qw_get(r.w, "wall.law.2").lo, 100000);              /* 100 W */
    OK(qw_run(r.w, 1000000000));
    OK(qw_run(plain.w, 1000000000));
    CHECK(qw_hash(r.w) == qw_hash(plain.w));
    CHECK(qw_get(r.w, "wall.law.2").lo < 20000 && qw_get(r.w, "wall.law.2").lo > 10000);   /* the rooms came closer */

    CHECK(qw_input(r.w, "wall.law.1", qi_point(5)) == QW_EOWNER);
    CHECK(strstr(qw_error(r.w), "derived") != NULL);
    CHECK(qw_derived_cell(r.w, "wall.law.1", dt, qi_top(), NULL) == QW_EDUP);
    CHECK(qw_derived_cell(r.w, "wall.stock", r.energy, qi_point(0), NULL) == QW_ESTOCK);
    CHECK(qw_derived_cell(r.w, "nowhere.x", dt, qi_top(), NULL) == QW_ENAME);
    /* a constant: a derived cell with a point */
    QnCell k;
    OK(qw_derived_cell(r.w, "wall.law.3", coarse, qi_point(273150), &k));
    CHECK_I64(qw_get(r.w, "wall.law.3").hi, 273150);
    CHECK(qw_hash(r.w) == qw_hash(plain.w));
    qw_free(r.w);
    qw_free(plain.w);
}
