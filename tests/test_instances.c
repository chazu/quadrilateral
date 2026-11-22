#include <stdlib.h>
#include <string.h>

#include "quad/world.h"
#include "test.h"

/* Instances that start and end during a run (ADR 0013).
 *
 * The scenario: a building has fuel, ash and a temperature. A ramp kernel
 * raises the temperature to 800 K at 500 s and lowers it again, so the
 * building is at or above its ignition temperature of 600 K from 300 s to
 * 700 s. The instance "combustion(<building>)" takes 1000 quanta of fuel
 * when it starts, burns them into the building's ash, and gives the rest
 * back when it ends. */

#define OK(x) CHECK((x) == QW_OK)
#define SEC 1000000LL

struct QkInstance { const QkHost *host; bool counted; };

static int burn_live;      /* burn kernel instances that exist now */
static bool start_fail;    /* the start map fails */
static bool end_leak;      /* the end map leaves one quantum in the stock */

static QkResult make_instance(const QkHost *host, bool counted, QkInstance **out)
{
    QkInstance *i = malloc(sizeof *i);
    if (!i) return QK_ERROR;
    i->host = host;
    i->counted = counted;
    if (counted) burn_live++;
    *out = i;
    return QK_OK;
}

static QkResult plain_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    return make_instance(host, false, out);
}

static QkResult counted_create(const QkKernel *self, const QkHost *host, QkCall *call, QkInstance **out)
{
    (void) self;
    (void) call;
    return make_instance(host, true, out);
}

static void any_destroy(QkInstance *i)
{
    if (i->counted) burn_live--;
    free(i);
}

#define NO_LIMIT { Q_NINF, Q_INF }
#define KERNEL_HEAD .abi_version = QK_ABI_VERSION, .size = sizeof(QkKernel), .qr_version = QR_VERSION

/* --- test.ramp: the building's temperature, a function of the time -------------------- */

enum { R_TEMP, R_PERIOD };

static const QkSlot ramp_slots[] = {
    [R_TEMP]   = { "temperature", QK_OUT, "K", 0,  NO_LIMIT },
    [R_PERIOD] = { "period",      QK_IN,  "s", -6, { 1, Q_INF } },
};

static QkResult ramp_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    out->horizon = i->host->now(call) + i->host->read(call, R_PERIOD).lo;
    return QK_OK;
}

static QkResult ramp_advance(QkInstance *i, QkCall *call, int64_t to)
{
    int64_t t = to / SEC;
    i->host->write(call, R_TEMP, 300 + (t <= 500 ? t : t <= 1000 ? 1000 - t : 0));
    return QK_OK;
}

static const QkKernel ramp_kernel = {
    KERNEL_HEAD,
    .name = "test.ramp",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = ramp_slots,
    .nslots = 2,
    .create = plain_create,
    .destroy = any_destroy,
    .predict = ramp_predict,
    .advance = ramp_advance,
};

/* --- test.burn: the combustion's evolution kernel ------------------------------------------ */

enum { B_CHARGE, B_ASH, B_INTENSITY, B_NOISE };

static const QkSlot burn_slots[] = {
    [B_CHARGE]    = { "charge",    QK_STOCK, "kg", -3, NO_LIMIT },
    [B_ASH]       = { "site.ash",  QK_LINK,  "kg", -3, NO_LIMIT },
    [B_INTENSITY] = { "intensity", QK_STATE, "",   0,  NO_LIMIT },
    [B_NOISE]     = { "noise",     QK_STATE, "",   0,  NO_LIMIT },
};

static const QkTransfer burn_transfers[] = { { "burn", B_CHARGE, B_ASH, true } };

static QkResult burn_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    out->horizon = i->host->now(call) + 20 * SEC;
    return QK_OK;
}

/* Burns one quantum for each second, plus a seeded amount of 0 to 9. */
static QkResult burn_advance(QkInstance *i, QkCall *call, int64_t to)
{
    const QkHost *h = i->host;
    QRCursor cur = h->random(call, "flicker");
    int64_t flicker = qr_range(&cur, 0, 9);
    int64_t amount = (to - h->now(call)) / SEC + flicker, charge = h->read(call, B_CHARGE).lo;
    h->transfer(call, 0, amount < charge ? amount : charge);
    h->write(call, B_INTENSITY, h->read(call, B_INTENSITY).lo + 1);
    h->write(call, B_NOISE, flicker);
    return QK_OK;
}

static const QkKernel burn_kernel = {
    KERNEL_HEAD,
    .name = "test.burn",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = burn_slots,
    .nslots = 4,
    .transfers = burn_transfers,
    .ntransfers = 1,
    .create = counted_create,
    .destroy = any_destroy,
    .predict = burn_predict,
    .advance = burn_advance,
};

/* --- test.ignite and test.extinguish: the start map and the end map ------------------------- */

enum { S_FUEL, S_CHARGE, S_INTENSITY, S_FLAG, S_SPARK };

static const QkSlot ignite_slots[] = {
    [S_FUEL]      = { "site.fuel", QK_LINK,  "kg",    -3, NO_LIMIT },
    [S_CHARGE]    = { "charge",    QK_STOCK, "kg",    -3, NO_LIMIT },
    [S_INTENSITY] = { "intensity", QK_STATE, "",      0,  NO_LIMIT },
    [S_FLAG]      = { "flag",      QK_STATE, "atoms", 0,  NO_LIMIT },
    [S_SPARK]     = { "spark",     QK_STATE, "",      0,  NO_LIMIT },
};

static const QkTransfer ignite_transfers[] = { { "take", S_FUEL, S_CHARGE, true } };

static QkResult ignite_map(QkInstance *i, QkCall *call)
{
    i->host->transfer(call, 0, 1000);
    i->host->write(call, S_INTENSITY, 5);
    if (start_fail) return QK_ERROR;   /* after a transfer and a write, which the host must discard */
    i->host->write_atom(call, S_FLAG, (uint32_t) i->host->atom(call, S_FLAG, "true"));
    /* a map draws with the key and the epoch, so the order of two starts at
     * one boundary changes this value */
    QRCursor cur = i->host->random(call, "spark");
    i->host->write(call, S_SPARK, qr_range(&cur, 1, 1000000));
    return QK_OK;
}

static const QkKernel ignite_kernel = {
    KERNEL_HEAD,
    .name = "test.ignite",
    .flags = QK_MAP | QK_STATELESS | QK_PORTABLE,
    .slots = ignite_slots,
    .nslots = 5,
    .transfers = ignite_transfers,
    .ntransfers = 1,
    .create = plain_create,
    .destroy = any_destroy,
    .map = ignite_map,
};

enum { E_CHARGE, E_FUEL, E_NOISE };

static const QkSlot extinguish_slots[] = {
    [E_CHARGE] = { "charge",    QK_STOCK, "kg", -3, NO_LIMIT },
    [E_FUEL]   = { "site.fuel", QK_LINK,  "kg", -3, NO_LIMIT },
    [E_NOISE]  = { "noise",     QK_STATE, "",   0,  NO_LIMIT },
};

static const QkTransfer extinguish_transfers[] = { { "give", E_CHARGE, E_FUEL, true } };

static QkResult extinguish_map(QkInstance *i, QkCall *call)
{
    int64_t charge = i->host->read(call, E_CHARGE).lo;
    i->host->transfer(call, 0, end_leak ? charge - 1 : charge);
    i->host->write(call, E_NOISE, 77);   /* the host must discard this: a dormant instance holds no state */
    return QK_OK;
}

static const QkKernel extinguish_kernel = {
    KERNEL_HEAD,
    .name = "test.extinguish",
    .flags = QK_MAP | QK_STATELESS | QK_PORTABLE,
    .slots = extinguish_slots,
    .nslots = 3,
    .transfers = extinguish_transfers,
    .ntransfers = 1,
    .create = plain_create,
    .destroy = any_destroy,
    .map = extinguish_map,
};

/* --- test.smoke: an instance that depends on a combustion ------------------------------------ */

enum { M_SOURCE, M_DENSITY };

static const QkSlot smoke_slots[] = {
    [M_SOURCE]  = { "source.intensity", QK_IN,    "", 0, NO_LIMIT },
    [M_DENSITY] = { "density",          QK_STATE, "", 0, NO_LIMIT },
};

static QkResult smoke_predict(QkInstance *i, QkCall *call, QkPrediction *out)
{
    out->horizon = i->host->now(call) + 50 * SEC;
    return QK_OK;
}

static QkResult smoke_advance(QkInstance *i, QkCall *call, int64_t to)
{
    (void) to;
    i->host->write(call, M_DENSITY, i->host->read(call, M_SOURCE).lo);
    return QK_OK;
}

static const QkKernel smoke_kernel = {
    KERNEL_HEAD,
    .name = "test.smoke",
    .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = smoke_slots,
    .nslots = 2,
    .create = plain_create,
    .destroy = any_destroy,
    .predict = smoke_predict,
    .advance = smoke_advance,
};

/* --- the world ----------------------------------------------------------------------------- */

typedef struct { QWorld *w; QwKind mass, temp, count, dur; } Town;

static Town make_town(uint64_t seed)
{
    Town t;
    memset(&t, 0, sizeof t);
    t.w = qw_new(seed, -6);
    OK(qw_kind(t.w, "mass", "kg", -3, true, &t.mass));
    OK(qw_kind(t.w, "temperature", "K", 0, false, &t.temp));
    OK(qw_kind(t.w, "count", "", 0, false, &t.count));
    OK(qw_kind(t.w, "duration", "s", -6, false, &t.dur));
    return t;
}

static const char *path(const char *owner, const char *feature)
{
    static char buf[4][128];
    static int next;
    char *b = buf[next++ % 4];
    snprintf(b, sizeof buf[0], "%s.%s", owner, feature);
    return b;
}

/* A building with the condition cell "<name>.hot": temperature >= 600 K. */
static void add_building(Town *t, const char *name)
{
    QwPhen p;
    QnCell temp, hot;
    OK(qw_phenomenon(t->w, name, &p));
    OK(qw_cell(t->w, path(name, "fuel"), t->mass, qi_point(5000), NULL));
    OK(qw_cell(t->w, path(name, "ash"), t->mass, qi_point(0), NULL));
    OK(qw_cell(t->w, path(name, "temperature"), t->temp, qi_point(300), &temp));
    OK(qw_cell(t->w, path(name, "period"), t->dur, qi_point(10 * SEC), NULL));
    OK(qw_set_cell(t->w, path(name, "hot"), qw_boolean(t->w), 3, &hot));
    CHECK(qn_reify_in(qw_net(t->w), hot, temp, (QInterval) { 600, Q_INF }) == QN_OK);
    OK(qw_attach(t->w, p, &ramp_kernel));
}

static const char *fire_key(const char *building)
{
    static char buf[4][128];
    static int next;
    char *b = buf[next++ % 4];
    snprintf(b, sizeof buf[0], "combustion(%s)", building);
    return b;
}

/* The instance "combustion(<building>)". With `condition`, it exists while
 * the building is hot. */
static QwPhen add_fire(Town *t, const char *building, bool condition)
{
    const char *key = fire_key(building);
    QwPhen p = 0;
    OK(qw_instance(t->w, key, &p));
    OK(qw_cell(t->w, path(key, "charge"), t->mass, qi_point(0), NULL));
    OK(qw_cell(t->w, path(key, "intensity"), t->count, (QInterval) { 0, 100 }, NULL));
    OK(qw_cell(t->w, path(key, "noise"), t->count, qi_point(0), NULL));
    OK(qw_cell(t->w, path(key, "spark"), t->count, qi_point(0), NULL));
    OK(qw_set_cell(t->w, path(key, "flag"), qw_boolean(t->w), 3, NULL));
    OK(qw_connect(t->w, p, "site", building));
    OK(qw_attach(t->w, p, &burn_kernel));
    OK(qw_instance_maps(t->w, p, &ignite_kernel, &extinguish_kernel));
    if (condition) OK(qw_exists_when(t->w, p, path(building, "hot")));
    return p;
}

static int64_t point(QWorld *w, const char *p) { return qw_get(w, p).lo; }

static bool is_interval(QWorld *w, const char *p, int64_t lo, int64_t hi)
{
    QInterval v = qw_get(w, p);
    return v.lo == lo && v.hi == hi;
}

/* A dormant combustion holds no state. */
static void check_dormant(QWorld *w, const char *key)
{
    CHECK_I64(point(w, path(key, "charge")), 0);
    CHECK(is_interval(w, path(key, "intensity"), 0, 100));
    CHECK(is_interval(w, path(key, "noise"), 0, 0));
    CHECK(is_interval(w, path(key, "spark"), 0, 0));
    CHECK(qw_get_set(w, path(key, "flag")) == 3);
    CHECK(qw_get_set(w, path(key, "alive")) == 1);
}

/* --- a condition starts and ends an instance inside one run ---------------------------------- */

void test_instance_condition(void)
{
    start_fail = end_leak = false;
    burn_live = 0;
    Town a = make_town(7);
    add_building(&a, "b");
    QwPhen fire = add_fire(&a, "b", true);
    CHECK(!qw_alive(a.w, fire));
    CHECK(qw_alive(a.w, 0));
    check_dormant(a.w, "combustion(b)");
    CHECK_I64(burn_live, 0);   /* a dormant instance has no kernel instance */

    OK(qw_run(a.w, 1000 * SEC));   /* one call: the start and the end are inside it */
    CHECK_I64((int64_t) qw_starts(a.w), 1);
    CHECK_I64((int64_t) qw_ends(a.w), 1);
    CHECK(!qw_alive(a.w, fire));
    CHECK_I64(burn_live, 0);
    CHECK_I64(qw_total(a.w, a.mass), 5000);
    check_dormant(a.w, "combustion(b)");
    int64_t ash = point(a.w, "b.ash");
    CHECK(ash >= 410 && ash < 1000);   /* 41 steps of 10 s, each 10 to 19 quanta */
    CHECK_I64(point(a.w, "b.fuel"), 5000 - ash);

    /* the same inputs give the same hash, and another seed gives another fire */
    Town b = make_town(7), d = make_town(8);
    add_building(&b, "b");
    add_fire(&b, "b", true);
    add_building(&d, "b");
    add_fire(&d, "b", true);
    OK(qw_run(b.w, 1000 * SEC));
    OK(qw_run(d.w, 1000 * SEC));
    CHECK(qw_hash(a.w) == qw_hash(b.w));
    CHECK(qw_hash(a.w) != qw_hash(d.w));

    /* step by step: the instance is alive from 300 s to 700 s, and every
     * total is exact at every boundary */
    Town c = make_town(7);
    add_building(&c, "b");
    QwPhen cf = add_fire(&c, "b", true);
    for (int64_t s = 10; s <= 1000; s += 10) {
        OK(qw_run(c.w, s * SEC));
        bool alive = s >= 300 && s <= 700;
        CHECK(qw_alive(c.w, cf) == alive);
        CHECK(qw_get_set(c.w, "combustion(b).alive") == (alive ? 2u : 1u));
        CHECK_I64(burn_live, alive);
        CHECK_I64(qw_total(c.w, c.mass), 5000);
        if (alive) {
            CHECK_I64(point(c.w, "combustion(b).intensity"), 5 + (s - 300) / 10);
            CHECK(qw_get_set(c.w, "combustion(b).flag") == 2);
            CHECK_I64(point(c.w, "b.fuel"), 4000);
        } else {
            check_dormant(c.w, "combustion(b)");
        }
    }
    CHECK(qw_hash(c.w) == qw_hash(a.w));

    qw_free(a.w);
    qw_free(b.w);
    qw_free(c.w);
    qw_free(d.w);
    CHECK_I64(burn_live, 0);
}

/* --- explicit calls, an instance declared during the run, and a second life ------------------ */

static QWorld *explicit_lives(uint64_t seed, int64_t first[10], int64_t second[10])
{
    Town t = make_town(seed);
    add_building(&t, "b");
    OK(qw_run(t.w, 100 * SEC));

    QwPhen fire = add_fire(&t, "b", false);   /* new cells during the run */
    CHECK(!qw_alive(t.w, fire));
    CHECK(qw_end(t.w, fire) == QW_ELIFE);
    CHECK_I64(qw_total(t.w, t.mass), 5000);
    OK(qw_run(t.w, 150 * SEC));   /* a dormant instance does nothing */
    check_dormant(t.w, "combustion(b)");
    CHECK_I64(point(t.w, "b.ash"), 0);

    int64_t now = 150;
    for (int life = 0; life < 2; life++) {
        uint64_t epoch = qn_epoch(qw_net(t.w));
        OK(qw_start(t.w, fire));
        CHECK(qw_alive(t.w, fire));
        CHECK(qn_epoch(qw_net(t.w)) == epoch + 1);
        CHECK(qw_start(t.w, fire) == QW_ELIFE);
        CHECK_I64(qw_total(t.w, t.mass), 5000);
        /* the start does not depend on the earlier life */
        CHECK_I64(point(t.w, "combustion(b).charge"), 1000);
        CHECK_I64(point(t.w, "combustion(b).intensity"), 5);
        CHECK_I64(point(t.w, "combustion(b).noise"), 0);
        CHECK(point(t.w, "combustion(b).spark") > 0);
        for (int i = 0; i < 10; i++) {
            now += 10;
            OK(qw_run(t.w, now * SEC));
            (life ? second : first)[i] = point(t.w, "combustion(b).noise");
            CHECK_I64(qw_total(t.w, t.mass), 5000);
        }
        CHECK_I64(point(t.w, "combustion(b).intensity"), 15);
        OK(qw_end(t.w, fire));
        CHECK(!qw_alive(t.w, fire));
        check_dormant(t.w, "combustion(b)");
        CHECK_I64(qw_total(t.w, t.mass), 5000);
        CHECK_I64(point(t.w, "b.fuel") + point(t.w, "b.ash"), 5000);
    }
    CHECK_I64((int64_t) qw_starts(t.w), 2);
    CHECK_I64((int64_t) qw_ends(t.w), 2);
    return t.w;
}

void test_instance_explicit(void)
{
    start_fail = end_leak = false;
    int64_t a1[10], a2[10], b1[10], b2[10];
    QWorld *a = explicit_lives(3, a1, a2), *b = explicit_lives(3, b1, b2);
    CHECK(qw_hash(a) == qw_hash(b));
    CHECK(!memcmp(a1, b1, sizeof a1) && !memcmp(a2, b2, sizeof a2));
    /* the event count continues, so the second life has new draws */
    CHECK(memcmp(a1, a2, sizeof a1) != 0);
    qw_free(a);
    qw_free(b);
}

/* --- identity does not depend on the order of declaration ------------------------------------ */

void test_instance_order_independence(void)
{
    start_fail = end_leak = false;
    Town x = make_town(11), y = make_town(11);
    add_building(&x, "b1");
    add_building(&x, "b2");
    add_fire(&x, "b1", true);
    add_fire(&x, "b2", true);

    add_building(&y, "b2");
    add_fire(&y, "b2", true);
    add_building(&y, "b1");
    add_fire(&y, "b1", true);

    QnCell cx, cy;
    OK(qw_find(x.w, "combustion(b1).noise", &cx));
    OK(qw_find(y.w, "combustion(b1).noise", &cy));
    CHECK(cx != cy);   /* the cell indices differ */
    CHECK(qw_hash(x.w) == qw_hash(y.w));

    bool differ = false;
    for (int64_t s = 50; s <= 1000; s += 50) {
        OK(qw_run(x.w, s * SEC));
        OK(qw_run(y.w, s * SEC));
        CHECK(qw_hash(x.w) == qw_hash(y.w));
        CHECK_I64(point(x.w, "combustion(b1).noise"), point(y.w, "combustion(b1).noise"));
        CHECK_I64(point(x.w, "combustion(b2).noise"), point(y.w, "combustion(b2).noise"));
        CHECK_I64(point(x.w, "combustion(b1).spark"), point(y.w, "combustion(b1).spark"));
        CHECK_I64(point(x.w, "combustion(b2).spark"), point(y.w, "combustion(b2).spark"));
        if (point(x.w, "b1.ash") != point(x.w, "b2.ash")) differ = true;
    }
    CHECK(differ);   /* each key has its own random stream */
    CHECK_I64((int64_t) qw_starts(x.w), 2);
    CHECK_I64((int64_t) qw_ends(y.w), 2);
    CHECK_I64(qw_total(x.w, x.mass), 10000);
    qw_free(x.w);
    qw_free(y.w);
}

/* --- an instance whose participant is an instance --------------------------------------------- */

static QwPhen add_smoke(Town *t, bool condition)
{
    const char *key = "smoke(combustion(b))";
    QwPhen p = 0;
    OK(qw_instance(t->w, key, &p));
    OK(qw_cell(t->w, path(key, "density"), t->count, qi_point(0), NULL));
    OK(qw_connect(t->w, p, "source", "combustion(b)"));
    OK(qw_attach(t->w, p, &smoke_kernel));
    if (condition) OK(qw_exists_when(t->w, p, "combustion(b).alive"));
    return p;
}

void test_instance_dependents(void)
{
    start_fail = end_leak = false;
    /* declared before its participant's condition holds; the smoke exists
     * while the combustion is alive */
    Town t = make_town(5);
    add_building(&t, "b");
    QwPhen fire = add_fire(&t, "b", true), smoke = add_smoke(&t, true);
    qw_log_changes(t.w, true);
    for (int64_t s = 10; s <= 1000; s += 10) {
        OK(qw_run(t.w, s * SEC));
        bool alive = s >= 300 && s <= 700;
        /* both change at the same boundary: the smoke starts in a second
         * round, and it ends before the combustion */
        CHECK(qw_alive(t.w, fire) == alive);
        CHECK(qw_alive(t.w, smoke) == alive);
        CHECK_I64((int64_t) qw_starts(t.w), s >= 300 ? 2 : 0);
        CHECK_I64((int64_t) qw_ends(t.w), s > 700 ? 2 : 0);
        if (!alive) CHECK_I64(point(t.w, "smoke(combustion(b)).density"), 0);
    }
    OK(qw_run(t.w, 1000 * SEC));
    CHECK_I64(qw_total(t.w, t.mass), 5000);

    /* the order inside the boundary: the epochs of the changes of "alive" */
    QnCell fa, sa;
    OK(qw_find(t.w, "combustion(b).alive", &fa));
    OK(qw_find(t.w, "smoke(combustion(b)).alive", &sa));
    uint64_t fire_epoch[2] = { 0, 0 }, smoke_epoch[2] = { 0, 0 };
    size_t nlog;
    const QwChange *log = qw_changes(t.w, &nlog);
    for (size_t i = 0; i < nlog; i++) {
        if (log[i].cell == fa) fire_epoch[log[i].after.lo == 1] = log[i].epoch;
        if (log[i].cell == sa) smoke_epoch[log[i].after.lo == 1] = log[i].epoch;
    }
    CHECK(fire_epoch[0] && smoke_epoch[0] == fire_epoch[0] + 1);   /* the start: the participant first */
    CHECK(smoke_epoch[1] && fire_epoch[1] == smoke_epoch[1] + 1);   /* the end: the dependent first */
    qw_free(t.w);

    /* An instance does not start in the round in which its participant
     * ends. The embers exist while the building is cool, but they need the
     * combustion. At 710 s the building is cool again and the combustion is
     * still alive, but it ends in that round. */
    Town c = make_town(5);
    add_building(&c, "b");
    add_fire(&c, "b", true);
    QnCell temp, cool;
    QwPhen embers;
    OK(qw_find(c.w, "b.temperature", &temp));
    OK(qw_set_cell(c.w, "b.cool", qw_boolean(c.w), 3, &cool));
    CHECK(qn_reify_in(qw_net(c.w), cool, temp, (QInterval) { Q_NINF, 599 }) == QN_OK);
    OK(qw_instance(c.w, "embers(combustion(b))", &embers));
    OK(qw_connect(c.w, embers, "source", "combustion(b)"));
    OK(qw_exists_when(c.w, embers, "b.cool"));
    OK(qw_run(c.w, 1000 * SEC));
    CHECK(!qw_alive(c.w, embers));
    CHECK_I64((int64_t) qw_starts(c.w), 1);
    CHECK_I64((int64_t) qw_ends(c.w), 1);
    qw_free(c.w);

    /* explicit calls keep the same order */
    Town e = make_town(5);
    add_building(&e, "b");
    fire = add_fire(&e, "b", false);
    smoke = add_smoke(&e, false);
    uint64_t h = qw_hash(e.w);
    CHECK(qw_start(e.w, smoke) == QW_ELIFE);   /* its participant is dormant */
    CHECK(strstr(qw_error(e.w), "combustion(b)") != NULL);
    CHECK(qw_hash(e.w) == h);
    OK(qw_start(e.w, fire));
    OK(qw_start(e.w, smoke));
    OK(qw_run(e.w, 100 * SEC));
    CHECK(point(e.w, "smoke(combustion(b)).density") > 5);
    h = qw_hash(e.w);
    CHECK(qw_end(e.w, fire) == QW_ELIFE);   /* the smoke connects to it */
    CHECK(strstr(qw_error(e.w), "smoke(combustion(b))") != NULL);
    CHECK(qw_hash(e.w) == h);
    CHECK(qw_alive(e.w, fire));
    OK(qw_end(e.w, smoke));
    OK(qw_end(e.w, fire));
    CHECK_I64(qw_total(e.w, e.mass), 5000);
    qw_free(e.w);
}

/* --- a failed start or end changes nothing ----------------------------------------------------- */

void test_instance_failures(void)
{
    start_fail = end_leak = false;
    burn_live = 0;
    Town t = make_town(9), twin = make_town(9);   /* the twin has no failed attempts */
    add_building(&t, "b");
    add_building(&twin, "b");
    QwPhen fire = add_fire(&t, "b", false), twin_fire = add_fire(&twin, "b", false);
    qw_log_changes(t.w, true);
    OK(qw_run(t.w, 50 * SEC));
    OK(qw_run(twin.w, 50 * SEC));
    OK(qw_start(twin.w, twin_fire));
    OK(qw_run(twin.w, 150 * SEC));
    OK(qw_end(twin.w, twin_fire));
    size_t n, n0;
    qw_changes(t.w, &n0);
    uint64_t h = qw_hash(t.w);

    start_fail = true;
    CHECK(qw_start(t.w, fire) == QW_EKERNEL);
    start_fail = false;
    CHECK(!qw_alive(t.w, fire));
    CHECK_I64(burn_live, 0);
    CHECK(qw_hash(t.w) == h);
    qw_changes(t.w, &n);
    CHECK(n == n0);

    OK(qw_start(t.w, fire));
    CHECK_I64(point(t.w, "combustion(b).charge"), 1000);   /* not also the transfer of the failed start */
    OK(qw_run(t.w, 100 * SEC));
    h = qw_hash(t.w);
    qw_changes(t.w, &n0);
    end_leak = true;
    CHECK(qw_end(t.w, fire) == QW_ESTOCK);
    end_leak = false;
    CHECK(strstr(qw_error(t.w), "combustion(b).charge") != NULL);
    CHECK(qw_alive(t.w, fire));
    CHECK_I64(burn_live, 1);
    CHECK(qw_hash(t.w) == h);
    qw_changes(t.w, &n);
    CHECK(n == n0);
    OK(qw_run(t.w, 150 * SEC));   /* the run continues, as if nobody had tried */
    OK(qw_end(t.w, fire));
    CHECK_I64(qw_total(t.w, t.mass), 5000);
    CHECK(qw_hash(t.w) == qw_hash(twin.w));
    qw_free(t.w);
    qw_free(twin.w);
}

/* --- conditions that do not settle ---------------------------------------------------------------- */

enum { L_POOL, L_HELD };

static const QkSlot leech_slots[] = {
    [L_POOL] = { "pool.amount", QK_LINK,  "kg", -3, NO_LIMIT },
    [L_HELD] = { "held",        QK_STOCK, "kg", -3, NO_LIMIT },
};

static const QkTransfer leech_take[] = { { "take", L_POOL, L_HELD, true } };
static const QkTransfer leech_give[] = { { "give", L_HELD, L_POOL, true } };

static QkResult leech_take_map(QkInstance *i, QkCall *call)
{
    i->host->transfer(call, 0, 50);
    return QK_OK;
}

static QkResult leech_give_map(QkInstance *i, QkCall *call)
{
    i->host->transfer(call, 0, i->host->read(call, L_HELD).lo);
    return QK_OK;
}

static const QkKernel leech_take_kernel = {
    KERNEL_HEAD, .name = "test.leech.take", .flags = QK_MAP | QK_STATELESS | QK_PORTABLE,
    .slots = leech_slots, .nslots = 2, .transfers = leech_take, .ntransfers = 1,
    .create = plain_create, .destroy = any_destroy, .map = leech_take_map,
};

static const QkKernel leech_give_kernel = {
    KERNEL_HEAD, .name = "test.leech.give", .flags = QK_MAP | QK_STATELESS | QK_PORTABLE,
    .slots = leech_slots, .nslots = 2, .transfers = leech_give, .ntransfers = 1,
    .create = plain_create, .destroy = any_destroy, .map = leech_give_map,
};

/* The leech exists while the pool is rich, and it empties the pool when it
 * starts. So its own start makes its condition false. */
void test_instance_oscillation(void)
{
    Town t = make_town(1);
    QwPhen pool, leech;
    QnCell amount, rich;
    OK(qw_phenomenon(t.w, "pool", &pool));
    OK(qw_cell(t.w, "pool.amount", t.mass, qi_point(50), &amount));
    OK(qw_set_cell(t.w, "pool.rich", qw_boolean(t.w), 3, &rich));
    CHECK(qn_reify_in(qw_net(t.w), rich, amount, (QInterval) { 10, Q_INF }) == QN_OK);
    OK(qw_instance(t.w, "leech(pool)", &leech));
    OK(qw_cell(t.w, "leech(pool).held", t.mass, qi_point(0), NULL));
    OK(qw_connect(t.w, leech, "pool", "pool"));
    OK(qw_instance_maps(t.w, leech, &leech_take_kernel, &leech_give_kernel));
    OK(qw_exists_when(t.w, leech, "pool.rich"));

    CHECK(qw_run(t.w, 0) == QW_ELIFE);
    CHECK(strstr(qw_error(t.w), "leech(pool)") != NULL);
    CHECK_I64((int64_t) qw_starts(t.w), 1);   /* the start happened; the second change is the error */
    CHECK_I64((int64_t) qw_ends(t.w), 0);
    CHECK_I64(qw_total(t.w, t.mass), 50);
    qw_free(t.w);

    /* an unknown condition changes nothing */
    Town u = make_town(1);
    QwPhen thing;
    OK(qw_phenomenon(u.w, "pool", &pool));
    OK(qw_set_cell(u.w, "pool.maybe", qw_boolean(u.w), 3, NULL));
    OK(qw_instance(u.w, "thing(pool)", &thing));
    OK(qw_exists_when(u.w, thing, "pool.maybe"));
    OK(qw_run(u.w, 0));
    CHECK(!qw_alive(u.w, thing));
    OK(qw_input_set(u.w, "pool.maybe", 2));
    OK(qw_run(u.w, 0));   /* the condition is checked when a run begins */
    CHECK(qw_alive(u.w, thing));
    OK(qw_input_set(u.w, "pool.maybe", 3));
    OK(qw_run(u.w, 0));
    CHECK(qw_alive(u.w, thing));
    OK(qw_input_set(u.w, "pool.maybe", 1));
    OK(qw_run(u.w, 0));
    CHECK(!qw_alive(u.w, thing));
    CHECK_I64((int64_t) qw_starts(u.w), 1);
    CHECK_I64((int64_t) qw_ends(u.w), 1);
    qw_free(u.w);
}

/* --- propagator groups of an instance and of its models ------------------------------------------- */

void test_instance_groups(void)
{
    Town t = make_town(1);
    QwPhen g;
    QnCell x, y, u, v;
    OK(qw_instance(t.w, "gadget(1,2)", &g));
    OK(qw_cell(t.w, "gadget(1,2).x", t.count, qi_point(3), &x));
    OK(qw_cell(t.w, "gadget(1,2).y", t.count, qi_top(), &y));
    OK(qw_build_begin(t.w, g));
    CHECK(qn_equal(qw_net(t.w), y, x, NULL) == QN_OK);
    OK(qw_model(t.w, g, "m"));
    OK(qw_model(t.w, g, "n"));
    OK(qw_cell(t.w, "gadget(1,2).m.u", t.count, qi_point(4), &u));
    OK(qw_cell(t.w, "gadget(1,2).m.v", t.count, qi_top(), &v));
    OK(qw_model_begin(t.w, g, "m"));
    CHECK(qn_equal(qw_net(t.w), v, u, NULL) == QN_OK);
    qw_model_end(t.w);
    qw_build_end(t.w);
    CHECK(qw_build_begin(t.w, 99) == QW_ENAME);

    for (int life = 0; life < 2; life++) {
        OK(qw_run(t.w, qw_now(t.w)));
        CHECK(is_interval(t.w, "gadget(1,2).y", Q_NINF, Q_INF));   /* dormant: no propagator runs */
        CHECK(is_interval(t.w, "gadget(1,2).m.v", Q_NINF, Q_INF));
        CHECK(qw_switch(t.w, g, "n", &ignite_kernel) == QW_ELIFE);
        OK(qw_start(t.w, g));
        CHECK_I64(point(t.w, "gadget(1,2).y"), 3);
        CHECK_I64(point(t.w, "gadget(1,2).m.v"), 4);
        OK(qw_end(t.w, g));
    }
    qw_free(t.w);
}

/* --- the change log ------------------------------------------------------------------------------- */

void test_instance_changelog(void)
{
    start_fail = end_leak = false;
    Town t = make_town(7);
    add_building(&t, "b");
    add_fire(&t, "b", true);
    QNet *net = qw_net(t.w);
    size_t ncells = qn_ncells(net);
    QInterval *state = malloc(ncells * sizeof *state);
    for (size_t c = 0; c < ncells; c++) state[c] = qn_base(net, (QnCell) c);
    QnCell alive, noise;
    OK(qw_find(t.w, "combustion(b).alive", &alive));
    OK(qw_find(t.w, "combustion(b).noise", &noise));

    qw_log_changes(t.w, true);
    OK(qw_run(t.w, 1000 * SEC));
    size_t n;
    const QwChange *e = qw_changes(t.w, &n);
    int alive_changes = 0;
    bool noise_reset = false;
    for (size_t i = 0; i < n; i++) {
        if (i) CHECK(e[i - 1].epoch < e[i].epoch || (e[i - 1].epoch == e[i].epoch && e[i - 1].cell < e[i].cell));
        CHECK(e[i].before.lo == state[e[i].cell].lo && e[i].before.hi == state[e[i].cell].hi);
        if (e[i].kind == QW_CHANGE_STOCK) CHECK_I64(e[i].after.lo, e[i].before.lo + e[i].amount);
        if (e[i].cell == alive) {
            /* false to true at the start, true to false at the end */
            CHECK(e[i].kind == QW_CHANGE_VALUE);
            CHECK_I64(e[i].before.lo, alive_changes == 0 ? 1 : 2);
            CHECK_I64(e[i].after.lo, alive_changes == 0 ? 2 : 1);
            alive_changes++;
        }
        /* the end is one epoch: the alive cell and the returned cells together */
        if (e[i].cell == noise && alive_changes == 2 && e[i].after.lo == 0 && e[i].after.hi == 0) noise_reset = true;
        state[e[i].cell] = e[i].after;
    }
    CHECK_I64(alive_changes, 2);
    CHECK(noise_reset);
    /* the log reproduces the final state */
    for (size_t c = 0; c < ncells; c++) {
        QInterval b = qn_base(net, (QnCell) c);
        CHECK(b.lo == state[c].lo && b.hi == state[c].hi);
    }
    free(state);
    qw_free(t.w);
}

/* --- errors ----------------------------------------------------------------------------------------- */

enum { W_ALIVE };

static const QkSlot writer_slots[] = { [W_ALIVE] = { "alive", QK_OUT, "atoms", 0, NO_LIMIT } };

static const QkKernel writer_kernel = {
    KERNEL_HEAD, .name = "test.writer", .flags = QK_EVOLVE | QK_STATELESS | QK_PORTABLE,
    .slots = writer_slots, .nslots = 1,
    .create = plain_create, .destroy = any_destroy, .predict = smoke_predict, .advance = smoke_advance,
};

void test_instance_errors(void)
{
    Town t = make_town(1);
    add_building(&t, "b");
    QwPhen fire = add_fire(&t, "b", true), p, q;

    /* names */
    CHECK(qw_instance(t.w, "combustion(b)", &p) == QW_EDUP);
    CHECK(qw_instance(t.w, "b", &p) == QW_EDUP);
    CHECK(qw_instance(t.w, "a b", &p) == QW_ENAME);
    CHECK(qw_instance(t.w, "a.b", &p) == QW_ENAME);
    OK(qw_phenomenon(t.w, "pair(b,b)", &p));
    CHECK(qw_cell(t.w, "b.alive", t.count, qi_point(0), NULL) == QW_ENAME);
    CHECK(qw_cell(t.w, "combustion(b).alive", t.count, qi_point(0), NULL) == QW_ENAME);
    OK(qw_cell(t.w, "b.model.alive", t.count, qi_point(0), NULL));   /* only the feature directly under the name */

    /* a stock of an instance starts at zero */
    CHECK(qw_cell(t.w, "combustion(b).extra", t.mass, qi_point(1), NULL) == QW_ESTOCK);

    /* only the host writes the alive cell */
    CHECK(qw_input_set(t.w, "combustion(b).alive", 2) == QW_EOWNER);
    OK(qw_instance(t.w, "writer(b)", &q));
    CHECK(qw_attach(t.w, q, &writer_kernel) == QW_EOWNER);

    /* conditions */
    CHECK(qw_exists_when(t.w, fire, "b.hot") == QW_EDUP);
    CHECK(qw_exists_when(t.w, q, "b.nothing") == QW_ENAME);
    CHECK(qw_exists_when(t.w, q, "b.temperature") == QW_ETYPE);
    CHECK(qw_exists_when(t.w, p, "b.hot") == QW_ELIFE);   /* not an instance */
    CHECK(qw_exists_when(t.w, 99, "b.hot") == QW_ENAME);

    /* explicit calls */
    CHECK(qw_start(t.w, fire) == QW_ELIFE);   /* it has a condition */
    CHECK(qw_end(t.w, fire) == QW_ELIFE);
    CHECK(qw_start(t.w, p) == QW_ELIFE);
    CHECK(qw_start(t.w, 99) == QW_ENAME);
    CHECK(qw_end(t.w, q) == QW_ELIFE);        /* dormant */

    /* maps */
    CHECK(qw_instance_maps(t.w, q, &burn_kernel, NULL) == QW_EKERNEL);
    CHECK(qw_instance_maps(t.w, p, NULL, NULL) == QW_ELIFE);
    OK(qw_instance_maps(t.w, q, NULL, NULL));

    /* a phenomenon that is not an instance cannot depend on an instance */
    QwPhen watcher;
    OK(qw_phenomenon(t.w, "watcher", &watcher));
    OK(qw_cell(t.w, "watcher.density", t.count, qi_point(0), NULL));
    OK(qw_connect(t.w, watcher, "source", "combustion(b)"));
    CHECK(qw_attach(t.w, watcher, &smoke_kernel) == QW_ELIFE);

    CHECK(!strcmp(qw_status_str(QW_ELIFE), "instance in the wrong state"));
    CHECK(qw_atom(t.w, qw_boolean(t.w), "true") == 1 && qw_atom(t.w, qw_boolean(t.w), "false") == 0);
    qw_free(t.w);
}
