#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quad/kernels.h"
#include "quad/world.h"
#include "test.h"

/* Snapshot and restore of a world in the middle of a run (ADR 0004 rule 8,
 * quad-5xe.1). A run that a restore continues gives the same state hash at
 * each time as a run with no stop. */

#define OK(x) CHECK((x) == QW_OK)
#define SEC(t) ((int64_t) (t) * 1000000)

static const QInterval ANY = { Q_NINF, Q_INF };

/* --- the world of phase 4: two rooms and a wall --------------------------------------------- */

typedef struct { QWorld *w; QwKind energy, temp, cap, cond, ratio; } Kinds;

static Kinds new_world(uint64_t seed)
{
    Kinds k;
    memset(&k, 0, sizeof k);
    k.w = qw_new(seed, -6);
    OK(qw_kind(k.w, "energy", "J", -3, true, &k.energy));
    OK(qw_kind(k.w, "temperature", "K", -6, false, &k.temp));
    OK(qw_kind(k.w, "capacity", "J/K", -3, false, &k.cap));
    OK(qw_kind(k.w, "conductance", "W/K", -3, false, &k.cond));
    OK(qw_kind(k.w, "ratio", "", -9, false, &k.ratio));
    return k;
}

static void room_cells(Kinds *k, const char *prefix, int64_t cap_mJK, int64_t energy_mJ)
{
    char path[128];
    QnCell e, c, t;
    snprintf(path, sizeof path, "%s.energy", prefix);
    OK(qw_cell(k->w, path, k->energy, qi_point(energy_mJ), &e));
    snprintf(path, sizeof path, "%s.capacity", prefix);
    OK(qw_cell(k->w, path, k->cap, qi_point(cap_mJK), &c));
    snprintf(path, sizeof path, "%s.temperature", prefix);
    OK(qw_derived_cell(k->w, path, k->temp, qi_top(), &t));
    CHECK(qn_product(qw_net(k->w), e, c, t, NULL) == QN_OK);
}

static QWorld *two_rooms(uint64_t seed)
{
    Kinds k = new_world(seed);
    QwPhen a, b, wall;
    const int64_t cap = 10000 * 1000;
    OK(qw_phenomenon(k.w, "roomA", &a));
    OK(qw_phenomenon(k.w, "roomB", &b));
    OK(qw_phenomenon(k.w, "wall", &wall));
    room_cells(&k, "roomA", cap, cap / 100 * 30315);
    room_cells(&k, "roomB", cap, cap / 100 * 29315);
    OK(qw_cell(k.w, "wall.conductance", k.cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(k.w, "wall.tolerance", k.ratio, qi_point(10000000), NULL));
    OK(qw_connect(k.w, wall, "hot", "roomA"));
    OK(qw_connect(k.w, wall, "cold", "roomB"));
    OK(qw_attach(k.w, wall, qk_conduction()));
    return k.w;
}

/* Runs `ref` and `w` to each time and checks that they have the same hash and the same count of boundaries. */
static void same_from(QWorld *ref, QWorld *w, int from, int to, int step)
{
    for (int t = from; t <= to; t += step) {
        OK(qw_run(ref, SEC(t)));
        OK(qw_run(w, SEC(t)));
        CHECK(qw_hash(ref) == qw_hash(w));
        CHECK(qw_boundaries(ref) == qw_boundaries(w));
    }
}

void test_snapshot_two_rooms(void)
{
    /* a run with no stop, and a run that a new world continues from a snapshot at 500 s */
    QWorld *ref = two_rooms(1), *first = two_rooms(1), *second = two_rooms(1);
    void *snap = NULL;
    size_t len = 0;
    OK(qw_run(ref, SEC(500)));
    OK(qw_run(first, SEC(500)));
    OK(qw_snapshot(first, &snap, &len));
    CHECK(snap != NULL && len > 0);
    CHECK(qw_hash(second) != qw_hash(first));
    OK(qw_restore(second, snap, len));
    CHECK(qw_hash(second) == qw_hash(first));
    CHECK_I64(qw_get(second, "roomA.temperature").lo, qw_get(first, "roomA.temperature").lo);     /* the laws ran */
    same_from(ref, second, 600, 1000, 100);
    CHECK(qw_advances(ref) == qw_advances(second));

    /* the same world goes back: after the restore it gives the same run again */
    uint64_t at1000 = qw_hash(first);
    OK(qw_run(first, SEC(1000)));
    at1000 = qw_hash(first);
    OK(qw_restore(first, snap, len));
    CHECK(qw_hash(first) != at1000);
    OK(qw_run(first, SEC(1000)));
    CHECK(qw_hash(first) == at1000);
    free(snap);

    /* a snapshot at a time that is not the end of a step of the wall: the prediction is state */
    QWorld *third = two_rooms(1), *fourth = two_rooms(1);
    OK(qw_run(third, SEC(537)));
    OK(qw_snapshot(third, &snap, &len));
    OK(qw_restore(fourth, snap, len));
    OK(qw_run(third, SEC(1000)));
    OK(qw_run(fourth, SEC(1000)));
    CHECK(qw_hash(third) == qw_hash(fourth) && qw_boundaries(third) == qw_boundaries(fourth));
    CHECK(qw_predicts(third, 2) == qw_predicts(fourth, 2));        /* the wall did not predict again after the restore */
    free(snap);
    /* a snapshot before the run */
    QWorld *fifth = two_rooms(1), *sixth = two_rooms(1);
    OK(qw_snapshot(fifth, &snap, &len));
    OK(qw_run(sixth, SEC(100)));
    OK(qw_restore(sixth, snap, len));
    CHECK(qw_hash(fifth) == qw_hash(sixth));
    same_from(fifth, sixth, 100, 300, 100);
    free(snap);
    qw_free(ref);
    qw_free(first);
    qw_free(second);
    qw_free(third);
    qw_free(fourth);
    qw_free(fifth);
    qw_free(sixth);
}

/* --- the world of phase 5: a room with two models ------------------------------------------- */

typedef struct { QWorld *w; QwPhen b; QkKernel *diff, *refine, *abstract; } Mixed;

static Mixed mixed(uint64_t seed, unsigned n)
{
    Kinds k = new_world(seed);
    Mixed m = { k.w, 0, qk_diffusion_new(n), qk_refine_new(n), qk_abstract_new(n) };
    QwPhen a, wall;
    const int64_t cap = 10000 * 1000;
    OK(qw_phenomenon(k.w, "roomA", &a));
    OK(qw_phenomenon(k.w, "roomB", &m.b));
    OK(qw_phenomenon(k.w, "wall", &wall));
    /* the rooms of phase 5 have no derived cells: an ordinary cell holds the temperature */
    char path[128];
    QnCell e, c, t;
    const char *prefixes[2] = { "roomA", "roomB.agg" };
    OK(qw_model(k.w, m.b, "agg"));
    OK(qw_model(k.w, m.b, "fine"));
    for (int i = 0; i < 2; i++) {
        if (i) OK(qw_model_begin(k.w, m.b, "agg"));
        snprintf(path, sizeof path, "%s.energy", prefixes[i]);
        OK(qw_cell(k.w, path, k.energy, qi_point(cap / 100 * (i ? 29315 : 30315)), &e));
        snprintf(path, sizeof path, "%s.capacity", prefixes[i]);
        OK(qw_cell(k.w, path, k.cap, qi_point(cap), &c));
        snprintf(path, sizeof path, "%s.temperature", prefixes[i]);
        OK(qw_cell(k.w, path, k.temp, qi_top(), &t));
        CHECK(qn_product(qw_net(k.w), e, c, t, NULL) == QN_OK);
        if (i) qw_model_end(k.w);
    }
    OK(qw_model_begin(k.w, m.b, "fine"));
    for (unsigned i = 0; i < n; i++) {
        snprintf(path, sizeof path, "roomB.fine.c%u.energy", i);
        OK(qw_cell(k.w, path, k.energy, qi_point(0), &e));
        snprintf(path, sizeof path, "roomB.fine.c%u.capacity", i);
        OK(qw_cell(k.w, path, k.cap, qi_point(cap / n), &c));
        snprintf(path, sizeof path, "roomB.fine.c%u.temperature", i);
        OK(qw_cell(k.w, path, k.temp, qi_top(), &t));
        CHECK(qn_product(qw_net(k.w), e, c, t, NULL) == QN_OK);
    }
    qw_model_end(k.w);
    OK(qw_cell(k.w, "roomB.fine.conductance", k.cond, qi_point(100 * 1000), NULL));
    OK(qw_cell(k.w, "roomB.fine.tolerance", k.ratio, qi_point(100000000), NULL));
    OK(qw_cell(k.w, "roomB.spread", k.temp, qi_point(500000), NULL));
    OK(qw_port(k.w, m.b, "west", "agg", "roomB.agg"));
    OK(qw_port(k.w, m.b, "west", "fine", "roomB.fine.c0"));
    OK(qw_model_attach(k.w, m.b, "fine", m.diff));
    OK(qw_cell(k.w, "wall.conductance", k.cond, qi_point(10 * 1000), NULL));
    OK(qw_cell(k.w, "wall.tolerance", k.ratio, qi_point(10000000), NULL));
    OK(qw_connect(k.w, wall, "hot", "roomA"));
    OK(qw_connect(k.w, wall, "cold", "roomB.west"));
    OK(qw_attach(k.w, wall, qk_conduction()));
    return m;
}

static void free_mixed(Mixed *m)
{
    qw_free(m->w);
    qk_free(m->diff);
    qk_free(m->refine);
    qk_free(m->abstract);
}

void test_snapshot_models(void)
{
    /* the room is fine from 200 s to 800 s. The snapshot at 500 s has the fine model active */
    Mixed ref = mixed(3, 4), first = mixed(3, 4), second = mixed(3, 4);
    Mixed *all[2] = { &ref, &first };
    for (int i = 0; i < 2; i++) {
        OK(qw_run(all[i]->w, SEC(200)));
        OK(qw_switch(all[i]->w, all[i]->b, "fine", all[i]->refine));
        OK(qw_run(all[i]->w, SEC(500)));
    }
    void *snap = NULL;
    size_t len = 0;
    OK(qw_snapshot(first.w, &snap, &len));
    CHECK(!strcmp(qw_active_model(second.w, second.b), "agg"));
    OK(qw_restore(second.w, snap, len));
    CHECK(!strcmp(qw_active_model(second.w, second.b), "fine"));
    CHECK(qw_hash(second.w) == qw_hash(first.w));
    CHECK_I64(qw_get(second.w, "roomB.agg.energy").lo, 0);                 /* the model that is not active has no stock */
    CHECK(qw_switches(second.w) == 1);
    same_from(ref.w, second.w, 600, 800, 100);
    /* the wall reads the fine model through the port: the switch back works in the restored world */
    OK(qw_switch(ref.w, ref.b, "agg", ref.abstract));
    OK(qw_switch(second.w, second.b, "agg", second.abstract));
    CHECK(qw_hash(ref.w) == qw_hash(second.w));
    same_from(ref.w, second.w, 900, 1000, 100);
    /* the same world goes back from the aggregate model to the fine one */
    OK(qw_run(first.w, SEC(800)));
    OK(qw_switch(first.w, first.b, "agg", first.abstract));
    OK(qw_run(first.w, SEC(1000)));
    CHECK(qw_hash(first.w) == qw_hash(ref.w));
    OK(qw_restore(first.w, snap, len));
    CHECK(!strcmp(qw_active_model(first.w, first.b), "fine"));
    OK(qw_run(first.w, SEC(800)));
    OK(qw_switch(first.w, first.b, "agg", first.abstract));
    OK(qw_run(first.w, SEC(1000)));
    CHECK(qw_hash(first.w) == qw_hash(ref.w));
    free(snap);
    free_mixed(&ref);
    free_mixed(&first);
    free_mixed(&second);
}

/* --- a kernel with private state: native.rate ------------------------------------------------- */

/* A tank with a tap and a leak. The kernel keeps the history of each rate and the cap of its step. */
static QWorld *tank(bool with_save, QkKernel **kernel)
{
    QWorld *w = qw_new(1, -6);
    QwKind volume, flow, time, ratio;
    QwPhen p;
    QnCell v, f, e;
    OK(qw_kind(w, "volume", "m^3", -9, true, &volume));
    OK(qw_kind(w, "flow", "m^3/s", -15, false, &flow));
    OK(qw_kind(w, "time", "s", -6, false, &time));
    OK(qw_kind(w, "ratio", "", -9, false, &ratio));
    OK(qw_phenomenon(w, "tank", &p));
    OK(qw_cell(w, "tank.volume", volume, qi_point(0), &v));
    OK(qw_cell(w, "tank.empties", time, qi_point(SEC(100)), &e));
    OK(qw_cell(w, "tank.inflow", flow, qi_point(2000000000000), NULL));
    OK(qw_derived_cell(w, "tank.outflow", flow, qi_top(), &f));
    OK(qw_cell(w, "tank.tolerance", ratio, qi_point(10000000), NULL));
    CHECK(qn_quotient(qw_net(w), f, v, e, NULL) == QN_OK);
    const QkSlot slots[] = {
        { "inflow", QK_IN, "m^3/s", -15, ANY },
        { "outflow", QK_IN, "m^3/s", -15, ANY },
        { "tolerance", QK_IN, "", -9, { 1, 1000000000 } },
        { "volume", QK_STOCK, "m^3", -9, ANY },
    };
    const QkRateTransfer moves[] = { { "tap", 0, 2, QK_NONE, 3, QK_NONE }, { "leak", 1, 2, 3, QK_NONE, QK_NONE } };
    *kernel = qk_rate_new(slots, 4, moves, 2);
    CHECK(*kernel != NULL);
    if (*kernel) {
        if (!with_save) (*kernel)->save = NULL;
        OK(qw_attach(w, p, *kernel));
    }
    return w;
}

void test_snapshot_private_state(void)
{
    QkKernel *k1, *k2, *k3;
    QWorld *ref = tank(true, &k1), *first = tank(true, &k2), *second = tank(true, &k3);
    /* 130 s: the steps grow, and the kernel has a part of an amount that it keeps for the next step */
    const int stops[] = { 130, 700 };
    for (size_t s = 0; s < 2; s++) {
        void *snap = NULL;
        size_t len = 0;
        OK(qw_run(ref, SEC(stops[s])));
        OK(qw_run(first, SEC(stops[s])));
        OK(qw_snapshot(first, &snap, &len));
        OK(qw_restore(second, snap, len));
        CHECK(qw_hash(second) == qw_hash(first));
        for (int t = stops[s] + 37; t <= stops[s] + 400; t += 121) {
            OK(qw_run(ref, SEC(t)));
            OK(qw_run(first, SEC(t)));             /* each stop is a boundary: the three runs stop at the same times */
            OK(qw_run(second, SEC(t)));
            CHECK(qw_hash(ref) == qw_hash(second));
            CHECK(qw_boundaries(ref) == qw_boundaries(second));
            CHECK_I64(qw_get(ref, "tank.volume").lo, qw_get(second, "tank.volume").lo);
        }
        free(snap);
    }
    /* a kernel with private state and no save function: the world refuses the snapshot */
    QkKernel *k4;
    QWorld *bad = tank(false, &k4);
    void *snap = NULL;
    size_t len = 0;
    OK(qw_run(bad, SEC(10)));
    CHECK(qw_snapshot(bad, &snap, &len) == QW_EKERNEL);
    CHECK(strstr(qw_error(bad), "tank: kernel 'native.rate' has private state and no save function") != NULL);
    CHECK(snap == NULL);
    qw_free(ref);
    qw_free(first);
    qw_free(second);
    qw_free(bad);
    qk_rate_free(k1);
    qk_rate_free(k2);
    qk_rate_free(k3);
    qk_rate_free(k4);
}

/* --- what a restore refuses --------------------------------------------------------------------- */

void test_snapshot_errors(void)
{
    QWorld *w = two_rooms(1), *other = two_rooms(1);
    Mixed m = mixed(1, 4);
    void *snap = NULL;
    size_t len = 0;
    OK(qw_run(w, SEC(100)));
    OK(qw_snapshot(w, &snap, &len));
    uint64_t before = qw_hash(other);
    /* bytes that are not a snapshot */
    CHECK(qw_restore(other, "not a snapshot of a world", 25) == QW_ESNAPSHOT);
    CHECK(strstr(qw_error(other), "the bytes are not a snapshot of a world") != NULL);
    CHECK(qw_restore(other, snap, 4) == QW_ESNAPSHOT);
    CHECK(qw_restore(other, NULL, 0) == QW_ESNAPSHOT);
    /* a world with another structure */
    CHECK(qw_restore(m.w, snap, len) == QW_ESNAPSHOT);
    CHECK(strstr(qw_error(m.w), "the snapshot is of a world with another structure") != NULL);
    /* a snapshot with its end cut, or with bytes after its end */
    CHECK(qw_restore(other, snap, len - 8) == QW_ESNAPSHOT);
    CHECK(strstr(qw_error(other), "the snapshot is cut or damaged") != NULL);
    uint8_t *longer = malloc(len + 8);
    memcpy(longer, snap, len);
    memset(longer + len, 0, 8);
    CHECK(qw_restore(other, longer, len + 8) == QW_ESNAPSHOT);
    free(longer);
    /* a snapshot with a damaged value: the restore refuses it and changes nothing. The layout is 11 words, two
     * words for each of the 8 cells, one word for each group, and then the phenomena */
    size_t cells_at = 11 * 8, phens_at = cells_at + 8 * 16 + qn_group_count(qw_net(other)) * 8;
    /* roomA and roomB have 4 words each. The wall has 4 words, then 5 words of its kernel, then its count of
     * assumptions */
    size_t wall_at = phens_at + 2 * 4 * 8, nas_at = wall_at + 4 * 8 + 5 * 8;
    const struct { size_t at; uint64_t value; } damage[] = {
        { cells_at, (uint64_t) INT64_MAX },            /* a base with its low end above its high end */
        { phens_at, 5 },                               /* a model that roomA does not have */
        { phens_at, 0 },                               /* model 0 of a phenomenon with no models */
        { nas_at, (uint64_t) 1 << 40 },                /* more assumptions than the bytes hold */
        { nas_at, 0 },                                 /* fewer assumptions: the rest does not fit */
        { len - 8, 64 },                               /* a private state that the bytes do not hold */
    };
    for (size_t i = 0; i < sizeof damage / sizeof damage[0]; i++) {
        uint8_t *bad = malloc(len);
        memcpy(bad, snap, len);
        for (int b = 0; b < 8; b++) bad[damage[i].at + (size_t) b] = (uint8_t) (damage[i].value >> (8 * b));
        CHECK(qw_restore(other, bad, len) == QW_ESNAPSHOT);
        CHECK(strstr(qw_error(other), "the snapshot is cut or damaged") != NULL);
        CHECK(qw_hash(other) == before);
        free(bad);
    }
    /* the world still takes the whole snapshot after a refusal */
    OK(qw_restore(other, snap, len));
    CHECK(qw_hash(other) == qw_hash(w) && qw_hash(other) != before);
    /* the bytes of a snapshot are the same for the same state */
    void *again = NULL;
    size_t len2 = 0;
    OK(qw_snapshot(other, &again, &len2));
    CHECK(len2 == len && !memcmp(again, snap, len));
    free(again);
    free(snap);
    CHECK(!strcmp(qw_status_str(QW_ESNAPSHOT), "snapshot does not fit"));
    qw_free(w);
    qw_free(other);
    free_mixed(&m);
}
